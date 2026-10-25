#include "config.h"

#include "test/core/test_view.h"

#include <algorithm>
#include <functional>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <torrent/exceptions.h>
#include <torrent/torrent.h>

#include "command_helpers.h"
#include "control.h"
#include "core/download.h"
#include "core/download_list.h"
#include "core/manager.h"
#include "core/incarnation.h"
#include "core/view.h"
#include "core/view_manager.h"
#include "core/retrackers_migration_pending.h"
#include "globals.h"
#include "rpc/parse_commands.h"
#include "session/thread_session.h"
#include "session/session_manager.h"

CPPUNIT_TEST_SUITE_REGISTRATION(TestView);

void initialize_command_events();
void initialize_command_download();
void initialize_command_local();

namespace {

const char* filter_command  = "test.view.filter";
const char* compare_command = "test.view.compare";
const char* name_command    = "test.view.name";
const char* added_command   = "test.view.event_added";
const char* removed_command = "test.view.event_removed";

std::vector<core::Download*> test_downloads;
std::string test_session_path;

std::set<core::Download*>      matches_filter;
std::map<core::Download*, int> sort_rank;
std::vector<core::Download*>   added_events;
std::vector<core::Download*>   removed_events;

// Each runs once, from the next filter, comparator or event command.
std::function<void(core::Download*)> filter_action;
std::function<void()>                compare_action;
std::function<void(core::Download*)> added_action;
std::function<void(core::Download*)> removed_action;

core::Download*
new_download() {
  torrent::Object info = torrent::Object::create_map();
  info.insert_key("name", "test_view_" + std::to_string(test_downloads.size()));
  info.insert_key("length", int64_t{16});
  info.insert_key("piece length", int64_t{262144});
  info.insert_key("pieces", std::string(20, char(0)));

  auto object = new torrent::Object(torrent::Object::create_map());
  object->insert_key("info", info);
  // These downloads bypass DownloadFactory, so supply the ordinary native
  // metadata that its lifecycle owners read during a real erase.
  auto& rtorrent = object->insert_key("rtorrent", torrent::Object::create_map());
  rtorrent.insert_key("incarnation", core::fresh_incarnation());
  for (const auto* key : {"state", "state_counter", "stop_generation", "hashing", "complete"})
    rtorrent.insert_key(key, int64_t{0});
  for (const auto* key : {"custom1", "custom2", "custom3", "custom4", "custom5", "tied_to_file"})
    rtorrent.insert_key(key, std::string());
  rtorrent.insert_key("custom", torrent::Object::create_map());
  rtorrent.insert_key("views", torrent::Object::create_list());

  auto download = new core::Download(torrent::download_add(object, 0));
  test_downloads.push_back(download);

  return download;
}

// A download only the test holds, for a view outside the view manager.
std::shared_ptr<core::Download>
make_download() {
  return std::shared_ptr<core::Download>(new_download(), [](core::Download* download) {
      torrent::download_remove(*download->download());
      delete download;
    });
}

// A download owned by the download list, and so held by every managed view.
core::Download*
insert_download() {
  auto download = new_download();
  control->core()->download_list()->insert(download);
  return download;
}

core::View*
insert_view(const std::string& name) {
  return *control->view_manager()->insert(name);
}

// Renders a dispatch list as "d0,d2" so a failure names the downloads the
// handler saw, not just how many there were.
std::string
describe(const std::vector<core::Download*>& downloads) {
  std::string result;

  for (const auto& download : downloads) {
    auto itr = std::find(test_downloads.begin(), test_downloads.end(), download);

    if (!result.empty())
      result += ',';

    result += 'd' + std::to_string(std::distance(test_downloads.begin(), itr));
  }

  return result;
}

std::string
describe_visible(const core::View& view) {
  std::vector<core::Download*> downloads;

  for (auto itr = view.begin_visible(); itr != view.end_visible(); ++itr)
    downloads.push_back(itr->get());

  return describe(downloads);
}

// Returns '1' when the view lock refused the action, '0' when it ran.
char
refused(const std::function<void()>& action) {
  try {
    action();
  } catch (const torrent::input_error& e) {
    CPPUNIT_ASSERT_EQUAL(std::string("View filter and sort commands cannot change views or downloads."), std::string(e.what()));
    return '1';
  }

  return '0';
}

torrent::Object
cmd_filter(core::Download* download, const torrent::Object&) {
  if (filter_action)
    std::exchange(filter_action, {})(download);

  return torrent::Object(static_cast<int64_t>(matches_filter.count(download)));
}

torrent::Object
cmd_compare(rpc::target_type target, const torrent::Object&) {
  if (compare_action)
    std::exchange(compare_action, {})();

  auto left  = static_cast<core::Download*>(target.second);
  auto right = static_cast<core::Download*>(target.third);

  return torrent::Object(static_cast<int64_t>(sort_rank[left] < sort_rank[right]));
}

// Reads the target, as a typical filter condition does.
torrent::Object
cmd_name(core::Download* download, const torrent::Object&) {
  return torrent::Object(std::string(download->info()->name().c_str()));
}

torrent::Object
cmd_event_added(core::Download* download, const torrent::Object&) {
  added_events.push_back(download);

  if (added_action)
    std::exchange(added_action, {})(download);

  return torrent::Object();
}

torrent::Object
cmd_event_removed(core::Download* download, const torrent::Object&) {
  removed_events.push_back(download);

  if (removed_action)
    std::exchange(removed_action, {})(download);

  return torrent::Object();
}

torrent::Object
command_object(const char* command) {
  return torrent::Object(std::string(command) + "=");
}

} // namespace

void
TestView::setUp() {
  test_fixture::setUp();
  mock_redirect_defaults();

  // Torrent-backed downloads and native erase use their real lifecycle owners.
  torrent::initialize_main_thread();
  torrent::initialize();
  session::ThreadSession::create_thread();

  if (control == nullptr)
    control = new Control;

  // Native erase owns session/Stop/retrackers cleanup; register its real commands.
  if (!rpc::commands.has("session.path"))
    initialize_command_local();
  if (!rpc::commands.has("d.hash"))
    initialize_command_download();

  if (!rpc::commands.has("d.multicall"))
    initialize_command_events();

  if (!rpc::commands.has(filter_command)) {
    CMD2_DL(filter_command, &cmd_filter);
    CMD2_ANY(compare_command, &cmd_compare);
    CMD2_DL(name_command, &cmd_name);
    CMD2_DL(added_command, &cmd_event_added);
    CMD2_DL(removed_command, &cmd_event_removed);
  }

  char temporary[] = "/tmp/rtorrent_test_view_XXXXXX";
  CPPUNIT_ASSERT(mkdtemp(temporary) != nullptr);
  test_session_path = temporary;
  session_thread::manager()->set_path(test_session_path);
  session_thread::thread()->start_thread();

  test_downloads.clear();
  matches_filter.clear();
  sort_rank.clear();
  added_events.clear();
  removed_events.clear();

  filter_action  = {};
  compare_action = {};
  added_action   = {};
  removed_action = {};
}

void
TestView::tearDown() {
  control->core()->download_list()->clear();
  control->view_manager()->clear();

  retrackers_migration_flush_erased_owners();
  session_thread::thread()->stop_thread_wait();
  session::ThreadSession::destroy_thread();
  torrent::cleanup();
  std::filesystem::remove_all(test_session_path);
  test_session_path.clear();

  test_fixture::tearDown();
}

// d0 and d1 start visible and stop matching, d2 and d3 start filtered out and
// start matching. The two halves are the same size on purpose: a test that
// only counted the dispatches would pass even if both events were sent over
// the same half of the changed range.
void
TestView::test_filter_dispatches_events_to_the_right_downloads() {
  core::View view;
  view.initialize("test_view");

  auto d0 = make_download();
  auto d1 = make_download();
  auto d2 = make_download();
  auto d3 = make_download();

  view.insert(d0);
  view.insert(d1);
  view.insert(d2);
  view.insert(d3);

  view.set_visible(d0.get());
  view.set_visible(d1.get());

  CPPUNIT_ASSERT_EQUAL(core::View::size_type(2), view.size_visible());

  matches_filter = {d2.get(), d3.get()};

  view.set_filter(command_object(filter_command));
  view.set_event_added(command_object(added_command));
  view.set_event_removed(command_object(removed_command));

  added_events.clear();
  removed_events.clear();

  view.filter();

  CPPUNIT_ASSERT_EQUAL(std::string("d2,d3"), describe(added_events));
  CPPUNIT_ASSERT_EQUAL(std::string("d0,d1"), describe(removed_events));

  CPPUNIT_ASSERT_EQUAL(core::View::size_type(2), view.size_visible());
  CPPUNIT_ASSERT(*view.begin_visible() == d2);
  CPPUNIT_ASSERT(*(view.begin_visible() + 1) == d3);
}

// Sort, filter, sorted insertion and erase without side effects in the
// commands behave as before.
void
TestView::test_sort_filter_and_erase() {
  auto d0 = insert_download();
  auto d1 = insert_download();
  auto d2 = insert_download();
  auto view = insert_view("test_view");

  sort_rank = {{d0, 2}, {d1, 0}, {d2, 1}};

  view->set_sort_current(command_object(compare_command));
  view->set_sort_new(command_object(compare_command));
  view->sort();

  CPPUNIT_ASSERT_EQUAL(std::string("d1,d2,d0"), describe_visible(*view));

  matches_filter = {d0, d2};

  view->set_filter(command_object(filter_command));
  view->set_event_added(command_object(added_command));
  view->set_event_removed(command_object(removed_command));
  view->filter();

  CPPUNIT_ASSERT_EQUAL(std::string("d2,d0"), describe_visible(*view));
  CPPUNIT_ASSERT_EQUAL(std::string("d1"), describe(removed_events));

  matches_filter.insert(d1);
  view->filter_download(d1);

  CPPUNIT_ASSERT_EQUAL(std::string("d1,d2,d0"), describe_visible(*view));
  CPPUNIT_ASSERT_EQUAL(std::string("d1"), describe(added_events));

  control->core()->download_list()->erase_ptr(d2);

  CPPUNIT_ASSERT_EQUAL(std::string("d1,d0"), describe_visible(*view));
  CPPUNIT_ASSERT_EQUAL(std::string("d1,d2"), describe(removed_events));
  CPPUNIT_ASSERT_EQUAL(size_t(2), control->core()->download_list()->size());
}

void
TestView::test_sort_refuses_view_changes() {
  auto d0 = insert_download();
  auto d1 = insert_download();
  auto other = insert_view("test_other");

  core::View view;
  view.initialize("test_view");

  sort_rank = {{d0, 1}, {d1, 0}};

  std::string refusals;

  compare_action = [&]() {
    refusals += refused([&] { control->view_manager()->insert("test_added"); });
    refusals += refused([&] { control->core()->download_list()->erase_ptr(d0); });
    refusals += refused([&] { other->set_not_visible(d0); });
    refusals += refused([&] { other->filter(); });
    refusals += refused([&] { view.sort(); });
    refusals += refused([&] { view.set_sort_current(torrent::Object()); });
  };

  view.set_sort_current(command_object(compare_command));
  view.sort();

  CPPUNIT_ASSERT_EQUAL(std::string("111111"), refusals);
  CPPUNIT_ASSERT_EQUAL(std::string("d1,d0"), describe_visible(view));
  CPPUNIT_ASSERT_EQUAL(std::string("d0,d1"), describe_visible(*other));
  CPPUNIT_ASSERT_EQUAL(size_t(1), control->view_manager()->size());
  CPPUNIT_ASSERT_EQUAL(size_t(2), control->core()->download_list()->size());
}

void
TestView::test_filter_refuses_view_changes() {
  auto d0 = insert_download();
  auto d1 = insert_download();

  core::View view;
  view.initialize("test_view");

  matches_filter = {d0, d1};

  std::string refusals;

  filter_action = [&](core::Download*) {
    refusals += refused([&] { control->core()->download_list()->erase_ptr(d1); });
    refusals += refused([&] { control->core()->download_list()->close_throw(d1); });
    refusals += refused([&] { control->core()->download_list()->close_directly(d1); });
    refusals += refused([&] { control->core()->download_list()->check_hash(d1); });
    refusals += refused([&] { control->core()->download_list()->pause(d1); });
    refusals += refused([&] { control->core()->download_list()->resume(d1); });
    refusals += refused([&] { control->view_manager()->insert("test_added"); });
    refusals += refused([&] { view.filter_download(d1); });
    refusals += refused([&] { view.set_visible(d1); });
    refusals += refused([&] { view.set_filter(torrent::Object()); });
    refusals += refused([&] { view.set_filter_temp(torrent::Object()); });
  };

  view.set_filter(command_object(filter_command));
  view.filter();

  CPPUNIT_ASSERT_EQUAL(std::string("11111111111"), refusals);
  CPPUNIT_ASSERT_EQUAL(std::string("d0,d1"), describe_visible(view));
  CPPUNIT_ASSERT_EQUAL(size_t(0), control->view_manager()->size());
  CPPUNIT_ASSERT_EQUAL(size_t(2), control->core()->download_list()->size());
}

// The insertion comparator of set_visible() and filter_download() runs
// locked as well.
void
TestView::test_insert_refuses_view_changes() {
  auto d0 = insert_download();
  auto d1 = insert_download();

  core::View view;
  view.initialize("test_view");
  view.set_not_visible(d1);

  sort_rank = {{d0, 1}, {d1, 0}};

  std::string refusals;

  compare_action = [&]() {
    refusals += refused([&] { control->core()->download_list()->erase_ptr(d0); });
    refusals += refused([&] { control->view_manager()->insert("test_added"); });
    refusals += refused([&] { view.set_not_visible(d0); });
    refusals += refused([&] { view.set_sort_new(torrent::Object()); });
  };

  view.set_sort_new(command_object(compare_command));
  view.set_visible(d1);

  CPPUNIT_ASSERT_EQUAL(std::string("1111"), refusals);
  CPPUNIT_ASSERT_EQUAL(std::string("d1,d0"), describe_visible(view));
  CPPUNIT_ASSERT_EQUAL(size_t(2), control->core()->download_list()->size());
}

// Events run after the filter commands, so a handler may change downloads and
// views, as 'd.stop' does through 'view.set_visible=stopped'. A download an
// earlier handler erased gets no event.
void
TestView::test_filter_events_may_change_views() {
  auto d0 = insert_download();
  auto d1 = insert_download();
  auto d2 = insert_download();
  auto view    = insert_view("test_view");
  auto stopped = insert_view("test_stopped");

  for (auto download : {d0, d1, d2})
    stopped->set_not_visible(download);

  view->set_filter(command_object(filter_command));
  view->set_event_removed(command_object(removed_command));

  removed_action = [&](core::Download* download) {
    stopped->set_visible(download);
    view->filter_download(download);
    control->core()->download_list()->erase_ptr(d1);
  };

  view->filter();

  CPPUNIT_ASSERT_EQUAL(std::string("d0,d2"), describe(removed_events));
  CPPUNIT_ASSERT_EQUAL(std::string("d0"), describe_visible(*stopped));
  CPPUNIT_ASSERT_EQUAL(core::View::size_type(0), view->size_visible());
  CPPUNIT_ASSERT_EQUAL(size_t(2), control->core()->download_list()->size());
}

// Native erase events may add views. Existing views are visited through stable
// pointers; newly created views must omit the download being erased.
void
TestView::test_erase_event_adds_views() {
  auto d0 = insert_download();
  insert_download();
  auto view = insert_view("test_view");

  view->set_event_removed(command_object(removed_command));
  removed_action = [](core::Download*) {
    for (int i = 0; i != 128; ++i)
      control->view_manager()->insert("test_added_" + std::to_string(i));
  };

  control->core()->download_list()->erase_ptr(d0);

  CPPUNIT_ASSERT_EQUAL(size_t(1), control->core()->download_list()->size());
  CPPUNIT_ASSERT_EQUAL(size_t(129), control->view_manager()->size());
  for (auto item : *control->view_manager())
    CPPUNIT_ASSERT_EQUAL(std::string("d1"), describe_visible(*item));

  auto d2 = insert_download();
  for (auto item : *control->view_manager())
    CPPUNIT_ASSERT_EQUAL(std::string("d1,d2"), describe_visible(*item));
  control->core()->download_list()->erase_ptr(d2);
  for (auto item : *control->view_manager())
    CPPUNIT_ASSERT_EQUAL(std::string("d1"), describe_visible(*item));
}

// Held recovery bypasses user code while making the download visible in main.
// Releasing the hold initializes every view exactly once through the owner.
void
TestView::test_held_insertion_defers_view_events() {
  auto main = insert_view("main");
  auto other = insert_view("test_other");
  other->set_event_added(command_object(added_command));

  auto download = new_download();
  auto list = control->core()->download_list();
  CPPUNIT_ASSERT(list->insert(download, true) != list->end());
  CPPUNIT_ASSERT(download->insertion_deferred());
  CPPUNIT_ASSERT_EQUAL(std::string("d0"), describe_visible(*main));
  CPPUNIT_ASSERT_EQUAL(std::string(), describe_visible(*other));
  CPPUNIT_ASSERT(added_events.empty());

  list->release_held_insertion(download, false);
  CPPUNIT_ASSERT(!download->insertion_deferred());
  CPPUNIT_ASSERT_EQUAL(std::string("d0"), describe_visible(*main));
  CPPUNIT_ASSERT_EQUAL(std::string("d0"), describe_visible(*other));
  CPPUNIT_ASSERT_EQUAL(std::string("d0"), describe(added_events));
}

// An event of the first view erases the download while DownloadList::insert()
// still has views left to filter it in.
void
TestView::test_insert_stops_at_erased_download() {
  auto first = insert_view("test_first");
  insert_view("test_second");

  first->set_event_added(command_object(added_command));

  added_action = [](core::Download* download) { control->core()->download_list()->erase_ptr(download); };

  auto list = control->core()->download_list();

  CPPUNIT_ASSERT(list->insert(new_download()) == list->end());
  CPPUNIT_ASSERT_EQUAL(std::string("d0"), describe(added_events));
  CPPUNIT_ASSERT_EQUAL(size_t(0), list->size());
}

// A comparator may read the view it sorts, while std::stable_sort holds one of
// its entries outside and leaves a moved-from one in its place. The commands
// that read a view skip that entry, also when no command would use it.
void
TestView::test_multicall_skips_moved_entries() {
  auto d0 = insert_download();
  auto d1 = insert_download();
  auto d2 = insert_download();
  auto view = insert_view("test_view");

  sort_rank = {{d0, 0}, {d1, 1}, {d2, 2}};
  matches_filter = {nullptr, d0, d1, d2};

  torrent::Object rows;
  torrent::Object filtered_rows;
  torrent::Object named_rows;
  torrent::Object hashes;

  // The insertion sort moves d1 out after the first comparison returns false.
  compare_action = [&]() {
    compare_action = [&]() {
      auto args = torrent::Object::create_list();
      args.as_list().push_back(std::string("test_view"));
      rows = rpc::call_command("d.multicall", args);

      hashes = rpc::call_command("download_list", args);

      args.as_list().push_back(command_object(filter_command));
      filtered_rows = rpc::call_command("d.multicall.filtered", args);

      args.as_list().back() = command_object(name_command);
      named_rows = rpc::call_command("d.multicall.filtered", args);
    };
  };

  view->set_sort_current(command_object(compare_command));
  view->sort();

  CPPUNIT_ASSERT(rows.is_list() && filtered_rows.is_list() && named_rows.is_list() && hashes.is_list());
  CPPUNIT_ASSERT_EQUAL(size_t(2), rows.as_list().size());
  CPPUNIT_ASSERT_EQUAL(size_t(2), filtered_rows.as_list().size());
  CPPUNIT_ASSERT_EQUAL(size_t(2), named_rows.as_list().size());
  CPPUNIT_ASSERT_EQUAL(size_t(2), hashes.as_list().size());
  CPPUNIT_ASSERT_EQUAL(std::string("d0,d1,d2"), describe_visible(*view));
}
