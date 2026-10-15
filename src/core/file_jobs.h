#pragma once

#include <ctime>
#include <map>
#include "core/retrackers_queue_receipts.h"
#include "stop_intent.h"
#include "core/download_list.h"
#include "core/retrackers_migration_creation.h"

// Daemon-owned Move jobs are independent of the download custom map: an
// internal retrackers erase/load must not roll them back with its snapshot.
namespace core::file_jobs {
struct Record { std::string user, queue, local, retry; uint64_t enqueued = 0; };
inline std::map<std::string, Record> records;
inline std::map<std::string, std::string> held;
inline const char* const reasons[] = {"source-not-ready", "transport-unavailable",
    "preflight-refused", "storage-failed", "worker-exit"};
inline bool hex(const std::string& s, size_t n, bool upper = false) {
  return s.size() == n && s.find_first_not_of(upper ? "0123456789ABCDEF" : "0123456789abcdef") == std::string::npos;
}
inline bool retry_parse(const std::string& s, const std::string& q, RetrackersRetry* out) {
  if (!queue_retry_parse(s, q, out, reasons, true)) return false;
  RetrackersRetry r;
  queue_retry_parse(s, q, &r, reasons, true);
  return !r.terminal() || !retrackers_retry_wait_reason(r.reason);
}
inline bool parse(const std::string& bytes, Record* out) {
  if (bytes.size() > 256 || bytes.empty() || bytes.back() != '\n' || bytes.rfind("m1:", 0) != 0) return false;
  Record r;
  if (bytes.size() < 144 || bytes[67] != ':' || bytes[100] != ':' || bytes[141] != ':') return false;
  r.user = bytes.substr(3,64); r.queue = bytes.substr(68,32); r.local = bytes.substr(101,40);
  if (!hex(r.user,64) || !hex(r.queue,32) || !stop_intent::valid_hash(r.local)) return false;
  const auto end = bytes.find(':',142);
  if (end == std::string::npos || end == 142 || end - 142 > 10) return false;
  const auto stamp = bytes.substr(142,end-142);
  if (stamp[0] == '0' || stamp.find_first_not_of("0123456789") != std::string::npos) return false;
  for (char c : stamp) r.enqueued = r.enqueued * 10 + c - '0';
  r.retry = bytes.substr(end+1,bytes.size()-end-2);
  RetrackersRetry retry;
  if (!r.retry.empty() && !retry_parse(r.retry,r.queue,&retry)) return false;
  if (out) *out = std::move(r);
  return true;
}
inline std::string format(const Record& r) {
  return "m1:"+r.user+":"+r.queue+":"+r.local+":"+std::to_string(r.enqueued)+":"+r.retry+"\n";
}
inline bool read(const std::string& session, const std::string& hash, Record* out) try {
  if (!hex(hash,40,true)) throw torrent::input_error("autotools: move hash invalid");
  if (session.empty()) return false;
  const int directory=stop_intent::open_directory(session,false);
  if (directory<0) return false;
  stop_intent::Fd dir(directory);
  const int opened=::openat(dir.get(),(hash+".move").c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
  if (opened<0 && errno==ENOENT) { records.erase(hash); held.erase(hash); return false; }
  stop_intent::Fd file(opened);
  struct stat st{};
  if (file.get()<0 || ::fstat(file.get(),&st)!=0 || !S_ISREG(st.st_mode) || st.st_uid!=::geteuid() ||
      (st.st_mode&07777)!=0600 || st.st_nlink!=1 || st.st_size<=0 || st.st_size>256)
    throw torrent::input_error("autotools: move record-unsafe");
  std::string bytes(st.st_size,'\0');
  Record r;
  if (::read(file.get(),bytes.data(),bytes.size())!=static_cast<ssize_t>(bytes.size()) || !parse(bytes,&r))
    throw torrent::input_error("autotools: move record-malformed");
  records[hash]=r; held.erase(hash); if(out) *out=r; return true;
} catch (const std::exception&) { throw torrent::input_error("autotools: move record-unreadable"); }
inline void sync_directory(int directory) {
  if (migration_test_cut("move-directory-sync-failed") || ::fsync(directory)!=0)
    throw torrent::input_error("autotools: move storage-failed");
}
inline void confirm_publication(const std::string& session) {
  stop_intent::Fd dir(stop_intent::open_directory(session,false));
  sync_directory(dir.get());
}
inline void write(const std::string& session, const std::string& hash, const Record& record, bool create=false) try {
  const std::string bytes=format(record);
  if (!hex(hash,40,true) || !parse(bytes,nullptr)) throw torrent::input_error("autotools: move record invalid");
  stop_intent::Fd dir(stop_intent::open_directory(session,true));
  stop_intent::TemporaryEntry temp{dir.get(),hash+".move."+random_hex16<std::runtime_error>(nullptr,"move nonce unavailable")+".tmp"};
  stop_intent::Fd file(::openat(dir.get(),temp.name.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
  if(file.get()<0) throw torrent::input_error("autotools: move storage-failed");
  temp.owned=true;
  if(::write(file.get(),bytes.data(),bytes.size())!=static_cast<ssize_t>(bytes.size()) || ::fdatasync(file.get())!=0)
    throw torrent::input_error("autotools: move storage-failed");
  if(migration_test_cut("before-move-record-publication")) ::_exit(137);
  const int result=create ? rename_session_entry_noreplace(dir.get(),temp.name,hash+".move") :
      ::renameat(dir.get(),temp.name.c_str(),dir.get(),(hash+".move").c_str());
  if(result!=0) throw torrent::input_error("autotools: move storage-failed");
  sync_directory(dir.get());
  temp.name.clear(); records[hash]=record; held.erase(hash);
  if(migration_test_cut("after-move-record-publication")) ::_exit(137);
} catch (const std::exception&) { throw torrent::input_error("autotools: move storage-failed"); }
inline void remove(const std::string& session, const std::string& hash, const std::string& queue) try {
  Record r;
  if(!read(session,hash,&r)) return;
  if(r.queue!=queue) throw torrent::input_error("autotools: move queue changed");
  stop_intent::Fd dir(stop_intent::open_directory(session,false));
  if(::unlinkat(dir.get(),(hash+".move").c_str(),0)!=0 || ::fsync(dir.get())!=0)
    throw torrent::input_error("autotools: move storage-failed");
  records.erase(hash); held.erase(hash);
} catch (const std::exception&) { throw torrent::input_error("autotools: move storage-failed"); }
inline void startup(const std::string& session) {
  if(session.empty()) return;
  const int directory=stop_intent::open_directory(session,false);
  if(directory<0) return;
  stop_intent::Fd dir(directory);
  DIR* entries=::fdopendir(::dup(dir.get()));
  if(!entries) throw torrent::input_error("autotools: move scan-failed");
  std::vector<std::string> names;
  while(auto* entry=::readdir(entries)) names.emplace_back(entry->d_name);
  ::closedir(entries);
  for(const auto& name:names) {
    if (name.size()==82 && hex(name.substr(0,40),40,true) && name.substr(40,6)==".move." &&
        hex(name.substr(46,32),32) && name.substr(78)==".tmp") {
      struct stat st{};
      if (::fstatat(dir.get(),name.c_str(),&st,AT_SYMLINK_NOFOLLOW)==0 && S_ISREG(st.st_mode) &&
          st.st_uid==::geteuid() && (st.st_mode&07777)==0600 && st.st_nlink==1) {
        if (::unlinkat(dir.get(),name.c_str(),0)!=0 || ::fsync(dir.get())!=0)
          throw torrent::input_error("autotools: move temporary-cleanup-failed");
      }
      continue;
    }
    if(name.size()!=45 || name.substr(40)!=".move" || !hex(name.substr(0,40),40,true)) continue;
    const auto hash=name.substr(0,40);
    try {
      Record record;
      if (!read(session,hash,&record)) continue;
      stop_intent::Fd parent(open_trusted_session_directory(session));
      struct stat saved{}, claim{};
      const bool no_session=::fstatat(parent.get(),(hash+".torrent").c_str(),&saved,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;
      const bool no_claim=::fstatat(dir.get(),(hash+".claim").c_str(),&claim,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;
      if(no_session && no_claim && !control->core()->download_list()->find_hex_ptr(hash.c_str()) &&
         !retrackers_migration_source_reserved(hash) && !retrackers_migration_pending(hash) &&
         retrackers_migration_target_source(hash).empty()) {
        remove(session,hash,record.queue);
        lt_log_print(torrent::LOG_EVENTS,"autotools: move %s cancelled: torrent-absent",hash.c_str());
      }
    }
    catch(const std::exception&) {
      held[hash]="record-malformed";
      lt_log_print(torrent::LOG_ERRORS,"autotools: move %s held: record-malformed; inspect %s/.rutorrent-stop-intents/%s.move",hash.c_str(),session.c_str(),hash.c_str());
    }
  }
}
inline std::string message(const std::string& session,const std::string& hash) {
  if (!records.count(hash) && !held.count(hash)) return {};
  try {
    Record r; if(!read(session,hash,&r)) return {};
    RetrackersRetry retry;
    if(r.retry.empty()) return {};
    retry_parse(r.retry,r.queue,&retry);
    return retry.terminal() ? "autotools: move retries exhausted: "+retry.reason+"; inspect journal and reset with tools/autotools-move-reset.php "+hash+" --apply" :
        "autotools: move waiting: "+retry.reason+"; retried automatically";
  } catch(const std::exception&) {
    return "autotools: move held: record-malformed; inspect "+session+"/.rutorrent-stop-intents/"+hash+".move";
  }
}
inline void erase_download(core::Download* owner) {
  const auto [session,hash]=stop_intent::identity(owner);
  if(session.empty() || rr_native_capture_erase_hash==hash ||
     (RetrackersMigrationCreation::abort_erase_permitted(owner) && retrackers_migration_uncommitted_candidate(hash))) return;
  Record r;
  try { if(read(session,hash,&r)) remove(session,hash,r.queue); }
  catch(const std::exception&) { throw torrent::input_error("autotools: Move record blocks direct erase; inspect session .move record"); }
}
}
