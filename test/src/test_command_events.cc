#include "config.h"

#include "test/src/test_command_events.h"

#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <torrent/data/download_data.h>
#include <torrent/data/file.h>
#include <torrent/data/file_list.h>
#include <torrent/data/file_manager.h>
#include <torrent/download_info.h>
#include <torrent/object.h>

#include <torrent/torrent.h>
#include <vector>

#include <torrent/system/scheduler.h>

#include "command_helpers.h"
#include "control.h"
#include "core/download.h"
#include "core/download_list.h"
#include "core/erasedata_intent.h"
#include "core/incarnation.h"
#include "core/manager.h"
#include "core/retrackers_migration_pending.h"
#include "core/session_path.h"
#include "core/view.h"
#include "core/view_manager.h"
#include "globals.h"
#include "rpc/parse_commands.h"
#include "session/session_manager.h"
#include "session/thread_session.h"
#include "utils/functional.h"

CPPUNIT_TEST_SUITE_REGISTRATION(TestCommandEvents);

void initialize_command_download();
void initialize_command_events();
void initialize_command_local();
void initialize_command_scheduler();
void initialize_command_system();
void initialize_command_ui();

namespace {

// Expose the existing scheduler operation as a base-class member pointer.
struct TestSchedulerAccess : torrent::system::Scheduler {
  using torrent::system::Scheduler::perform;
};

const char*                                             event_command = "test.command_events.event";
const char*                                             existing_file = ".";
const char*                                             missing_file  = "test_command_events.missing";

std::string                                             handled;
std::string                                             session_path;
int                                                     hash_failed_events;

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

torrent::Object
cmd_hash_failed(core::Download*, const torrent::Object&) {
  hash_failed_events++;
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

int64_t
write_payload(core::Download* download, int64_t completed) {
  const auto path = session_path + "/" + download->info()->name().c_str();
  std::ofstream(path) << std::string(16, 'x');

  struct stat file_stat{};
  CPPUNIT_ASSERT(stat(path.c_str(), &file_stat) == 0);

  download->file_list()->set_root_dir(session_path);

  auto& resume = download->download()->bencode()->insert_key("libtorrent_resume", torrent::Object::create_map());
  resume.insert_key("bitfield", int64_t{1});
  auto& entry = resume.insert_key("files", torrent::Object::create_list()).as_list().emplace_back(torrent::Object::create_map());
  entry.insert_key("completed", completed);
  entry.insert_key("mtime", static_cast<int64_t>(file_stat.st_mtime));

  auto& rtorrent = download->download()->bencode()->get_key("rtorrent");
  rtorrent.insert_key("complete", int64_t{0});
  rtorrent.insert_key("state_changed", int64_t{1});
  rtorrent.insert_key("state_counter", int64_t{0});
  rtorrent.insert_key("priority", int64_t{2});
  rtorrent.insert_key("connection_seed", std::string("seed"));
  rtorrent.insert_key("choke_heuristics.up.seed", std::string("upload_leech"));
  rtorrent.insert_key("choke_heuristics.down.seed", std::string("download_leech"));

  return file_stat.st_mtime;
}

torrent::Object
check_then_touch(core::Download* download, bool quick_hashed, int64_t completed) {
  const auto mtime = write_payload(download, completed);

  download->download()->open();
  download->download()->set_bitfield(true);
  download->file_list()->front()->unset_flags(torrent::File::flag_create_queued | torrent::File::flag_resize_queued);

  download->set_quick_hashed(quick_hashed);
  CPPUNIT_ASSERT(download->download()->hash_check(true));

  const timespec touched[2] = { { mtime + 86400, 0 }, { mtime + 86400, 0 } };
  CPPUNIT_ASSERT(utimensat(AT_FDCWD, (session_path + "/" + download->info()->name().c_str()).c_str(), touched, 0) == 0);

  (torrent::this_thread::scheduler()->*&TestSchedulerAccess::perform)(torrent::this_thread::cached_time());
  CPPUNIT_ASSERT_EQUAL(int64_t{0}, rpc::call_command_value("d.hashing", rpc::make_target(download)));

  auto saved = download->download()->bencode()->get_key("libtorrent_resume").get_key_list("files").front();
  saved.insert_key("mtime", saved.get_key_value("mtime") - mtime);
  return saved;
}

void
walk_hashing(core::Download* download) {
  auto view = control->core()->hashing_view();

  view->insert(*control->core()->download_list()->find(download->info()->hash()));
  view->set_visible(download);

  for (auto& slot : view->signal_changed())
    slot();
}

void
finish_hashing(std::initializer_list<core::Download*> downloads) {
  for (auto download : downloads)
    control->core()->hashing_view()->erase(download);

  (torrent::this_thread::scheduler()->*&TestSchedulerAccess::perform)(torrent::this_thread::cached_time());
}

void
fail_hash_check(core::Download* download) {
  auto hash_done = download->data()->slot_initial_hash();
  hash_done();
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

  if (control->core()->hashing_view() == nullptr) {
    auto view = new core::View;
    view->initialize("test.hashing");
    control->core()->set_hashing_view(view);
  }

  // The native erase keeps its records in the session directory.
  if (!rpc::commands.has("session.path"))
    initialize_command_local();

  if (!rpc::commands.has("d.name"))
    initialize_command_download();

  if (!rpc::commands.has("view.set_visible"))
    initialize_command_ui();

  if (!rpc::commands.has("start_tied"))
    initialize_command_events();

  if (!rpc::commands.has("scheduler.max_active"))
    initialize_command_scheduler();

  if (!rpc::commands.has("system.sockets.size"))
    initialize_command_system();

  if (!rpc::commands.has(event_command)) {
    CMD2_DL(event_command, &cmd_event);
    CMD2_DL("event.download.hash_removed", &cmd_event);
    CMD2_DL("event.download.hash_failed", &cmd_hash_failed);
    CMD2_DL("event.download.finished", &cmd_event);
  }

  char temporary[] = "/tmp/rtorrent_test_command_events_XXXXXX";
  CPPUNIT_ASSERT(mkdtemp(temporary) != nullptr);
  session_path = temporary;
  session_thread::manager()->set_path(session_path);
  session_thread::thread()->start_thread();

  handled.clear();
  hash_failed_events = 0;
  erase_on.clear();
}

void
TestCommandEvents::tearDown() {
  erase_on.clear();

  auto hashing = control->core()->hashing_view();
  while (hashing->begin_visible() != hashing->end_filtered())
    hashing->erase(hashing->begin_visible()->get());

  control->core()->download_list()->clear();
  control->view_manager()->clear();
  (torrent::this_thread::scheduler()->*&TestSchedulerAccess::perform)(torrent::this_thread::cached_time() + std::chrono::seconds(1));

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

void
TestCommandEvents::test_hash_failure_releases_active() {
  auto downloads = insert_downloads(1, missing_file);
  auto active    = *control->view_manager()->insert("active");

  fail_hash_check(downloads[0]);

  CPPUNIT_ASSERT_EQUAL(1, hash_failed_events);
  CPPUNIT_ASSERT_EQUAL(size_t(4), active->size());
}

void
TestCommandEvents::test_hash_failure_lets_scheduler_start_another() {
  auto downloads = insert_downloads(1, missing_file);
  auto active    = insert_hidden_view("active", {downloads[1], downloads[2], downloads[3], downloads[4]});

  control->view_manager()->insert("started");
  active->set_event_removed(torrent::Object("scheduler.simple.update="));
  rpc::call_command_set_value("scheduler.max_active.set", 1);

  fail_hash_check(downloads[0]);

  CPPUNIT_ASSERT_EQUAL(size_t(1), active->size());
  CPPUNIT_ASSERT_EQUAL(downloads[1], active->begin_visible()->get());
}

void
TestCommandEvents::test_hash_failure_with_erasing_handler() {
  auto downloads = insert_downloads(1, missing_file);
  auto active    = *control->view_manager()->insert("active");

  active->set_event_removed(torrent::Object(std::string(event_command) + "="));
  erase_on = {{downloads[0], {downloads[0]}}};

  fail_hash_check(downloads[0]);

  CPPUNIT_ASSERT_EQUAL(0, hash_failed_events);
  CPPUNIT_ASSERT_EQUAL(size_t(4), control->core()->download_list()->size());
}

void
TestCommandEvents::test_hash_failure_preserves_erasedata_refusal() {
  auto downloads = insert_downloads(1, missing_file);
  auto download = downloads[0];
  const auto hash = rpc::call_command_string("d.hash", rpc::make_target(download));
  std::filesystem::create_directory(session_path + "/.rutorrent-stop-intents");
  std::filesystem::permissions(session_path + "/.rutorrent-stop-intents", std::filesystem::perms::owner_all);
  const auto path = session_path + "/.rutorrent-stop-intents/" + hash + ".erasedata";
  const core::erasedata_intent::Record record{hash, std::string(32, 'a'), std::string(16, '1'),
                                             "test", std::string(32, 'b'), std::string(64, 'c'), 'P'};
  std::ofstream(path) << core::erasedata_intent::format(record);
  std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
  download->set_hash_failed(true);

  control->core()->download_list()->resume(download, 0);
  CPPUNIT_ASSERT_MESSAGE(download->message(), download->message().find("erasedata: recovery held: ") == 0);
  CPPUNIT_ASSERT(download->message().find(hash + ".erasedata") != std::string::npos);

  std::ofstream(path) << "unreadable\n";
  download->set_message("");
  control->core()->download_list()->resume(download, 0);
  CPPUNIT_ASSERT_MESSAGE(download->message(), download->message().find("erasedata: malformed-record; inspect ") == 0);
  CPPUNIT_ASSERT(download->message().find(hash + ".erasedata") != std::string::npos);
  std::filesystem::remove(path);
}

void
TestCommandEvents::test_hash_done_skips_unchanged_resume_save() {
  auto downloads = insert_downloads(0, missing_file);

  auto unchanged = check_then_touch(downloads[0], true, 1);
  CPPUNIT_ASSERT_EQUAL(int64_t{0}, unchanged.get_key_value("mtime"));
  CPPUNIT_ASSERT_EQUAL(int64_t{1}, unchanged.get_key_value("completed"));

  auto full_check = check_then_touch(downloads[1], false, 1);
  CPPUNIT_ASSERT_EQUAL(int64_t{86400}, full_check.get_key_value("mtime"));

  auto changed = check_then_touch(downloads[2], true, 0);
  CPPUNIT_ASSERT_EQUAL(int64_t{86400}, changed.get_key_value("mtime"));
  CPPUNIT_ASSERT_EQUAL(int64_t{1}, changed.get_key_value("completed"));
}

void
TestCommandEvents::test_hash_failure_keeps_deferred_completion() {
  auto downloads = insert_downloads(1, missing_file);
  auto download = downloads[0];
  auto active = insert_hidden_view("active", {download});
  download->file_list()->set_root_dir(session_path);
  download->download()->open();
  download->download()->set_bitfield(true);
  auto saved_callback = download->data()->slot_initial_hash();
  download->data()->slot_initial_hash() = [] {};
  CPPUNIT_ASSERT(download->download()->hash_check(true));
  auto perform = &TestSchedulerAccess::perform;
  static_assert(std::is_same_v<decltype(perform), void (torrent::system::Scheduler::*)(torrent::system::Scheduler::time_type)>);
  (torrent::this_thread::scheduler()->*perform)(torrent::this_thread::cached_time());
  download->data()->slot_initial_hash() = saved_callback;
  CPPUNIT_ASSERT(download->is_hash_checked());
  CPPUNIT_ASSERT(download->is_done());
  rpc::call_command("d.hashing.set", int64_t{0}, rpc::make_target(download));
  download->set_completion_deferred(true);
  download->set_hash_failed(true);
  download->set_resume_flags(42);
  handled.clear();
  const auto visible = active->size();

  control->core()->download_list()->resume(download, 0);

  CPPUNIT_ASSERT(download->completion_deferred());
  CPPUNIT_ASSERT(handled.empty());
  CPPUNIT_ASSERT_EQUAL(visible, active->size());
  CPPUNIT_ASSERT_EQUAL(uint32_t{42}, download->resume_flags());
}

void
TestCommandEvents::test_walk_waits_for_quick_checked_download() {
  auto downloads = insert_downloads(0, missing_file);

  write_payload(downloads[0], 1);
  write_payload(downloads[1], 1);
  downloads[0]->download()->open();
  downloads[0]->download()->set_bitfield(true);
  CPPUNIT_ASSERT(downloads[0]->download()->hash_check(true));
  downloads[0]->set_quick_hashed(true);

  walk_hashing(downloads[0]);
  walk_hashing(downloads[1]);

  CPPUNIT_ASSERT(!downloads[1]->is_open());
  finish_hashing({ downloads[0], downloads[1] });
}

void
TestCommandEvents::test_hash_done_keeps_files_the_walk_opened() {
  control->view_manager()->insert("active");
  auto downloads = insert_downloads(1, missing_file);

  write_payload(downloads[0], 1);
  write_payload(downloads[1], 1);

  auto opened = torrent::file_manager()->files_opened_counter();
  walk_hashing(downloads[0]);
  finish_hashing({ downloads[0] });

  CPPUNIT_ASSERT(downloads[0]->is_active());
  CPPUNIT_ASSERT_EQUAL(opened + 1, torrent::file_manager()->files_opened_counter());

  downloads[1]->download()->open();
  opened = torrent::file_manager()->files_opened_counter();
  walk_hashing(downloads[1]);
  finish_hashing({ downloads[1] });

  CPPUNIT_ASSERT(downloads[1]->is_active());
  CPPUNIT_ASSERT_EQUAL(opened + 1, torrent::file_manager()->files_opened_counter());
}

void
TestCommandEvents::test_startup_record_guards() {
  auto core    = control->core();
  auto written = core->m_startup_written;

  utils::scope_guard restore([core, written]() {
    torrent::this_thread::scheduler()->erase(&core->m_task_hashing_slice);
    control->m_shutdown_received = false;
    control->m_shutdown_quick    = false;
    core->m_startup_written      = written;
  });

  core->m_startup_written = false;

  torrent::this_thread::scheduler()->wait_for(&core->m_task_hashing_slice, std::chrono::seconds(1));
  core->startup_record_write();
  CPPUNIT_ASSERT(!core->m_startup_written);
  torrent::this_thread::scheduler()->erase(&core->m_task_hashing_slice);

  control->m_shutdown_received = true;
  core->startup_record_write();
  CPPUNIT_ASSERT(!core->m_startup_written);
  control->m_shutdown_received = false;

  control->m_shutdown_quick = true;
  core->startup_record_write();
  CPPUNIT_ASSERT(!core->m_startup_written);
  control->m_shutdown_quick = false;

  core->startup_record_write();
  CPPUNIT_ASSERT(core->m_startup_written);
}

void
TestCommandEvents::test_startup_record_clamps_slice_ms() {
  auto core     = control->core();
  auto written  = core->m_startup_written;
  auto settings = core->m_startup_settings;

  utils::scope_guard restore([core, written, settings]() {
    rpc::call_command_set_value("pieces.hash.slice_ms.set", 100);
    core->m_startup_written  = written;
    core->m_startup_settings = settings;
  });

  for (auto [value, clamped] : { std::pair<int64_t, int64_t>{ -1, 0 }, { 0, 0 }, { 60000, 60000 }, { 60001, 60000 } }) {
    rpc::call_command_set_value("pieces.hash.slice_ms.set", value);
    core->m_startup_written = false;
    core->m_startup_settings.clear();
    core->startup_record_walk(std::chrono::microseconds(1), 0, false);

    CPPUNIT_ASSERT_EQUAL("slice_ms=" + std::to_string(clamped) + " ", core->m_startup_settings.substr(0, core->m_startup_settings.find(' ') + 1));
  }
}

// The retrackers journal readers of a torrent load under one active session
// directory pass: the read path never creates the journal directory, one
// created outside the daemon is seen from the next unit of work, and while the
// pass holds the directory every reader sees a name created after it began.
void
TestCommandEvents::test_journal_readers_in_session_pass() {
  struct Serve {
    Serve() { core::session_directory_pass().serve(std::this_thread::get_id()); }
    ~Serve() { core::session_directory_pass().serve(std::thread::id()); }
  } serve;
  const std::string hash(40, 'A');
  const std::string other(40, 'C');
  const std::string target(40, 'B');
  const std::string journal = session_path + "/.retrackers-migration";
  const auto write = [&](const std::string& name, const std::string& contents) {
    const int fd = ::open((journal + "/" + name).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CPPUNIT_ASSERT(fd >= 0);
    CPPUNIT_ASSERT_EQUAL(static_cast<ssize_t>(contents.size()), ::write(fd, contents.data(), contents.size()));
    CPPUNIT_ASSERT_EQUAL(0, ::close(fd));
  };
  CPPUNIT_ASSERT(retrackers_migration_target_source(hash).empty());
  CPPUNIT_ASSERT(!retrackers_migration_pending(hash));
  CPPUNIT_ASSERT(!std::filesystem::exists(journal));
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(journal.c_str(), 0700));
  write(hash + ".target", target + "\n");
  CPPUNIT_ASSERT(retrackers_migration_target_source(hash).empty());
  core::session_directory_check();
  CPPUNIT_ASSERT_EQUAL(target, retrackers_migration_target_source(hash));
  // The pass holds the directories across a check() that finds them unchanged.
  // A walk would refuse this session directory now and opening the journal
  // directory again would refuse its mode, so the readers below answer from
  // the held descriptors.
  core::session_directory_check();
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(session_path.c_str(), 0777));
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(journal.c_str(), 0750));
  CPPUNIT_ASSERT(retrackers_migration_target_source(other).empty());
  write(other + ".target", target + "\n");
  CPPUNIT_ASSERT_EQUAL(target, retrackers_migration_target_source(other));
  CPPUNIT_ASSERT(!retrackers_migration_pending(hash));
  write(hash + ".rollback.torrent", "d4:infodee");
  CPPUNIT_ASSERT(retrackers_migration_pending(hash));
  CPPUNIT_ASSERT(retrackers_migration_source_reserved(hash));
  // With its release receipt seen the reservation goes on to read the capsule,
  // which this test does not write.
  write(hash + ".released.receipt", "released\n");
  CPPUNIT_ASSERT_THROW(retrackers_migration_source_reserved(hash), torrent::input_error);
  write(hash + ".receipt", "receipt\n");
  CPPUNIT_ASSERT(!retrackers_migration_pending(hash));
  // The next unit of work walks and refuses the session directory as an RR input error.
  core::session_directory_check();
  try {
    retrackers_migration_target_source(hash);
    CPPUNIT_FAIL("an unsafe session directory must be refused");
  } catch (const torrent::input_error& error) {
    CPPUNIT_ASSERT(std::string(error.what()).find("RR migration session path refused: unsafe session directory") == 0);
  }
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(session_path.c_str(), 0700));
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(journal.c_str(), 0700));
}
