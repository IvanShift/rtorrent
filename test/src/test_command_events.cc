#include "config.h"

#include "test/src/test_command_events.h"

#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <torrent/download_info.h>
#include <torrent/object.h>
#include <torrent/torrent.h>
#include <vector>

#include "command_helpers.h"
#include "control.h"
#include "core/download.h"
#include "core/download_list.h"
#include "core/incarnation.h"
#include "core/manager.h"
#include "core/retrackers_migration_pending.h"
#include "core/view.h"
#include "core/view_manager.h"
#include "globals.h"
#include "rpc/parse_commands.h"
#include "session/session_manager.h"
#include "session/thread_session.h"

CPPUNIT_TEST_SUITE_REGISTRATION(TestCommandEvents);

void initialize_command_download();
void initialize_command_events();
void initialize_command_local();
void initialize_command_ui();

namespace {

const char*                                             event_command = "test.command_events.event";
const char*                                             existing_file = ".";
const char*                                             missing_file  = "test_command_events.missing";

std::string                                             handled;
std::string                                             session_path;

std::map<core::Download*, std::vector<core::Download*>> erase_on;

torrent::Object
cmd_event(core::Download* download, const torrent::Object&) {
  if (download->is_erasing())
    return torrent::Object();

  if (!handled.empty())
    handled += ',';

  handled += download->info()->name().c_str();

  for (auto target : erase_on[download])
    control->core()->download_list()->erase_ptr(target);

  return torrent::Object();
}

std::vector<core::Download*>
insert_downloads(int64_t state, const std::string& tied_to_file) {
  std::vector<core::Download*> downloads;

  for (int i = 0; i < 5; i++) {
    torrent::Object info = torrent::Object::create_map();
    info.insert_key("name", "d" + std::to_string(i));
    info.insert_key("length", int64_t{16});
    info.insert_key("piece length", int64_t{262144});
    info.insert_key("pieces", std::string(20, char(0)));

    torrent::Object rtorrent = torrent::Object::create_map();
    rtorrent.insert_key("state", state);
    rtorrent.insert_key("tied_to_file", tied_to_file);
    rtorrent.insert_key("ignore_commands", int64_t{0});
    rtorrent.insert_key("hashing", int64_t{1});
    // DownloadFactory gives every download one; the native erase reads it.
    rtorrent.insert_key("incarnation", core::fresh_incarnation());

    auto object = new torrent::Object(torrent::Object::create_map());
    object->insert_key("info", info);
    object->insert_key("rtorrent", rtorrent);

    auto download = new core::Download(torrent::download_add(object, 0));
    control->core()->download_list()->insert(download);
    downloads.push_back(download);
  }

  return downloads;
}

core::View*
insert_hidden_view(const std::string& name, const std::vector<core::Download*>& downloads) {
  auto view = *control->view_manager()->insert(name);

  for (auto download : downloads)
    view->set_not_visible(download);

  return view;
}

void
check_erasing_handlers(const char* command, const std::vector<core::Download*>& d) {
  erase_on = {{d[0], {d[0]}}, {d[1], {d[2], d[4]}}};

  rpc::call_command(command, torrent::Object());

  CPPUNIT_ASSERT_EQUAL(std::string("d0"), handled);

  rpc::call_command(command, torrent::Object());

  CPPUNIT_ASSERT_EQUAL(std::string("d0,d1,d3"), handled);
  CPPUNIT_ASSERT_EQUAL(size_t(2), control->core()->download_list()->size());
}

} // namespace

void
TestCommandEvents::setUp() {
  mock_redirect_defaults();

  torrent::initialize_main_thread();
  torrent::initialize();
  session::ThreadSession::create_thread();

  if (control == nullptr)
    control = new Control;

  // The native erase keeps its records in the session directory.
  if (!rpc::commands.has("session.path"))
    initialize_command_local();

  if (!rpc::commands.has("d.name"))
    initialize_command_download();

  if (!rpc::commands.has("view.set_visible"))
    initialize_command_ui();

  if (!rpc::commands.has("start_tied"))
    initialize_command_events();

  if (!rpc::commands.has(event_command)) {
    CMD2_DL(event_command, &cmd_event);
    CMD2_DL("event.download.hash_removed", &cmd_event);
  }

  char temporary[] = "/tmp/rtorrent_test_command_events_XXXXXX";
  CPPUNIT_ASSERT(mkdtemp(temporary) != nullptr);
  session_path = temporary;
  session_thread::manager()->set_path(session_path);
  session_thread::thread()->start_thread();

  handled.clear();
  erase_on.clear();
}

void
TestCommandEvents::tearDown() {
  erase_on.clear();

  control->core()->download_list()->clear();
  control->view_manager()->clear();

  // Retire the erased owners now rather than from a scheduled task that
  // outlives the session directory.
  retrackers_migration_flush_erased_owners();
  session_thread::thread()->stop_thread_wait();
  session::ThreadSession::destroy_thread();
  torrent::cleanup();
  std::filesystem::remove_all(session_path);
}

void
TestCommandEvents::test_start_tied_with_erasing_handlers() {
  auto downloads = insert_downloads(0, existing_file);

  insert_hidden_view("started", downloads)->set_event_added(torrent::Object(std::string(event_command) + "="));

  check_erasing_handlers("start_tied", downloads);
}

void
TestCommandEvents::test_stop_untied_with_erasing_handlers() {
  auto downloads = insert_downloads(1, missing_file);

  insert_hidden_view("stopped", downloads)->set_event_added(torrent::Object(std::string(event_command) + "="));

  check_erasing_handlers("stop_untied", downloads);
}

void
TestCommandEvents::test_close_untied_with_erasing_handlers() {
  auto downloads = insert_downloads(1, missing_file);

  insert_hidden_view("stopped", downloads);
  control->view_manager()->insert("active");

  check_erasing_handlers("close_untied", downloads);
}
