#include "config.h"
#include "stop_intent.h"
#include "core/erasedata_lifecycle.h"

#include "download_factory.h"
#include "incarnation.h"
#include "retrackers_migration_limits.h"
#include "retrackers_migration_creation.h"
#include "retrackers_migration_pending.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <openssl/sha.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <torrent/utils/log.h>
#include <torrent/utils/resume.h>
#include <torrent/object.h>
#include <torrent/object_stream.h>
#include <torrent/exceptions.h>
#include <torrent/rate.h>
#include <torrent/data/file_utils.h>
#include <torrent/net/http_stack.h>
#include <torrent/runtime/client_config.h>
#include <torrent/utils/string_manip.h>

#include "control.h"
#include "globals.h"
#include "core/download.h"
#include "core/http_queue.h"
#include "core/manager.h"
#include "rpc/parse_commands.h"

namespace core {

bool
is_network_uri(const std::string& uri) {
  return
    std::strncmp(uri.c_str(), "http://", 7) == 0 ||
    std::strncmp(uri.c_str(), "https://", 8) == 0 ||
    std::strncmp(uri.c_str(), "ftp://", 6) == 0;
}

static constexpr const char* session_invalid_message = "Session data is invalid, ignoring it";

// A session reload has no load options of its own. Keep the start flag and
// commands the metadata placeholder was saved with, if they are well formed.
// A new load always rebuilds the section, so one in the loaded data is never used.
static bool
has_meta_download_options(const torrent::Object& root) {
  if (!root.has_key_map("rtorrent_meta_download"))
    return false;

  const auto& meta = root.get_key("rtorrent_meta_download");

  if (!meta.has_key_value("start") || !meta.has_key_value("print_log") || !meta.has_key_list("commands"))
    return false;

  // process_meta_download() takes start as the declared load flag.
  const auto start = meta.get_key_value("start");
  if (start != 0 && start != 1)
    return false;

  const auto& commands = meta.get_key_list("commands");
  return std::all_of(commands.begin(), commands.end(), rpc::is_valid_command);
}

static std::unique_ptr<torrent::Object>
download_factory_load_stream(const char* filename, bool* is_invalid) {
  struct stat sb;

  if (stat(filename, &sb) != 0 || !S_ISREG(sb.st_mode))
    return std::unique_ptr<torrent::Object>();

  std::fstream stream(filename, std::ios::in | std::ios::binary);

  if (!stream.is_open())
    return std::unique_ptr<torrent::Object>();

  auto obj = std::make_unique<torrent::Object>();
  stream >> *obj;

  if (!stream.good() || !obj->is_map()) {
    *is_invalid = true;
    return std::unique_ptr<torrent::Object>();
  }

  return obj;
}

bool
is_magnet_uri(const std::string& uri) {
  return
    std::strncmp(uri.c_str(), "magnet:?", 8) == 0;
}

DownloadFactory::DownloadFactory(Manager* m) :
    m_manager(m) {

  m_task_load.slot() = std::bind(&DownloadFactory::receive_load, this);
  m_task_commit.slot() = std::bind(&DownloadFactory::receive_commit, this);

  // m_variables["connection_leech"] = rpc::call_command("protocol.connection.leech");
  // m_variables["connection_seed"]  = rpc::call_command("protocol.connection.seed");
  m_variables["connection_leech"] = std::string();
  m_variables["connection_seed"]  = std::string();
  m_variables["directory"]        = rpc::call_command("directory.default");
  m_variables["tied_to_file"]     = torrent::Object((int64_t)false);
}

DownloadFactory::~DownloadFactory() {
  torrent::this_thread::scheduler()->erase(&m_task_load);
  torrent::this_thread::scheduler()->erase(&m_task_commit);

  delete m_object;
}

void
DownloadFactory::load(const std::string& uri) {
  m_uri = uri;
  torrent::this_thread::scheduler()->wait_for(&m_task_load, 0ms);
}

// This function must be called before DownloadFactory::commit().
void
DownloadFactory::load_raw_data(const std::string& input) {
  if (m_stream)
    throw torrent::internal_error("DownloadFactory::load*() called on an object with m_stream != NULL");

  m_stream.reset(new std::stringstream(input));
  m_loaded = true;
}

void
DownloadFactory::commit() {
  torrent::this_thread::scheduler()->wait_for(&m_task_commit, 0ms);
}

bool
DownloadFactory::has_retrackers_apply_command() const {
  return std::any_of(m_commands.begin(), m_commands.end(), [](const std::string& command) {
    return command.rfind("d.retrackers.apply=", 0) == 0;
  });
}

void
DownloadFactory::receive_load() {
  if (m_stream)
    throw torrent::internal_error("DownloadFactory::load*() called on an object with m_stream != NULL");

  if (is_network_uri(m_uri)) {
    m_stream.reset(new std::stringstream);

    auto done_fn   = [this]() { receive_loaded(); };
    auto failed_fn = [this](const std::string& error) { receive_failed(error); };

    m_manager->http_queue()->insert(m_uri, m_stream, done_fn, failed_fn);

    m_variables["tied_to_file"] = (int64_t)false;
    return;
  }

  if (is_magnet_uri(m_uri)) {
    // DEBUG: Use m_object.
    m_stream.reset(new std::stringstream());
    *m_stream << "d10:magnet-uri" << m_uri.length() << ":" << m_uri << "e";

    m_variables["tied_to_file"] = (int64_t)false;

    receive_loaded();
    return;
  }

  std::fstream stream(expand_path(m_uri).c_str(), std::ios::in | std::ios::binary);

  if (!stream.is_open())
    return receive_failed("Could not open file");

  m_object = new torrent::Object;
  if (has_retrackers_apply_command()) {
    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    if (size <= 0 || size > 64 * 1024 * 1024)
      return receive_failed("RR candidate staged metainfo size invalid");
    stream.seekg(0);
    std::string bytes(static_cast<size_t>(size), '\0');
    if (!stream.read(bytes.data(), bytes.size()))
      return receive_failed("RR candidate staged metainfo read failed");
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest);
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char value : digest) {
      m_retrackersRawSha256 += hex[value >> 4];
      m_retrackersRawSha256 += hex[value & 15];
    }
    std::stringstream captured(bytes);
    captured >> *m_object;
    if (!captured.good())
      return receive_failed("RR candidate staged metainfo parse failed");
  } else {
    stream >> *m_object;
    if (!stream.good())
      return receive_failed("Reading torrent file failed");
  }

  m_isFile = true;

  receive_loaded();
}

void
DownloadFactory::receive_loaded() {
  m_loaded = true;

  if (m_commited)
    receive_success();
}

void
DownloadFactory::receive_commit() {
  m_commited = true;

  if (m_loaded)
    receive_success();
}

void
DownloadFactory::receive_success() {
  bool session_invalid = false;

  std::unique_ptr<torrent::Object> rtorrent_object;
  std::unique_ptr<torrent::Object> libtorrent_resume_object;

  if (m_session) {
    if (m_uri.empty())
      throw torrent::input_error("Session torrent URI is empty.");

    rtorrent_object          = download_factory_load_stream((expand_path(m_uri) + ".rtorrent").c_str(), &session_invalid);
    libtorrent_resume_object = download_factory_load_stream((expand_path(m_uri) + ".libtorrent_resume").c_str(), &session_invalid);

    if (session_invalid)
      lt_log_print(torrent::LOG_ERRORS, "%s: %s", session_invalid_message, m_uri.c_str());
  }

  uint32_t tracker_key;

  if (rtorrent_object && rtorrent_object->has_key_value("key"))
    tracker_key = rtorrent_object->get_key_value("key");
  else
    tracker_key = random() % (std::numeric_limits<uint32_t>::max() - 1) + 1;

  Download* download = m_stream != nullptr ?
    m_manager->download_list()->create(m_stream.get(), tracker_key, m_printLog) :
    m_manager->download_list()->create(m_object, tracker_key, m_printLog);

  m_object = NULL;

  if (download == NULL) {
    // core::Manager should already have added the error message to
    // the log.
    m_slot_finished();
    return;
  }

  const std::string rr_hash = torrent::utils::transform_to_hex_str(download->info()->hash());
  const auto refuse_load = [this, download]() {
    torrent::download_remove(*download->download());
    delete download;
    m_slot_finished();
  };

  if (session_invalid) {
    download->set_hash_failed(true);
    download->set_message(session_invalid_message);

    if (m_printLog)
      m_manager->push_log_std(std::string(session_invalid_message) + ": \"" + m_uri + "\"");
  }

  torrent::Object* root = download->bencode();

  if (download->download()->info()->is_meta_download() && !(m_session && has_meta_download_options(*root))) {
    int64_t declared_start = m_start;
    // Session reloads do not set m_start. Preserve a valid original load flag:
    // service magnets may have been started solely to retrieve metadata.
    if (m_session && root->has_key_map("rtorrent_meta_download")) {
      const auto& saved_meta = root->get_key("rtorrent_meta_download");
      if (saved_meta.has_key_value("start") &&
          (saved_meta.get_key_value("start") == 0 || saved_meta.get_key_value("start") == 1))
        declared_start = saved_meta.get_key_value("start");
    }
    torrent::Object& meta = root->insert_key("rtorrent_meta_download", torrent::Object::create_map());
    meta.insert_key("start", declared_start);
    meta.insert_key("print_log", m_printLog);

    torrent::Object::list_type& commands = meta.insert_key("commands", torrent::Object::create_list()).as_list();

    for (auto& m_command : m_commands)
      commands.push_back(m_command);
  }

  if (m_session) {
    if (rtorrent_object)
      root->insert_key_move("rtorrent", *rtorrent_object);

    if (libtorrent_resume_object)
      root->insert_key_move("libtorrent_resume", *libtorrent_resume_object);

  } else {
    // We only allow session torrents to keep their
    // 'rtorrent/libtorrent' sections. The "fast_resume" section
    // should be safe to keep.
    root->erase_key("rtorrent");
  }

  torrent::Object* rtorrent = &root->insert_preserve_copy("rtorrent", torrent::Object::create_map()).first->second;
  torrent::Object& resumeObject = root->insert_preserve_copy("libtorrent_resume", torrent::Object::create_map()).first->second;

  rtorrent->insert_key("key", download->tracker_controller().key());

  const bool saved_incarnation_valid = rtorrent->has_key_string("incarnation") &&
      valid_incarnation(rtorrent->get_key_string("incarnation"));
  // Session loads keep the daemon identity; new objects always receive a fresh one.
  if (!m_session || !rtorrent->has_key_string("incarnation") ||
      !valid_incarnation(rtorrent->get_key_string("incarnation"))) {
    if (m_session && rtorrent->has_key("incarnation"))
      m_manager->push_log_std("Invalid session incarnation; assigning a new ID: " + m_uri);
    rtorrent->insert_key("incarnation", fresh_incarnation());
  }

  bool erasedata_held = false;
  try {
    erasedata_held = erasedata_lifecycle::admit(rpc::call_command_string("session.path"),
        rr_hash, rtorrent->get_key_string("incarnation"), m_session, saved_incarnation_valid);
  } catch (const std::exception& e) {
    const auto message = "erasedata: load refused H=" + rr_hash + ": " + e.what();
    lt_log_print(torrent::LOG_ERRORS, "%s", message.c_str());
    m_manager->push_log_std(message);
    refuse_load();
    return;
  }
  initialize_rtorrent(download, rtorrent);
  if (erasedata_held) {
    rtorrent->insert_key("state", int64_t(0));
    erasedata_intent::Record intent;
    erasedata_intent::read(rpc::call_command_string("session.path"), rr_hash, &intent);
    download->set_message("erasedata: original session recovery held; inspect " +
        erasedata_lifecycle::description(intent) + "; exact erase recovery or ordinary Remove/re-add cancels");
  }

  if (!rtorrent->has_key_string("custom1")) rtorrent->insert_key("custom1", std::string());
  if (!rtorrent->has_key_string("custom2")) rtorrent->insert_key("custom2", std::string());
  if (!rtorrent->has_key_string("custom3")) rtorrent->insert_key("custom3", std::string());
  if (!rtorrent->has_key_string("custom4")) rtorrent->insert_key("custom4", std::string());
  if (!rtorrent->has_key_string("custom5")) rtorrent->insert_key("custom5", std::string());

  rpc::call_command("d.uploads_min.set",      rpc::call_command("throttle.min_uploads"), rpc::make_target(download));
  rpc::call_command("d.uploads_max.set",      rpc::call_command("throttle.max_uploads"), rpc::make_target(download));
  rpc::call_command("d.downloads_min.set",    rpc::call_command("throttle.min_downloads"), rpc::make_target(download));
  rpc::call_command("d.downloads_max.set",    rpc::call_command("throttle.max_downloads"), rpc::make_target(download));
  rpc::call_command("d.peers_min.set",        rpc::call_command("throttle.min_peers.normal"), rpc::make_target(download));
  rpc::call_command("d.peers_max.set",        rpc::call_command("throttle.max_peers.normal"), rpc::make_target(download));
  rpc::call_command("d.tracker_numwant.set",  rpc::call_command("trackers.numwant"), rpc::make_target(download));
  rpc::call_command("d.max_file_size.set",    rpc::call_command("system.file.max_size"), rpc::make_target(download));

  if (rpc::call_command_value("d.complete", rpc::make_target(download)) != 0) {
    if (rpc::call_command_value("throttle.min_peers.seed") >= 0)
      rpc::call_command("d.peers_min.set", rpc::call_command("throttle.min_peers.seed"), rpc::make_target(download));

    if (rpc::call_command_value("throttle.max_peers.seed") >= 0)
      rpc::call_command("d.peers_max.set", rpc::call_command("throttle.max_peers.seed"), rpc::make_target(download));
  }

  if (m_session && rtorrent->has_key("retrackers_limits")) {
    int64_t values[6] = {};
    bool valid = rtorrent->has_key_map("retrackers_limits");
    if (valid) {
      const auto& limits = rtorrent->get_key("retrackers_limits");
      size_t i = 0;
      for (const char* key : retrackers_migration_limits) {
        if (!limits.has_key_value(key)) { valid = false; break; }
        const auto value = limits.get_key_value(key);
        if (value < -1 || value > 2147483647) { valid = false; break; }
        values[i++] = value;
      }
    }
    if (!valid) {
      const std::string msg = "RR session sidecar " + m_uri + ".rtorrent: retrackers_limits invalid; download held.";
      download->set_hash_failed(true);
      download->set_message(msg);
      m_manager->push_log_std(msg);
    } else {
      size_t i = 0;
      for (const char* key : retrackers_migration_limits)
        rpc::call_command((std::string(key) + ".set").c_str(), values[i++], rpc::make_target(download));
    }
  }

  // Skip forcing trackers to scrape when rtorrent starts
  if (m_initLoad && rpc::call_command_value("trackers.delay_scrape"))
    download->set_resume_flags(torrent::Download::start_skip_tracker);

  // Check first if we already have these values set in the session
  // torrent, so that it is safe to change the values.
  try {
    if (rpc::call_command_value("system.file.split_size") >= 0)
      torrent::file_split_all(download->download()->file_list(),
                              rpc::call_command_value("system.file.split_size"),
                              rpc::call_command_string("system.file.split_suffix"));
  } catch (const torrent::input_error& e) {
    std::string msg = "Could not split files: " + std::string(e.what());

    lt_log_print(torrent::LOG_ERRORS, "%s: %s", msg.c_str(), m_uri.c_str());

    torrent::download_remove(*download->download());
    delete download;

    return receive_failed(msg);
  }

  try {
    // Restoring a saved directory is allowed while a move claim keeps the download closed.
    stop_intent::ScopedRestorePath restore(m_session);
    if (rtorrent->has_key_string("directory")) {
      rpc::call_command("d.directory_base.set", rtorrent->get_key("directory"), rpc::make_target(download));

    } else if (download->download()->info()->is_meta_download()) {
      auto& metadata_path = control->core()->magnet_path();

      if (!metadata_path.empty())
        rpc::call_command("d.directory.set", metadata_path, rpc::make_target(download));
      else
        rpc::call_command("d.directory.set", session_thread::session_path(), rpc::make_target(download));

    } else {
      rpc::call_command("d.directory.set", m_variables["directory"], rpc::make_target(download));
    }
  } catch (const torrent::input_error& e) {
    const std::string msg = "RR load initialization refused: " + rr_hash + ": " + e.what();
    lt_log_print(torrent::LOG_ERRORS, "%s", msg.c_str());
    m_manager->push_log_std(msg);
    refuse_load();
    return;
  }

  if (!m_session && m_variables["tied_to_file"].as_value())
    rpc::call_command("d.tied_to_file.set", m_uri.empty() ? m_variables["tied_file"] : m_uri, rpc::make_target(download));

  rpc::call_command("d.peer_exchange.set", torrent::runtime::client_config()->is_pex_enabled(), rpc::make_target(download));

  try {
    torrent::resume_load_addresses(*download->download(), resumeObject);
    torrent::resume_load_file_priorities(*download->download(), resumeObject);
    torrent::resume_load_tracker_settings(*download->download(), resumeObject);

  } catch (const torrent::input_error& e) {
    std::string msg = std::string(session_invalid_message) + ": " + e.what();

    lt_log_print(torrent::LOG_ERRORS, "%s: %s", msg.c_str(), m_uri.c_str());

    if (m_printLog)
      m_manager->push_log_std(msg + ": \"" + m_uri + "\"");

    download->set_hash_failed(true);
    download->set_message(msg);
  }

  std::string rr_target_source;
  bool rr_unproven = false;
  try {
    rr_target_source = retrackers_migration_target_source(rr_hash);
    const bool rr_has_apply = has_retrackers_apply_command();
    if ((retrackers_migration_source_reserved(rr_hash) && m_retrackersRollbackHash != rr_hash) ||
        !rr_target_source.empty()) {
      const bool committed_session = !rr_target_source.empty() && m_session &&
          retrackers_migration_committed(rr_target_source, rr_hash) && !download->is_hash_failed();
      const bool restored_session = m_session && rr_target_source == rr_hash &&
          retrackers_migration_restored_source(rr_hash) && !download->is_hash_failed();
      const bool prepared_session = m_session && !download->is_hash_failed() &&
          retrackers_migration_prepared_session(download, rr_hash);
      const bool pending_candidate = !rr_target_source.empty() && !m_session && rr_has_apply &&
          retrackers_migration_pending(rr_target_source);
      const bool daemon_rollback = m_session && m_retrackersRollbackHash == rr_hash &&
          !download->is_hash_failed();
      const std::string expected_digest = pending_candidate ?
          retrackers_migration_candidate_digest(rr_target_source) : std::string();
      const bool candidate_digest_matches = expected_digest.empty() ||
          expected_digest == m_retrackersRawSha256;
      rr_unproven = ((!committed_session && !restored_session && !prepared_session &&
                      !pending_candidate && !daemon_rollback) ||
                     !candidate_digest_matches);
    }
  } catch (const torrent::input_error& e) {
    const std::string msg = "RR migration reservation load refused: " + rr_hash + ": " + e.what();
    lt_log_print(torrent::LOG_ERRORS, "%s", msg.c_str());
    m_manager->push_log_std(msg);
    refuse_load();
    return;
  }
  if (rr_unproven) {
    m_manager->push_log_std("RR migration pending: unproven load refused: " + rr_hash);
    refuse_load();
    return;
  }

  // The action of inserting might cause the torrent to be
  // opened/started or such. Figure out a nicer way of handling this.
  if (m_manager->download_list()->insert(download, erasedata_held) == m_manager->download_list()->end()) {
    // Insertion owns the download even when a user callback erases it.

    m_slot_finished();
    return;
  }

  // Save the info-hash just in case the commands decide to delete it.
  torrent::HashString infohash = download->info()->hash();
  // Load commands may erase the target while other snapshots keep its wrapper.
  const auto lifetime = download->lifetime();

  RetrackersMigrationCreation rr_creation(download);
  try {
    if (!rr_target_source.empty() && !m_session) {
      RetrackersMigrationCreation::mark_apply(download);
      if (m_start) throw torrent::input_error("RR migration candidate must load stopped.");
    }
    if (!m_retrackersRollbackHash.empty()) {
      RetrackersMigrationCreation::mark_apply(download);
      if (torrent::utils::transform_to_hex_str(download->info()->hash()) != m_retrackersRollbackHash ||
          download->is_hash_failed())
        throw torrent::input_error("RR rollback session snapshot is invalid.");
    }
    if (torrent::log_groups[torrent::LOG_TORRENT_DEBUG].valid())
      log_created(download, rtorrent);

    if (!erasedata_held)
      for (const auto& command : m_commands)
        rpc::parse_command_multiple_std(command, rpc::make_target(download), [&lifetime]() {
            if (lifetime.expired())
              throw torrent::input_error("The newly created download was removed.");
          });
    if (!rr_target_source.empty() && !m_session && !rr_creation.applied())
      throw torrent::input_error("RR migration apply did not finish.");

    if (lifetime.expired())
      throw torrent::input_error("The newly created download was removed.");

    if (!m_session)
       rpc::call_command("d.state.set", (int64_t)m_start, rpc::make_target(download));

    bool session_event_ready = !erasedata_held;
    if (m_session && !erasedata_held) {
      session_event_ready = retrackers_migration_repair_prepared_loaded(download);
      // A completed same-hash release retires the reservation. The restored
      // owner then gets the ordinary session event, which starts a saved
      // started download; skipping it left state=1 but closed and inactive.
      if (session_event_ready && !rr_target_source.empty()) {
        try { rr_target_source = retrackers_migration_target_source(rr_hash); }
        catch (const torrent::input_error&) { /* Unknown: keep the event withheld. */ }
      }
      if (session_event_ready) {
        try {
          const auto claim = stop_intent::lease(download);
          if (claim.phase == 'T' || (claim.phase == 'D' && claim.mode == 'O'))
            rpc::call_command("d.replay_stop_close_claim", claim.job, rpc::make_target(download));
        } catch (const std::exception& e) {
          session_event_ready = false;
          lt_log_print(torrent::LOG_ERRORS, "Stop claim terminal replay held startup: %s", e.what());
        }
      }
    }
    // An inserted-session callback may erase its target. Finish native replay first.
    if (session_event_ready) {
      stop_intent::ScopedSuppress suppress;
      // v1 candidate acknowledgement is owned by the ordinary inserted_new hook.
      if (rr_target_source.empty() ||
          (!m_session && !retrackers_migration_candidate_digest(rr_target_source).empty()))
        rpc::commands.call_catch(m_session ? "event.download.inserted_session" : "event.download.inserted_new",
                                 rpc::make_target(download), torrent::Object(), "Download event action failed: ");
    }

  } catch (torrent::input_error& e) {
    std::string msg = "Command on torrent creation failed: " + std::string(e.what());

    if (m_printLog)
      m_manager->push_log_std(msg);

    auto existing = m_manager->download_list()->find(infohash);
    if (!lifetime.expired() && existing != m_manager->download_list()->end() && existing->get() == download) {
      if (rr_creation.attempted()) {
        m_manager->push_log_std("RR migration candidate aborted: " + msg);
        // Keep the input metainfo available for a later recovery attempt.
        rpc::call_command("d.tied_to_file.set", std::string(), rpc::make_target(download));
        if (!m_session && !rr_target_source.empty() &&
            retrackers_migration_uncommitted_candidate(
                torrent::utils::transform_to_hex_str(infohash))) {
          RetrackersMigrationCreation::ScopedAbortErase abort(download);
          m_manager->download_list()->erase_ptr(download);
        } else {
          m_manager->download_list()->erase_ptr(download);
        }
      } else {
        download->set_hash_failed(true);
        download->set_message(msg);
      }
    }
  }

  if (!m_retrackersRollbackHash.empty()) {
    auto existing = m_manager->download_list()->find(infohash);
    m_retrackersRollbackSucceeded = existing != m_manager->download_list()->end() &&
        existing->get() == download && !download->is_hash_failed();
  }
  m_slot_finished();
}

void
DownloadFactory::log_created(Download* download, torrent::Object* rtorrent) {
  std::stringstream dump;

  dump << "info_hash = " << torrent::utils::transform_to_hex_str(download->info()->hash()) << std::endl;
  dump << "session = " << (m_session ? "true" : "false") << std::endl;

  if (download->download()->info()->is_meta_download())
    dump << "magnet = true" << std::endl;

  if (!rtorrent->has_key_string("directory"))
    dump << "directory = \"" << (m_variables["directory"].is_string() ? m_variables["directory"].as_string() : std::string()) << '"' << std::endl;
  else
    dump << "directory_base = \"" << (rtorrent->get_key("directory").is_string() ? rtorrent->get_key("directory").as_string() : std::string()) << '"' << std::endl;

  dump << "---COMMANDS---" << std::endl;

  for (const auto& m_command : m_commands) {
    dump << m_command << std::endl;
  }

  std::string dump_str = dump.str();

  lt_log_print_dump(torrent::LOG_TORRENT_DEBUG, dump_str.c_str(), dump_str.size(), "Creating new download:");
}

void
DownloadFactory::receive_failed(const std::string& msg) {
  // Add message to log.
  if (m_printLog)
    m_manager->push_log_std(msg + ": \"" + m_uri + "\"");

  m_slot_finished();
}

void
DownloadFactory::initialize_rtorrent(Download* download, torrent::Object* rtorrent) {
  auto cached_seconds = torrent::this_thread::cached_seconds().count();

  if (!rtorrent->has_key_value("state") || rtorrent->get_key_value("state") > 1) {
    rtorrent->insert_key("state", (int64_t)m_start);
    rtorrent->insert_key("state_changed", cached_seconds);
    rtorrent->insert_key("state_counter", int64_t());

  } else if (!rtorrent->has_key_value("state_changed") ||
             rtorrent->get_key_value("state_changed") > cached_seconds || rtorrent->get_key_value("state_changed") == 0 ||
             !rtorrent->has_key_value("state_counter") || (uint64_t)rtorrent->get_key_value("state_counter") > (1 << 20)) {
    rtorrent->insert_key("state_changed", cached_seconds);
    rtorrent->insert_key("state_counter", int64_t());
  }

  // A new same-hash download starts at the current durable generation. Old
  // journal records belong to a prior incarnation, not to this new user load.
  if (!m_session) {
    try {
      const auto hash = rpc::call_command_string("d.hash", rpc::make_target(download));
      rtorrent->insert_key("stop_generation", static_cast<int64_t>(
          stop_intent::counter(rpc::call_command_string("session.path"), hash, false)));
    } catch (const std::exception& e) {
      rtorrent->insert_key("state", int64_t(0));
      m_start = false;
      download->set_hash_failed(true);
      download->set_message("Stop intent generation unavailable; new download held");
      lt_log_print(torrent::LOG_ERRORS, "Stop intent generation unavailable for new download: %s", e.what());
    }
  }
  // A saved active session older than the last durable Stop must not autostart.
  if (m_session) {
    try {
      const auto hash = rpc::call_command_string("d.hash", rpc::make_target(download));
      const auto durable = stop_intent::counter(rpc::call_command_string("session.path"), hash, false);
      const auto saved = rtorrent->has_key_value("stop_generation") ?
        rtorrent->get_key_value("stop_generation") : int64_t(0);
      if (saved < 0 || static_cast<uint64_t>(saved) != durable ||
          !stop_intent::startup_hold_token(rpc::call_command_string("session.path"), hash).empty()) {
        rtorrent->insert_key("state", int64_t(0));
        rtorrent->insert_key("stop_generation", static_cast<int64_t>(durable));
        lt_log_print(torrent::LOG_ERRORS, "Stop intent recovery held session startup for %s", hash.c_str());
      }
    } catch (const std::exception& e) {
      rtorrent->insert_key("state", int64_t(0));
      lt_log_print(torrent::LOG_ERRORS, "Stop intent recovery held session startup: %s", e.what());
    }
  }

  rtorrent->insert_preserve_copy("complete", (int64_t)0);
  rtorrent->insert_preserve_copy("hashing",  (int64_t)Download::variable_hashing_stopped);

  rtorrent->insert_preserve_copy("timestamp.started",  (int64_t)0);
  rtorrent->insert_preserve_copy("timestamp.finished", (int64_t)0);

  rtorrent->insert_preserve_copy("tied_to_file", "");
  rtorrent->insert_key("loaded_file", m_isFile ? m_uri : std::string());

  if (rtorrent->has_key_value("priority"))
    rpc::call_command("d.priority.set", rtorrent->get_key_value("priority") % 4, rpc::make_target(download));
  else
    rpc::call_command("d.priority.set", (int64_t)2, rpc::make_target(download));

  if (rtorrent->has_key_value("total_uploaded"))
    download->info()->mutable_up_rate()->set_total(rtorrent->get_key_value("total_uploaded"));

  if (rtorrent->has_key_value("total_downloaded"))
    download->info()->mutable_down_rate()->set_total(rtorrent->get_key_value("total_downloaded"));

  if (rtorrent->has_key_value("chunks_done") && rtorrent->has_key_value("chunks_wanted"))
    download->download()->set_chunks_done(rtorrent->get_key_value("chunks_done"), rtorrent->get_key_value("chunks_wanted"));

  download->set_throttle_name(rtorrent->has_key_string("throttle_name")
                              ? rtorrent->get_key_string("throttle_name")
                              : std::string());

  rtorrent->insert_preserve_copy("ignore_commands", (int64_t)0);
  rtorrent->insert_preserve_copy("views", torrent::Object::create_list());

  rtorrent->insert_preserve_type("connection_leech", m_variables["connection_leech"]);
  rtorrent->insert_preserve_type("connection_seed",  m_variables["connection_seed"]);

  rtorrent->insert_preserve_copy("choke_heuristics.up.leech",   std::string());
  rtorrent->insert_preserve_copy("choke_heuristics.up.seed",    std::string());
  rtorrent->insert_preserve_copy("choke_heuristics.down.leech", std::string());
  rtorrent->insert_preserve_copy("choke_heuristics.down.seed",  std::string());
}

}
