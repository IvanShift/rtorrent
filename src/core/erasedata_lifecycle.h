#pragma once

#include <memory>

#include "core/erasedata_intent.h"
#include "core/download.h"
#include "rpc/parse_commands.h"

namespace core::erasedata_lifecycle {
inline thread_local Download* erase_owner = nullptr;
inline thread_local std::string erase_nonce;

inline std::string session() { return rpc::call_command_string("session.path"); }
inline std::string hash(Download* d) { return rpc::call_command_string("d.hash", rpc::make_target(d)); }
inline std::string incarnation(Download* d) { return rpc::call_command_string("d.incarnation", rpc::make_target(d)); }
inline std::string description(const erasedata_intent::Record& r) {
  return r.hash + ".erasedata H=" + r.hash + " G=" + r.generation + " U=" + r.user + " N=" + r.nonce;
}
struct ScopedErase {
  ScopedErase(Download* owner, const std::string& nonce) : previous(erase_owner), previous_nonce(erase_nonce) {
    if (erase_owner) throw torrent::input_error("erasedata: nested erase owner refused");
    erase_owner = owner; erase_nonce = nonce;
  }
  ~ScopedErase() { erase_owner = previous; erase_nonce = previous_nonce; }
  Download* previous;
  std::string previous_nonce;
};
inline void ordinary_erase(Download* d) {
  const auto h = hash(d), s = session();
  erasedata_intent::Record record;
  if (erase_owner == d) {
    if (!erasedata_intent::read(s, h, &record) || record.nonce != erase_nonce ||
        record.incarnation != incarnation(d) || record.phase != 'E')
      throw torrent::input_error("erasedata: exact erase ownership changed");
    return;
  }
  const int fd = erasedata_intent::lock(s, h, false);
  // Existing locks must be checked even after collect_finish removed E.
  std::unique_ptr<SessionPathFd> lifetime(fd < 0 ? nullptr : new SessionPathFd(fd));
  if (erasedata_intent::read(s, h, &record)) {
    erasedata_intent::remove(s, h, record);
    lt_log_print(torrent::LOG_NOTICE, "erasedata: ordinary Remove revoked %s; payload retained", description(record).c_str());
  }
}
inline void require_activity(Download* d) {
  erasedata_intent::Record record;
  if (erasedata_intent::read(session(), hash(d), &record)) {
    const auto message = "erasedata: recovery held: " + description(record) +
        "; payload retained; use exact erase recovery, ordinary Remove or re-add to cancel";
    d->set_message(message);
    throw torrent::input_error(message);
  }
}
// Called before insertion, load-tail commands or inserted hooks. A true result
// is an exact original session restore for reconciliation, never an autostart.
inline bool admit(const std::string& s, const std::string& h, const std::string& i, bool saved, bool saved_identity_valid) {
  const int fd = erasedata_intent::lock(s, h, false);
  std::unique_ptr<SessionPathFd> lifetime(fd < 0 ? nullptr : new SessionPathFd(fd));
  erasedata_intent::Record record;
  if (!erasedata_intent::read(s, h, &record)) return false;
  if (fd < 0) throw torrent::input_error("erasedata: intent has no stable lifetime lock: " + description(record));
  if (saved && !saved_identity_valid)
    throw torrent::input_error("erasedata: unproven session identity refused: " + description(record) +
        "; payload retained; inspect original session metadata");
  if (saved && record.incarnation == i) {
    if (record.phase == 'C')
      throw torrent::input_error("erasedata: erased session refused: " + description(record) + "; repair retained session residue");
    lt_log_print(torrent::LOG_WARN, "erasedata: exact original session held: %s; reconcile or ordinary Remove/re-add to cancel", description(record).c_str());
    return true;
  }
  // Main-thread cancellation is durable before the new object becomes visible.
  erasedata_intent::remove(s, h, record);
  lt_log_print(torrent::LOG_NOTICE, "erasedata: new admission revoked %s; old payload deletion permanently cancelled", description(record).c_str());
  return false;
}
}
