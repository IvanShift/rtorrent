#ifndef RTORRENT_RETRACKERS_MIGRATION_PENDING_H
#define RTORRENT_RETRACKERS_MIGRATION_PENDING_H

#include <string>

namespace core { class Download; }
bool retrackers_migration_late_save(core::Download* download);
bool retrackers_migration_original_claim_active(const std::string& hash);
// Durably cancels the exact queued, uncaptured original claim of a download
// before a direct user erase. Returns false when the claim has no queue token,
// so the erase stays refused; throws torrent::local_error (an input or storage
// error) when cancellation is refused or fails, and the claim keeps the source.
bool retrackers_migration_cancel_queued_original(core::Download* download);
extern thread_local std::string rr_native_capture_erase_hash;
bool retrackers_migration_uncommitted_candidate(const std::string& hash);
bool retrackers_migration_candidate_activity_held(const std::string& hash);
void retrackers_migration_retire_released(core::Download* download);
// After an erase removed the ordinary triple, queues the hash so that a batch
// a few seconds later durably retires its owner tombstone and finalized
// receipt when nothing else in the journal names the hash. Failures are
// logged; startup retries. The flush runs a pending batch now (shutdown).
void retrackers_migration_retire_erased_owner(const std::string& hash);
void retrackers_migration_flush_erased_owners();
// Unschedules the watcher that recovers transactions of exited workers.
void retrackers_migration_stop_worker_watch();
bool retrackers_migration_repair_prepared_loaded(core::Download* download);

// A durable rollback snapshot reserves this info hash until explicit resolution.
bool retrackers_migration_pending(const std::string& hash);
std::string retrackers_migration_candidate_digest(const std::string& hash);
std::string retrackers_migration_target_source(const std::string& hash);
bool retrackers_migration_source_reserved(const std::string& hash);
bool retrackers_migration_prepared_session(core::Download* download, const std::string& hash);
bool retrackers_migration_committed(const std::string& old_hash, const std::string& new_hash);
bool retrackers_migration_restored_source(const std::string& hash);

#endif
