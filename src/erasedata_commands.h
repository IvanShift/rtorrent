#pragma once

namespace {
namespace ed = core::erasedata_intent;
namespace edlife = core::erasedata_lifecycle;
using EraseRecord = ed::Record;
EraseRecord erasedata_args(const torrent::Object::list_type& args, bool worker = false) {
  if (args.size() != (worker ? 7 : 6) ||
      !std::all_of(args.begin(), args.end(), [](const auto& v) { return v.is_string(); }))
    throw torrent::input_error("erasedata: exact H/I/G/U/N/D tuple required");
  EraseRecord r{args[0].as_string(), args[1].as_string(), args[2].as_string(),
      args[3].as_string(), args[4].as_string(), args[5].as_string(), 'P'};
  if (!ed::valid(r)) throw torrent::input_error("erasedata: noncanonical intent tuple");
  return r;
}
std::string erasedata_identity(const EraseRecord& r) { return "erasedata:" + r.hash + ":" + r.nonce; }
core::Download* erasedata_live(const EraseRecord& r) {
  return control->core()->download_list()->find_hex_ptr(r.hash.c_str());
}
std::string erasedata_relation(const EraseRecord& r) {
  auto* live = erasedata_live(r);
  return !live ? "absent" : edlife::incarnation(live) == r.incarnation ? "exact" : "different";
}
EraseRecord erasedata_require(const EraseRecord& wanted) {
  EraseRecord actual;
  if (!ed::read(edlife::session(), wanted.hash, &actual)) throw torrent::input_error("erasedata: missing intent; payload retained");
  if (!ed::same(actual, wanted)) throw torrent::input_error("erasedata: intent mismatch; payload retained");
  return actual;
}
void erasedata_guards(core::Download* d) {
  stop_intent::require_unleased(d);
  const auto h = edlife::hash(d);
  core::file_jobs::Record move;
  if (stop_intent::checker_owned(d) || rr_queued_original(h) != "none" ||
      retrackers_migration_pending(h) || retrackers_migration_source_reserved(h) ||
      !retrackers_migration_target_source(h).empty() ||
      core::file_jobs::read(edlife::session(), h, &move))
    throw torrent::input_error("erasedata: independent file-operation owner held");
  if (d->is_erasing()) throw torrent::input_error("erasedata: partial erase held; exact recovery required");
}
std::string erasedata_prepare(const torrent::Object::list_type& args) {
  const auto r = erasedata_args(args);
  auto* d = erasedata_live(r);
  if (!d || edlife::incarnation(d) != r.incarnation)
    throw torrent::input_error("erasedata: original incarnation changed");
  core::SessionPathFd lifetime(ed::lock(edlife::session(), r.hash, true));
  EraseRecord actual;
  if (ed::read(edlife::session(), r.hash, &actual)) {
    if (!ed::same(actual, r)) throw torrent::input_error("erasedata: another intent retained");
    return ed::phase_name(actual.phase);
  }
  erasedata_guards(d);
  // A crash before erase must restore this exact identity, held for reconciliation.
  session_thread::manager()->save_full_download_durable(d);
  ed::write(edlife::session(), r.hash, r, true);
  d->set_message("erasedata: prepared " + edlife::description(r) +
      "; payload retained until guarded erase; ordinary Remove/re-add cancels");
  if (core::migration_test_cut("erasedata-after-prepare")) ::_exit(137);
  return "prepared";
}
std::string erasedata_cancel(const torrent::Object::list_type& args) {
  const auto wanted = erasedata_args(args);
  core::SessionPathFd lifetime(ed::lock(edlife::session(), wanted.hash, true));
  const auto actual = erasedata_require(wanted);
  if (actual.phase != 'P') throw torrent::input_error("erasedata: cancel phase refused; exact erase recovery required");
  auto* d = erasedata_live(actual);
  if (!d || edlife::incarnation(d) != actual.incarnation)
    throw torrent::input_error("erasedata: cancel original incarnation changed");
  ed::remove(edlife::session(), actual.hash, actual);
  if (d->message().rfind("erasedata:", 0) == 0) d->set_message({});
  return "cancelled";
}
torrent::Object erasedata_state(const torrent::Object::list_type& args) {
  const auto wanted = erasedata_args(args);
  EraseRecord actual;
  const auto phase = !ed::read(edlife::session(), wanted.hash, &actual) ? "missing" :
      !ed::same(actual, wanted) ? "mismatch" : ed::phase_name(actual.phase);
  auto result = torrent::Object::create_map();
  result.insert_key("phase", std::string(phase));
  result.insert_key("live_relation", erasedata_relation(wanted));
  return result;
}
std::string erasedata_erase(const torrent::Object::list_type& args) {
  const auto wanted = erasedata_args(args);
  core::SessionPathFd lifetime(ed::lock(edlife::session(), wanted.hash, true));
  auto actual = erasedata_require(wanted);
  if (actual.phase == 'C') return "committed";
  auto* d = erasedata_live(actual);
  if (d) {
    if (edlife::incarnation(d) != actual.incarnation)
      throw torrent::input_error("erasedata: later incarnation refused; payload retained");
    erasedata_guards(d);
    actual.phase = 'E';
    ed::write(edlife::session(), actual.hash, actual);
    if (core::migration_test_cut("erasedata-before-erase")) ::_exit(137);
    const auto target = rpc::make_target(d);
    edlife::ScopedErase owner(d, actual.nonce);
    // Keep existing tied deletion and unrelated erased hooks in this one callback.
    rpc::call_command("d.custom5.set", std::string(), target);
    rpc::call_command("d.delete_tied", torrent::Object(), target);
    control->core()->download_list()->erase_ptr(d);
    if (core::migration_test_cut("erasedata-after-erase")) ::_exit(137);
  } else if (actual.phase != 'E') {
    throw torrent::input_error("erasedata: prepared original absent; no erase attestation");
  }
  if (erasedata_live(actual))
    throw torrent::input_error("erasedata: partial erase held: " + edlife::description(actual));
  // SessionManager cancels pending stores. Its unlink helper ignores errors;
  // positively attest every ordinary residue before committing deletion authority.
  try { ed::session_absent(edlife::session(), actual.hash); }
  catch (const torrent::input_error& e) {
    throw torrent::input_error(std::string(e.what()) + "; retained " + edlife::description(actual) +
        "; payload retained; repair named session residue then retry exact erase recovery");
  }
  actual.phase = 'C';
  ed::write(edlife::session(), actual.hash, actual);
  return "committed";
}
void erasedata_worker(const EraseRecord& r, const std::string& proof) {
  const auto actor = core::retrackers_process_pool::worker_pid(erasedata_identity(r));
  if (actor <= 0 || proof != std::to_string(actor))
    throw torrent::input_error("erasedata: exact native worker required");
  core::SessionPathFd dir(stop_intent::open_directory(edlife::session(), false));
  struct stat stable{}, inherited{};
  const auto name = r.hash + ".erasedata.lock";
  const auto path = "/proc/" + std::to_string(actor) + "/fd/198";
  if (::fstatat(dir.get(), name.c_str(), &stable, AT_SYMLINK_NOFOLLOW) != 0 ||
      ::stat(path.c_str(), &inherited) != 0 || stable.st_dev != inherited.st_dev ||
      stable.st_ino != inherited.st_ino || stable.st_nlink != 1 || !S_ISREG(stable.st_mode) ||
      stable.st_uid != ::geteuid() || (stable.st_mode & 07777) != 0600)
    throw torrent::input_error("erasedata: inherited lifetime descriptor changed");
  // Inode equality alone does not prove the inherited description still holds
  // its flock. Probe through a separate OFD; never unlock the inherited OFD.
  try {
    const int probe = ed::lock(edlife::session(), r.hash, false);
    if (probe >= 0) ::close(probe);
    throw torrent::input_error("erasedata: inherited lifetime is unlocked");
  } catch (const torrent::input_error& e) {
    if (std::string(e.what()) != "erasedata: collector-busy") throw;
  }
}
std::string erasedata_collect_start(const torrent::Object::list_type& args) {
  const auto wanted = erasedata_args(args);
  const auto r = erasedata_require(wanted);
  if (r.phase != 'C' || erasedata_live(r)) return "busy";
  const auto identity = erasedata_identity(r);
  core::retrackers_process_pool::reap();
  // An erasedata child exit retains C. Retrying uses the same durable intent;
  // no captured obligation is acknowledged by the fact that its child exited.
  auto& exited = core::retrackers_process_pool::exited_workers;
  exited.erase(std::remove_if(exited.begin(), exited.end(), [&](const auto& e) { return e.identity == identity; }), exited.end());
  if (core::retrackers_process_pool::active(identity)) return "active";
  const int lock = ed::lock(edlife::session(), r.hash, false);
  if (lock < 0) throw torrent::input_error("erasedata: stable lifetime lock missing");
  core::SessionPathFd lifetime(lock);
  ed::session_absent(edlife::session(), r.hash);
  const auto result = core::retrackers_process_pool::launch(identity,
      {core::retrackers_process_pool::php_executable,
       core::retrackers_process_pool::application_root + "/plugins/erasedata/collect_worker.php",
       r.user, r.hash, r.incarnation, r.generation, r.nonce, r.digest}, false, lifetime.get());
  if (result == "spawned") rr_watch_workers_schedule();
  return result;
}
std::string erasedata_collect_begin(const torrent::Object::list_type& args) {
  const auto wanted = erasedata_args(args, true), r = erasedata_require(wanted);
  erasedata_worker(r, args[6].as_string());
  if (r.phase != 'C' || erasedata_live(r)) throw torrent::input_error("erasedata: collection admission changed");
  ed::session_absent(edlife::session(), r.hash);
  return "active";
}
std::string erasedata_collect_finish(const torrent::Object::list_type& args) {
  const auto wanted = erasedata_args(args, true), r = erasedata_require(wanted);
  erasedata_worker(r, args[6].as_string());
  if (r.phase != 'C' || erasedata_live(r)) throw torrent::input_error("erasedata: finish admission changed");
  ed::session_absent(edlife::session(), r.hash);
  // The application has incapacitated its manifest before this callback. The
  // stable lock still fences admission until this worker and every helper exit.
  ed::remove(edlife::session(), r.hash, r);
  return "finished";
}
void initialize_erasedata_commands() {
  CMD2_ANY_LIST("system.erasedata.prepare", [](auto, const auto& a) { return erasedata_prepare(a); });
  CMD2_ANY_LIST("system.erasedata.cancel", [](auto, const auto& a) { return erasedata_cancel(a); });
  CMD2_ANY_LIST("system.erasedata.state", [](auto, const auto& a) { return erasedata_state(a); });
  CMD2_ANY_LIST("system.erasedata.erase", [](auto, const auto& a) { return erasedata_erase(a); });
  CMD2_ANY_LIST("system.erasedata.collect_start", [](auto, const auto& a) { return erasedata_collect_start(a); });
  CMD2_ANY_LIST("system.erasedata.collect_begin", [](auto, const auto& a) { return erasedata_collect_begin(a); });
  CMD2_ANY_LIST("system.erasedata.collect_finish", [](auto, const auto& a) { return erasedata_collect_finish(a); });
}
}
