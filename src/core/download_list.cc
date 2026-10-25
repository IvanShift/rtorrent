#include "config.h"
#include "stop_intent.h"
#include "core/file_jobs.h"
#include "core/download_admission.h"
#include "core/erasedata_lifecycle.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <torrent/data/file.h>
#include <torrent/utils/resume.h>
#include <torrent/exceptions.h>
#include <torrent/download.h>
#include <torrent/hash_string.h>
#include <torrent/object.h>
#include <torrent/object_stream.h>
#include <torrent/torrent.h>
#include <torrent/utils/log.h>
#include <torrent/utils/string_manip.h>

#include "rpc/parse_commands.h"

#include "control.h"
#include "globals.h"
#include "manager.h"
#include "view.h"
#include "view_manager.h"

#include "core/dht_manager.h"
#include "core/download.h"
#include "core/download_list.h"
#include "core/download_factory.h"
#include "core/retrackers_migration_creation.h"
#include "core/retrackers_migration_pending.h"
#include "session/session_manager.h"
#include "ui/root.h"

#define LT_LOG_DOWNLOAD_DEBUG(log_fmt, ...)                             \
  lt_log_print_hash(torrent::LOG_TORRENT_DEBUG, download->info()->hash(), "download_list : " log_fmt, __VA_ARGS__);
#define LT_LOG_DOWNLOAD_EVENTS(log_fmt, ...)                             \
  lt_log_print_info(torrent::LOG_TORRENT_EVENTS, download->info(), "download_list : " log_fmt, __VA_ARGS__);

#define DL_TRIGGER_EVENT(download, event_name)                          \
  rpc::commands.call_catch(event_name, rpc::make_target(download), torrent::Object(), "Event '" event_name "' failed: ");

namespace core {

inline void
DownloadList::check_contains([[maybe_unused]] Download* d) {
#ifdef USE_EXTRA_DEBUG
  if (std::find_if(begin(), end(), [d](const auto& entry) { return entry.get() == d; }) == end())
    throw torrent::internal_error("DownloadList::check_contains(...) failed.");
#endif
}

void
DownloadList::clear() {
  int error_count = 0;

  while (!empty()) {
    auto download = back();

    try {
      close(download);
      base_type::pop_back();

      torrent::download_remove(*download->download());

    } catch (torrent::internal_error& e) {
      lt_log_print(torrent::LOG_ERRORS, "DownloadList::clear() failed to close or remove download: %s", e.what());
      error_count++;
      continue;
    }
  }

  if (error_count > 0)
    throw torrent::internal_error("DownloadList::clear() failed to close or remove " + std::to_string(error_count) + " downloads.");
}

void
DownloadList::session_save() {
  for (auto& download : *this)
    session_thread::manager()->save_resume_download(download.get());

  control->dht_manager()->save_dht_cache();
  control->ui()->save_input_history();
}

DownloadList::iterator
DownloadList::find(const torrent::HashString& hash) {
  return std::find_if(begin(), end(), [hash](const auto& d) { return hash == d->info()->hash(); });
}

DownloadList::iterator
DownloadList::find_hex(const char* hash) {
  if (strlen(hash) != 40)
    return end();

  torrent::HashString key;

  if (torrent::utils::transform_from_hex(hash, hash + 40, key) != key.end())
    return end();

  return std::find_if(begin(), end(), [key](const auto& d) { return key == d->info()->hash(); });
}

Download*
DownloadList::find_hex_ptr(const char* hash) {
  iterator itr = find_hex(hash);

  return itr != end() ? itr->get() : NULL;
}

Download*
DownloadList::create(torrent::Object* obj, uint32_t tracker_key, bool printLog) {
  if (!obj->is_map() || download_input_zero_infohash(*obj)) {
    const auto reason = obj->is_map() ? "zero-info-hash" : "invalid-metainfo-root";
    lt_log_print(torrent::LOG_ERRORS, "Download load refused: %s", reason);
    control->core()->push_log_std(std::string("Download load refused: ") + reason);
    delete obj;
    return nullptr;
  }
  torrent::Download download;

  try {
    download = torrent::download_add(obj, tracker_key);

  } catch (torrent::local_error& e) {
    if (printLog)
      lt_log_print(torrent::LOG_TORRENT_EVENTS, "download_list : could not create download : %s", e.what());

    delete obj;

    return NULL;
  }

  // There's no non-critical exceptions that should be throwable by
  // the ctor, so don't catch.
  return new Download(download);
}

Download*
DownloadList::create(std::istream* str, uint32_t tracker_key, bool printLog) {
  torrent::Object* object = new torrent::Object;

  try {
    *str >> *object;

    // Don't throw input_error from here as gcc-3.3.5 produces bad
    // code.
    if (str->fail()) {
      delete object;

      if (printLog)
        lt_log_print(torrent::LOG_TORRENT_EVENTS, "download_list : could not create download : invalid or incomplete bencode data");

      return NULL;
    }

    return create(object, tracker_key, printLog);

  } catch (torrent::local_error& e) {
    delete object;

    if (printLog)
      lt_log_print(torrent::LOG_TORRENT_EVENTS, "download_list : could not create download : %s", e.what());

    return NULL;
  }

}

DownloadList::iterator
DownloadList::insert(Download* download, bool held_recovery) {
  iterator itr = base_type::insert(end(), std::shared_ptr<Download>(download));
  download->set_insertion_deferred(held_recovery);

  LT_LOG_DOWNLOAD_DEBUG("inserting download", 0);

  try {
    (*itr)->data()->slot_initial_hash()        = std::bind(&DownloadList::hash_done, this, download);
    (*itr)->data()->slot_download_done()       = std::bind(&DownloadList::received_finished, this, download);

    // This needs to be separated into two different calls to ensure
    // the download remains in the view.
    for (auto v : *control->view_manager()) {
      if (held_recovery && v->name() == "main") v->insert_held_visible(*itr);
      else v->insert(*itr);
    }
    if (!held_recovery && !initialize_insertion(download)) return end();

  } catch (torrent::local_error& e) {
    // Should perhaps relax this, just print an error and remove the
    // downloads?
    throw torrent::internal_error("Caught during DownloadList::insert(...): " + std::string(e.what()));
  }

  return itr;
}

bool
DownloadList::initialize_insertion(Download* download) {
  const auto lifetime = download->lifetime();
  const std::vector<View*> views(control->view_manager()->begin(), control->view_manager()->end());
  for (auto view : views) {
    view->filter_download(download);
    if (lifetime.expired()) return false;
  }
  DL_TRIGGER_EVENT(download, "event.download.inserted");
  return !lifetime.expired();
}

void
DownloadList::release_held_insertion(Download* download, bool resume_allowed) {
  check_contains(download);
  const auto lifetime = download->lifetime();
  if (download->insertion_deferred()) {
    // The exact P is already durably cancelled and its lock closed. Consume
    // before callbacks: a reentrant cancel or erase must not replay insertion.
    download->set_insertion_deferred(false);
    if (!initialize_insertion(download)) return;
    stop_intent::ScopedSuppress suppress;
    DL_TRIGGER_EVENT(download, "event.download.inserted_session");
    if (lifetime.expired()) return;
    return;
  }
  if (resume_allowed && replay_deferred_completion(download)) return;
  if (lifetime.expired()) return;
  // Preserve a later Stop. A completed hash may already be in the started
  // view, so merely filtering views would make the next Start a no-op.
  if (resume_allowed && !download->is_hash_failed() &&
      rpc::call_command_value("d.hashing", rpc::make_target(download)) == 0 &&
      rpc::call_command_value("d.state", rpc::make_target(download)) == 1)
    resume(download, download->resume_flags());
  else
    control->core()->hashing_view()->filter_download(download);
}

bool
DownloadList::replay_deferred_completion(Download* download) {
  // Close discards the checked bitfield. Retain the notification until a fresh
  // hash proves completion; opening alone cannot authorize finished callbacks.
  if (!download->completion_deferred() || !download->is_hash_checked() ||
      !download->is_done()) return false;
  if (defer_completion_if_held(download)) return false;
  download->set_completion_deferred(false);
  received_finished(download);
  return true;
}

void
DownloadList::erase_ptr(Download* download) {
  erase(std::find_if(begin(), end(), [download](const auto& entry) { return entry.get() == download; }));
}

void
DownloadList::erase_meta_placeholder(Download* download) {
  erase(std::find_if(begin(), end(), [download](const auto& entry) { return entry.get() == download; }), false);
}

DownloadList::iterator
DownloadList::erase(iterator itr, bool retire_stop_intent) {
  View::check_unlocked();

  if (itr == end())
    throw torrent::internal_error("DownloadList::erase(...) could not find download.");

  // An event handler below may erase the same download again.
  if ((*itr)->is_erasing())
    return std::next(itr);

  stop_intent::require_unleased(itr->get());
  const std::string erase_hash = torrent::utils::transform_to_hex_str((*itr)->info()->hash());
  if (retrackers_migration_original_claim_active(erase_hash) &&
      rr_native_capture_erase_hash != erase_hash &&
      !(RetrackersMigrationCreation::abort_erase_permitted(itr->get()) &&
        retrackers_migration_uncommitted_candidate(erase_hash))) {
    // Only an exact queued, uncaptured observation may be cancelled for a
    // direct erase; that cancellation is durable before any erase state below.
    bool cancelled = false;
    try {
      cancelled = retrackers_migration_cancel_queued_original(itr->get());
    } catch (const torrent::local_error& e) {
      // Refusals and storage failures alike keep the claim and the download.
      throw torrent::input_error(std::string("RR original claim blocks direct erase: ") + e.what());
    }
    if (!cancelled)
      throw torrent::input_error("RR original claim blocks direct erase.");
  }
  // Read every migration guard before changing erasure state or removing views.
  const bool retire_counter = retire_stop_intent && !retrackers_migration_pending(erase_hash) &&
      !retrackers_migration_original_claim_active(erase_hash) &&
      !retrackers_migration_uncommitted_candidate(erase_hash);
  erasedata_lifecycle::ordinary_erase(itr->get());
  core::file_jobs::erase_download(itr->get());
  retrackers_migration_retire_released(itr->get());
  (*itr)->set_erasing();
  // Callbacks below can add views. New views omit erasing downloads; existing
  // views are visited through stable pointers rather than manager iterators.
  const std::vector<View*> views(control->view_manager()->begin(), control->view_manager()->end());
  // Erasure and its view callbacks are internal shutdown, not a newer user Stop.
  stop_intent::ScopedSuppress suppress;

  lt_log_print_hash(torrent::LOG_TORRENT_DEBUG, (*itr)->info()->hash(), "download_list : removing download");

  // Makes sure close doesn't restart hashing of this download.
  (*itr)->set_hash_failed(true);

  close(*itr);
  session_thread::manager()->remove_download(itr->get());
  retrackers_migration_retire_erased_owner(erase_hash);

  DL_TRIGGER_EVENT(*itr, "event.download.erased");

  for (auto view : views)
    view->erase(itr->get());

  if (retire_counter) {
    try { stop_intent::erase_counter(session_thread::manager()->path(), erase_hash); }
    catch (const std::exception& e) {
      lt_log_print(torrent::LOG_ERRORS, "Cannot retire stop intent for %s: %s", erase_hash.c_str(), e.what());
    }
  }

  (*itr)->release_lifetime();
  torrent::download_remove(*(*itr)->download());

  return base_type::erase(itr);
}

bool
DownloadList::open(Download* download) {
  try {

    open_throw(download);

    return true;

  } catch (torrent::local_error& e) {
    lt_log_print(torrent::LOG_TORRENT_EVENTS, "download_list : could not open download : %s", e.what());
    return false;
  }
}

void
DownloadList::open_throw(Download* download) {
  View::check_unlocked();
  check_contains(download);
  erasedata_lifecycle::require_activity(download);
  if (retrackers_migration_candidate_activity_held(
      torrent::utils::transform_to_hex_str(download->info()->hash())))
    throw torrent::input_error("RR candidate open held until durable release.");
  if (!stop_intent::owner_open() && !stop_intent::terminal_activity_allowed(download))
    stop_intent::require_unleased(download);

  lt_log_print_hash(torrent::LOG_TORRENT_DEBUG, download->info()->hash(), "download_list : opening download");

  if (download->download()->info()->is_open())
    return;

  int openFlags = download->resume_flags();

  if (rpc::call_command_value("system.file.allocate"))
    openFlags |= torrent::Download::open_enable_fallocate;

  download->download()->open(openFlags);

  DL_TRIGGER_EVENT(download, "event.download.opened");
}

void
DownloadList::close(Download* download) {
  try {
    close_throw(download);

  } catch (torrent::local_error& e) {
    lt_log_print(torrent::LOG_TORRENT_EVENTS, "download_list : could not close download : %s", e.what());
  }
}

// Releases the files without changing the download's state, for callers that
// need the files closed and will keep using the download.
void
DownloadList::close_files(Download* download) {
  View::check_unlocked();
  lt_log_print_hash(torrent::LOG_TORRENT_DEBUG, download->info()->hash(), "download_list : closing download files");

  if (download->download()->info()->is_active()) {
    download->download()->stop(torrent::Download::stop_skip_tracker);

    if (torrent::resume_check_target_files(*download->download(), download->download()->bencode()->get_key("libtorrent_resume")))
      torrent::resume_save_progress(*download->download(), download->download()->bencode()->get_key("libtorrent_resume"));
  }

  if (download->download()->info()->is_open())
    download->download()->close();
}

void
DownloadList::close_directly(Download* download) {
  // set_state_stopped() would refuse only after the files are closed.
  View::check_unlocked();

  lt_log_print_hash(torrent::LOG_TORRENT_DEBUG, download->info()->hash(), "download_list : closing download directly");

  auto lifetime   = download->lifetime();
  bool was_active = download->download()->info()->is_active();
  bool was_open   = download->download()->info()->is_open();

  close_files(download);
  set_state_stopped(download);

  if (was_active) {
    DL_TRIGGER_EVENT(download, "event.download.paused");

    if (lifetime.expired())
      return;

    update_paused_state(download);
  }

  if (was_open) {
    DL_TRIGGER_EVENT(download, "event.download.hash_removed");

    if (lifetime.expired())
      return;

    DL_TRIGGER_EVENT(download, "event.download.closed");
  }
}

void
DownloadList::set_state_stopped(Download* download) {
  control->view_manager()->find_ptr_throw("stopped")->set_visible(download);
  rpc::call_command("d.state.set", (int64_t)0, rpc::make_target(download));
}

void
DownloadList::update_paused_state(Download* download) {
  rpc::call_command("d.state_changed.set", torrent::this_thread::cached_seconds().count(), rpc::make_target(download));
  rpc::call_command("d.state_counter.set", rpc::call_command_value("d.state_counter", rpc::make_target(download)) + 1, rpc::make_target(download));

  // If initial seeding is complete, don't try it again when restarting.
  if (download->is_done() &&
      rpc::call_command("d.connection_current", torrent::Object(), rpc::make_target(download)).as_string() == "initial_seed")
    rpc::call_command("d.connection_seed.set", rpc::call_command("d.connection_current", torrent::Object(), rpc::make_target(download)), rpc::make_target(download));
}

void
DownloadList::close_quick(Download* download) {
  lt_log_print_hash(torrent::LOG_TORRENT_DEBUG, download->info()->hash(), "download_list : closing download quickly.");

  close(download);

  // Make sure we cancel any tracker requests. This should rather be
  // handled by some parameter to the close function, or some other
  // way of giving the client more control of when STOPPED requests
  // are sent.
  download->download()->manual_cancel();
}

void
DownloadList::close_throw(Download* download) {
  check_contains(download);

  LT_LOG_DOWNLOAD_DEBUG("closing download with throw", 0);

  // When pause gets called it will clear the initial hash check state
  // and set hash failed. This should ensure hashing doesn't restart
  // until resume gets called.
  auto lifetime = download->lifetime();

  pause(download);

  if (lifetime.expired())
    return;

  // Check for is_open after pause due to hashing.
  if (!download->is_open())
    return;

  // Save the torrent on close, this covers shutdown and if a torrent
  // is manually closed which would clear the progress data. For
  // better crash protection, save regulary in addition to this.
  //
  // Used to be in pause, but this was wrong for rehashing etc.
  //
  // Reconsider this save. Should be done explicitly when shutting down.
  //control->core()->download_store()->save(download);

  download->download()->close();

  if (!download->is_hash_failed() && rpc::call_command_value("d.hashing", rpc::make_target(download)) != Download::variable_hashing_stopped)
    throw torrent::internal_error("DownloadList::close_throw(...) called but we're going into a hashing loop.");

  DL_TRIGGER_EVENT(download, "event.download.hash_removed");

  if (lifetime.expired())
    return;

  DL_TRIGGER_EVENT(download, "event.download.closed");
}

void
DownloadList::resume(Download* download, int flags) {
  // Refuse before the catch below hides the refusal of the view change.
  View::check_unlocked();
  check_contains(download);
  if (retrackers_migration_candidate_activity_held(
      torrent::utils::transform_to_hex_str(download->info()->hash()))) {
    lt_log_print(torrent::LOG_TORRENT_EVENTS, "RR candidate resume held until durable release");
    return;
  }
  if (!stop_intent::owner_restoring() && !stop_intent::terminal_activity_allowed(download) &&
      !stop_intent::held(download).empty()) {
    lt_log_print(torrent::LOG_TORRENT_EVENTS, "Stop claim held native resume");
    return;
  }

  LT_LOG_DOWNLOAD_DEBUG("resuming download : flags:%0x.", flags);

  try {
    // resume is also called from libtorrent completion callbacks. Public RPC
    // entry points perform their own throwing preflight before reaching here.
    erasedata_lifecycle::require_activity(download);

    if (download->completion_deferred() && download->is_hash_checked() && download->is_done()) {
      // An explicit later Resume may discharge a retained notification. Its
      // completion path owns any rehash/resume and must honor nested Stop.
      replay_deferred_completion(download);
      return;
    }

    if (download->download()->info()->is_active())
      return;

    rpc::parse_command_single(rpc::make_target(download), "view.set_visible=active");

    // We need to make sure the flags aren't reset if someone decideds
    // to call resume() while it is hashing, etc.
    if (download->resume_flags() == Download::default_resume_flags)
      download->set_resume_flags(flags);

    // Manual or end-of-download rehashing clears the resume data so
    // we can just start the hashing again without clearing it again.
    //
    // It is also assumed the is_hash_checked flag gets cleared when
    // 'hashing' was set.
    if (!download->is_hash_checked()) {
      // If the hash failed flag wasn't cleared then hashing won't be
      // initiated.
      if (download->is_hash_failed())
        return;

      if (rpc::call_command_value("d.hashing", rpc::make_target(download)) == Download::variable_hashing_stopped)
        rpc::call_command("d.hashing.set", Download::variable_hashing_initial, rpc::make_target(download));

      DL_TRIGGER_EVENT(download, "event.download.hash_queued");
      return;
    }

    // This will never actually do anything due to the above hash check.
    // open_throw(download);

    auto cached_seconds = torrent::this_thread::cached_seconds().count();

    rpc::call_command("d.state_changed.set", cached_seconds, rpc::make_target(download));
    rpc::call_command("d.state_counter.set", rpc::call_command_value("d.state_counter", rpc::make_target(download)) + 1, rpc::make_target(download));

    if (download->is_done()) {
      torrent::Object conn_current = rpc::call_command("d.connection_seed", torrent::Object(), rpc::make_target(download));
      torrent::Object choke_up     = rpc::call_command("d.up.choke_heuristics.seed", torrent::Object(), rpc::make_target(download));
      torrent::Object choke_down   = rpc::call_command("d.down.choke_heuristics.seed", torrent::Object(), rpc::make_target(download));

      if (conn_current.is_string_empty()) conn_current = rpc::call_command("protocol.connection.seed", torrent::Object(), rpc::make_target(download));
      if (choke_up.is_string_empty())     choke_up     = rpc::call_command("protocol.choke_heuristics.up.seed", torrent::Object(), rpc::make_target(download));
      if (choke_down.is_string_empty())   choke_down   = rpc::call_command("protocol.choke_heuristics.down.seed", torrent::Object(), rpc::make_target(download));

      rpc::call_command("d.connection_current.set",    conn_current, rpc::make_target(download));
      rpc::call_command("d.up.choke_heuristics.set",   choke_up, rpc::make_target(download));
      rpc::call_command("d.down.choke_heuristics.set", choke_down, rpc::make_target(download));

    } else {
      torrent::Object conn_current = rpc::call_command("d.connection_leech", torrent::Object(), rpc::make_target(download));
      torrent::Object choke_up     = rpc::call_command("d.up.choke_heuristics.leech", torrent::Object(), rpc::make_target(download));
      torrent::Object choke_down   = rpc::call_command("d.down.choke_heuristics.leech", torrent::Object(), rpc::make_target(download));

      if (conn_current.is_string_empty()) conn_current = rpc::call_command("protocol.connection.leech", torrent::Object(), rpc::make_target(download));
      if (choke_up.is_string_empty())     choke_up     = rpc::call_command("protocol.choke_heuristics.up.leech", torrent::Object(), rpc::make_target(download));
      if (choke_down.is_string_empty())   choke_down   = rpc::call_command("protocol.choke_heuristics.down.leech", torrent::Object(), rpc::make_target(download));

      rpc::call_command("d.connection_current.set",    conn_current, rpc::make_target(download));
      rpc::call_command("d.up.choke_heuristics.set",   choke_up, rpc::make_target(download));
      rpc::call_command("d.down.choke_heuristics.set", choke_down, rpc::make_target(download));

      // For the moment, clear the resume data so we force hash-check
      // on non-complete downloads after a crash. This shouldn't be
      // needed, but for some reason linux 2.6 is very lazy about
      // updating mtime.
      //
      // Disabling this due to the new resume code.
      //      torrent::resume_save_progress(*download->download(), download->download()->bencode()->get_key("libtorrent_resume"), true);
    }

    // If the DHT server is set to auto, start it now.
    if (!download->download()->info()->is_private())
      control->dht_manager()->auto_start();

    // Update the priority to ensure it has the correct
    // seeding/unfinished modifiers.
    download->set_priority(download->priority());
    download->download()->start(download->resume_flags());

    download->set_resume_flags(Download::default_resume_flags);

    DL_TRIGGER_EVENT(download, "event.download.resumed");

  } catch (torrent::local_error& e) {
    LT_LOG_DOWNLOAD_EVENTS("could not resume download : %s", e.what());
  }
}

void
DownloadList::pause(Download* download, int flags) {
  // Refuse before the catch below hides the refusal of the view change.
  View::check_unlocked();
  check_contains(download);

  LT_LOG_DOWNLOAD_DEBUG("pausing download : flags:%0x.", flags);

  auto lifetime = download->lifetime();

  try {

    download->set_resume_flags(Download::default_resume_flags);

    rpc::parse_command_single(rpc::make_target(download), "view.set_not_visible=active");

    // Always clear hashing on pause. When a hashing request is added,
    // it should have cleared the hash resume data.
    if (rpc::call_command_value("d.hashing", rpc::make_target(download)) != Download::variable_hashing_stopped) {
      download->download()->hash_stop();
      rpc::call_command_set_value("d.hashing.set", Download::variable_hashing_stopped, rpc::make_target(download));

      DL_TRIGGER_EVENT(download, "event.download.hash_removed");

      if (lifetime.expired())
        return;
    }

    if (!download->download()->info()->is_active())
      return;

    download->download()->stop(flags);
    torrent::resume_save_progress(*download->download(), download->download()->bencode()->get_key("libtorrent_resume"));

    // TODO: This is actually for pause, not stop... And doesn't get
    // called when the download isn't active, but was in the 'started'
    // view.
    DL_TRIGGER_EVENT(download, "event.download.paused");

    if (lifetime.expired())
      return;

    update_paused_state(download);

    // Save the state after all the slots, etc have been called so we
    // include the modifications they may make.
    //control->core()->download_store()->save(download);

  } catch (torrent::local_error& e) {
    LT_LOG_DOWNLOAD_EVENTS("could not pause download : %s", e.what());
  }
}

void
DownloadList::check_hash(Download* download) {
  // hash_queue() clears the progress of a closed download before any view change.
  View::check_unlocked();
  check_contains(download);

  LT_LOG_DOWNLOAD_DEBUG("checking hash", 0);

  try {
    if (rpc::call_command_value("d.hashing", rpc::make_target(download)) != Download::variable_hashing_stopped)
      return;

    hash_queue(download, Download::variable_hashing_rehash);

  } catch (torrent::local_error& e) {
    LT_LOG_DOWNLOAD_EVENTS("could not check hash : %s", e.what());
  }
}

void
DownloadList::hash_done(Download* download) {
  check_contains(download);

  LT_LOG_DOWNLOAD_DEBUG("hash done", 0);

  if (download->is_hash_checking() || download->is_active())
    throw torrent::internal_error("DownloadList::hash_done(...) download in invalid state.");

  if (!download->is_hash_checked()) {
    download->set_hash_failed(true);

    auto& msg = download->download()->hash_error_message();
    if (!msg.empty())
      download->set_message(msg);

    DL_TRIGGER_EVENT(download, "event.download.hash_failed");
    return;
  }

  // Need to find some sane conditional here. Can we check the total
  // downloaded to ensure something was transferred, thus we didn't
  // just hash an already completed torrent with lacking session data?
  //
  // Perhaps we should use a seperate variable or state, and check
  // that. Thus we can bork the download if the hash check doesn't
  // confirm all the data, avoiding large BW usage on f.ex. the
  // ReiserFS bug with >4GB files.

  int64_t hashing = rpc::call_command_value("d.hashing", rpc::make_target(download));
  rpc::call_command_set_value("d.hashing.set", Download::variable_hashing_stopped, rpc::make_target(download));

  if (download->is_done() && download->download()->info()->is_meta_download())
    return process_meta_download(download);

  constexpr auto completed_unfinished_message =
    "Download registered as completed, but hash check returned unfinished chunks.";
  constexpr auto final_bad_chunks_message = "Hash check on download completion found bad chunks.";
  constexpr auto invalid_hashing_message =
    "Hash check completed but the \"hashing\" variable is in an invalid state.";

  // A pass that found every chunk retires the diagnoses an earlier pass set
  // below. Any other message is left alone.
  if (download->is_done() &&
      (hashing == Download::variable_hashing_initial ||
       hashing == Download::variable_hashing_rehash ||
       hashing == Download::variable_hashing_last) &&
      (download->message() == completed_unfinished_message ||
       download->message() == final_bad_chunks_message ||
       download->message() == invalid_hashing_message))
    download->set_message("");

  switch (hashing) {
  case Download::variable_hashing_initial:
  case Download::variable_hashing_rehash:
    // Normal re/hashing.

    // If the download was previously completed but the files were
    // f.ex deleted, then we clear the state and complete.
    if (rpc::call_command_value("d.complete", rpc::make_target(download)) && !download->is_done()) {
      set_state_stopped(download);
      download->set_message(completed_unfinished_message);
    }

    // Save resume data so we update time-stamps and priorities if
    // they were invalid/changed while loading/hashing.
    rpc::call_command("d.complete.set", (int64_t)download->is_done(), rpc::make_target(download));
    torrent::resume_save_progress(*download->download(), download->download()->bencode()->get_key("libtorrent_resume"));

    if (rpc::call_command_value("d.state", rpc::make_target(download)) == 1) {
      auto lifetime = download->lifetime();

      resume(download, download->resume_flags());

      if (lifetime.expired())
        return;
    }

    break;

  case Download::variable_hashing_last:

    if (download->is_done()) {
      confirm_finished(download);
    } else {
      download->set_message(final_bad_chunks_message);
      LT_LOG_DOWNLOAD_EVENTS("hash check on download completion found bad chunks", 0);

      DL_TRIGGER_EVENT(download, "event.download.hash_final_failed");
    }

    // TODO: Should we skip the 'hash_done' event here?
    return;

  case Download::variable_hashing_stopped:
  default:
    // Either an error or someone wrote to the hashing variable...
    download->set_message(invalid_hashing_message);
    return;
  }

  const auto lifetime = download->lifetime();
  DL_TRIGGER_EVENT(download, "event.download.hash_done");
  if (!lifetime.expired()) replay_deferred_completion(download);
}

void
DownloadList::hash_queue(Download* download, int type) {
  check_contains(download);

  // Refuse before close, resume-data clearing or hashing state changes.
  erasedata_lifecycle::require_activity(download);

  LT_LOG_DOWNLOAD_DEBUG("hash queue : type:%d", type);

  if (rpc::call_command_value("d.hashing", rpc::make_target(download)) != Download::variable_hashing_stopped)
    throw torrent::internal_error("DownloadList::hash_queue(...) hashing already queued.");

  // HACK
  if (download->is_open()) {
    auto lifetime = download->lifetime();

    pause(download, torrent::Download::stop_skip_tracker);

    if (lifetime.expired())
      return;

    download->download()->close();

    DL_TRIGGER_EVENT(download, "event.download.hash_removed");

    if (lifetime.expired())
      return;

    DL_TRIGGER_EVENT(download, "event.download.closed");

    if (lifetime.expired())
      return;
  }

  torrent::resume_clear_progress(*download->download(), download->download()->bencode()->get_key("libtorrent_resume"));

  download->set_hash_failed(false);
  rpc::call_command_set_value("d.hashing.set", type, rpc::make_target(download));

  if (download->is_open())
    throw torrent::internal_error("DownloadList::hash_clear(...) download still open.");

  // If any more stuff is added here, make sure resume etc are still
  // correct.
  DL_TRIGGER_EVENT(download, "event.download.hash_queued");
}

bool
DownloadList::defer_completion_if_held(Download* download) {
  try {
    if (!erasedata_lifecycle::activity_held(download)) return false;
  } catch (const torrent::local_error& e) {
    // These callers own a real completion notification. An unknown admission
    // retains it just like a positive hold; it cannot authorize completion.
    download->set_message("erasedata: deferred completion admission unavailable for " +
        torrent::utils::transform_to_hex_str(download->info()->hash()) + ".erasedata; payload retained");
    LT_LOG_DOWNLOAD_EVENTS("deferred completion held : %s", e.what());
  }
  download->set_completion_deferred(true);
  return true;
}

void
DownloadList::received_finished(Download* download) {
  check_contains(download);

  LT_LOG_DOWNLOAD_DEBUG("received finished", 0);

  if (defer_completion_if_held(download)) return;

  try {
    if (rpc::call_command_value("pieces.hash.on_completion"))
      hash_queue(download, Download::variable_hashing_last);
    else
      confirm_finished(download);
  } catch (const torrent::local_error& e) {
    LT_LOG_DOWNLOAD_EVENTS("completion held : %s", e.what());
  }
}

// The download must be open when we call this function.
void
DownloadList::confirm_finished(Download* download) {
  check_contains(download);

  LT_LOG_DOWNLOAD_DEBUG("confirming finished", 0);

  if (defer_completion_if_held(download)) return;

  if (download->download()->info()->is_meta_download())
    return process_meta_download(download);

  rpc::call_command("d.complete.set", (int64_t)1, rpc::make_target(download));

  // Clean up these settings:
  torrent::Object conn_current = rpc::call_command("d.connection_seed", torrent::Object(), rpc::make_target(download));
  torrent::Object choke_up     = rpc::call_command("d.up.choke_heuristics.seed", torrent::Object(), rpc::make_target(download));
  torrent::Object choke_down   = rpc::call_command("d.down.choke_heuristics.seed", torrent::Object(), rpc::make_target(download));

  if (conn_current.is_string_empty()) conn_current = rpc::call_command("protocol.connection.seed", torrent::Object(), rpc::make_target(download));
  if (choke_up.is_string_empty())     choke_up     = rpc::call_command("protocol.choke_heuristics.up.seed", torrent::Object(), rpc::make_target(download));
  if (choke_down.is_string_empty())   choke_down   = rpc::call_command("protocol.choke_heuristics.down.seed", torrent::Object(), rpc::make_target(download));

  // libtorrent refuses initial seeding on an active download.
  if (download->is_active() && conn_current.as_string() == "initial_seed")
    conn_current = "seed";

  rpc::call_command("d.connection_current.set",    conn_current, rpc::make_target(download));
  rpc::call_command("d.up.choke_heuristics.set",   choke_up, rpc::make_target(download));
  rpc::call_command("d.down.choke_heuristics.set", choke_down, rpc::make_target(download));

  download->set_priority(download->priority());

  if (rpc::call_command_value("d.peers_min", rpc::make_target(download)) == rpc::call_command_value("throttle.min_peers.normal") &&
      rpc::call_command_value("throttle.min_peers.seed") >= 0)
    rpc::call_command("d.peers_min.set", rpc::call_command("throttle.min_peers.seed"), rpc::make_target(download));

  if (rpc::call_command_value("d.peers_max", rpc::make_target(download)) == rpc::call_command_value("throttle.max_peers.normal") &&
      rpc::call_command_value("throttle.max_peers.seed") >= 0)
    rpc::call_command("d.peers_max.set", rpc::call_command("throttle.max_peers.seed"), rpc::make_target(download));

  // Do this before the slots are called in case one of them closes
  // the download.
  //
  // Obsolete.
  if (!download->is_active() && rpc::call_command_value("session.on_completion") != 0)
    session_thread::manager()->save_resume_download(download);

  // Send the completed request before resuming so we don't reset the
  // up/downloaded baseline.
  download->download()->send_completed();

  // Finished callbacks may erase, Pause or Close the download. The existing
  // Stop token fences the implicit restart even when d.state remains one.
  const auto lifetime = download->lifetime();
  std::optional<std::string> generation;
  try {
    generation = rpc::call_command_string("d.stop_generation", rpc::make_target(download));
  } catch (const torrent::local_error& e) {
    LT_LOG_DOWNLOAD_EVENTS("completion restart held : %s", e.what());
  }

  DL_TRIGGER_EVENT(download, "event.download.finished");

  if (lifetime.expired())
    return;

//   if (download->resume_flags() != Download::default_resume_flags)
//     throw torrent::internal_error("DownloadList::confirm_finished(...) download->resume_flags() != Download::default_resume_flags.");

  // See #1292.
  //
  // Just reset the value for the moment. If a torrent finishes while
  // others are hashing, or some other situtation that causes resume
  // flag to change could cause the state to be invalid.
  //
  // TODO: Add a check when setting the flags to see if the torrent is
  // being hashed.
  download->set_resume_flags(Download::default_resume_flags);

  if (!download->is_active() && rpc::call_command_value("d.state", rpc::make_target(download)) == 1 && generation) {
    try {
      if (*generation == rpc::call_command_string("d.stop_generation", rpc::make_target(download)))
        resume(download,
               torrent::Download::start_no_create |
               torrent::Download::start_skip_tracker |
               torrent::Download::start_keep_baseline);
    } catch (const torrent::local_error& e) {
      LT_LOG_DOWNLOAD_EVENTS("completion restart held : %s", e.what());
    }
  }
}

void
DownloadList::process_meta_download(Download* download) {
  LT_LOG_DOWNLOAD_DEBUG("processing meta download", 0);

  const auto lifetime = download->lifetime();
  try {
    const auto target = rpc::make_target(download);
    const auto hash = rpc::call_command_string("d.hash", target);
    const auto session = rpc::call_command_string("session.path");
    const auto generation = stop_intent::counter(session, hash, false);
    // A stopped load may temporarily start solely to fetch metadata. Preserve
    // its declared start flag while still honoring a later explicit user Stop.
    int64_t start = download->bencode()->get_key("rtorrent_meta_download").get_key_value("start");
    if (rpc::call_command_value("d.state", target) == 0) start = 0;

    // The metadata placeholder shares the real torrent's hash. Its replacement
    // must not create a user Stop generation inherited by the real download.
    {
      stop_intent::ScopedSuppress suppress;
      rpc::parse_command_single(target, "view.set_visible=stopped");
      if (lifetime.expired())
        return;
      rpc::call_command("d.close", torrent::Object(), target);
      if (lifetime.expired())
        return;
    }

    std::string  metafile = (*download->file_list()->begin())->frozen_path().str();

    std::fstream file(metafile.c_str(), std::ios::in | std::ios::binary);

    if (!file.is_open()) {
      LT_LOG_DOWNLOAD_EVENTS("could not read download metadata : %s", metafile.c_str());
      return;
    }

    auto bencode = std::make_unique<torrent::Object>(torrent::Object::create_map());
    file >> bencode->insert_key("info", torrent::Object());

    if (file.fail()) {
      LT_LOG_DOWNLOAD_EVENTS("could not create download, the input is not a valid torrent : %s", metafile.c_str());
      return;
    }

    file.close();

    // Preserve the placeholder until erasure succeeds; a refused conversion can be retried.
    bencode->insert_key("rtorrent_meta_download", download->bencode()->get_key("rtorrent_meta_download"));
    if (download->bencode()->has_key("announce"))
      bencode->insert_key("announce", download->bencode()->get_key("announce"));
    if (download->bencode()->has_key("announce-list"))
      bencode->insert_key("announce-list", download->bencode()->get_key("announce-list"));

    const bool retire_counter = !retrackers_migration_pending(hash) &&
        !retrackers_migration_original_claim_active(hash) &&
        !retrackers_migration_uncommitted_candidate(hash);
    // Stage fallible factory setup while the placeholder can still be retained.
    auto factory = control->core()->prepare_download_from_meta_download(std::move(bencode), metafile);
    // Keep the generation until all close/erase/view callbacks have finished.
    // An explicit Stop from any of them must transfer to the real download.
    erase_meta_placeholder(download);
    try {
      if (stop_intent::counter(session, hash, false) != generation) start = 0;
    } catch (const std::exception& e) {
      start = 0;
      lt_log_print(torrent::LOG_ERRORS, "Metadata stop generation unavailable for %s; replacement stopped: %s",
                   hash.c_str(), e.what());
    }
    factory->set_start(start);
    if (retire_counter) {
      try { stop_intent::erase_counter(session, hash); }
      catch (const std::exception& e) {
        lt_log_print(torrent::LOG_ERRORS, "Cannot retire metadata stop intent for %s: %s", hash.c_str(), e.what());
      }
    }

    factory->commit();
    factory.release(); // The scheduled completion callback now owns the factory.
  } catch (const std::exception& e) {
    // Metadata callbacks run outside an RPC boundary. Contain a refused claim
    // or malformed metadata instead of unwinding the daemon's main loop.
    lt_log_print(torrent::LOG_ERRORS, "Metadata replacement held: %s", e.what());
    if (!lifetime.expired()) download->set_message(std::string("Metadata replacement held: ") + e.what());
  }

}

}
