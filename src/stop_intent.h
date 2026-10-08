#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <vector>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <sys/stat.h>
#include <unistd.h>
#include <torrent/exceptions.h>
#include <torrent/utils/log.h>
#include "control.h"
#include "core/manager.h"
#include "core/session_path.h"
#include "globals.h"
#include "core/download.h"
#include "core/incarnation.h"
#include "core/retrackers_migration_pending.h"
#include "rpc/parse_commands.h"

namespace stop_intent {
using Fd = core::SessionPathFd;

inline int open_directory(const std::string& session, bool create) {
  Fd parent(core::open_trusted_session_directory(session));
  struct stat st{};
  constexpr const char* name = ".rutorrent-stop-intents";
  const std::string path = core::session_path_child(session, name);
  if (create && ::mkdirat(parent.get(), name, 0700) == 0) {
    if (::fsync(parent.get()) != 0) throw std::runtime_error("cannot sync intent directory entry: " + path + ": " + std::strerror(errno));
  } else if (create && errno != EEXIST) {
    throw std::runtime_error("cannot create intent directory: " + path + ": " + std::strerror(errno));
  }
  const int dir = ::openat(parent.get(), name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (dir < 0 && !create && errno == ENOENT) return -1;
  if (dir < 0) throw std::runtime_error("cannot open stop intent directory: " + path + ": " + std::strerror(errno));
  if (::fstat(dir, &st) != 0) {
    const int error = errno;
    ::close(dir);
    throw std::runtime_error("cannot inspect stop intent directory: " + path + ": " + std::strerror(error));
  }
  if (!S_ISDIR(st.st_mode) ||
      st.st_uid != ::geteuid() || (st.st_mode & 0777) != 0700) {
    ::close(dir);
    throw std::runtime_error("unsafe stop intent directory ownership or mode: " + path + " " +
        core::session_path_metadata(st) +
        "; require uid=" + std::to_string(::geteuid()) + " mode=0700 (setgid is allowed)");
  }
  return dir;
}

inline uint64_t read_counter(int fd, const std::string& path) {
  struct stat st{};
  if (::fstat(fd, &st) != 0)
    throw std::runtime_error("cannot inspect stop intent journal: " + path + ": " + std::strerror(errno));
  if (!S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 07777) != 0600 || st.st_nlink != 1)
    throw std::runtime_error("unsafe stop intent journal ownership or mode: " + path + " " +
        core::session_path_metadata(st) + "; require single-link regular file uid=" +
        std::to_string(::geteuid()) + " mode=0600");
  if (st.st_size == 0) return 0;
  if (st.st_size < 0 || st.st_size % 17 != 0)
    throw std::runtime_error("partial stop intent journal: " + path);
  char record[17];
  if (::pread(fd, record, sizeof(record), st.st_size - sizeof(record)) != sizeof(record) || record[16] != '\n')
    throw std::runtime_error("unreadable stop intent journal: " + path);
  uint64_t value = 0;
  for (unsigned i = 0; i < 16; ++i) {
    char c = record[i];
    unsigned nibble = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
    if (nibble == 16) throw std::runtime_error("malformed stop intent journal: " + path);
    value = (value << 4) | nibble;
  }
  return value;
}

inline bool valid_hash(const std::string& hash) {
  return hash.size() == 40 && hash.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

struct TemporaryEntry {
  int directory;
  std::string name;
  bool owned = false;
  ~TemporaryEntry() { if (owned && !name.empty()) ::unlinkat(directory, name.c_str(), 0); }
};

inline uint64_t counter(const std::string& session, const std::string& hash, bool increment) {
  if (session.empty() && !increment) return 0;
  if (session.empty() || !valid_hash(hash))
    throw std::runtime_error("invalid session path or hash");
  const int directory = open_directory(session, increment);
  if (directory < 0) return 0;
  Fd dir(directory);
  const std::string path = core::session_path_child(session, ".rutorrent-stop-intents/" + hash);
  const int file = ::openat(dir.get(), hash.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0 && errno != ENOENT)
    throw std::runtime_error("cannot open stop intent journal: " + path + ": " + std::strerror(errno));
  uint64_t value = 0;
  if (file >= 0) {
    Fd journal(file);
    value = read_counter(journal.get(), path);
  }
  if (!increment) return value;
  if (value >= INT64_MAX) throw std::runtime_error("stop intent counter exhausted: " + path);
  char record[18];
  const int length = std::snprintf(record, sizeof(record), "%016llx\n", static_cast<unsigned long long>(++value));
  TemporaryEntry temporary{dir.get(), hash + ".counter." +
      core::random_hex16<std::runtime_error>(nullptr, "cannot generate stop counter nonce") + ".tmp"};
  Fd staging(::openat(dir.get(), temporary.name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
  temporary.owned = true;
  // Replace one complete record. Interrupted or partial writes preserve the
  // previous generation; successful writes compact legacy append-only journals.
  if (length != 17 || ::write(staging.get(), record, 17) != 17 || ::fdatasync(staging.get()) != 0)
    throw std::runtime_error("cannot durably stage stop intent: " + path);
  if (::renameat(dir.get(), temporary.name.c_str(), dir.get(), hash.c_str()) != 0 || ::fsync(dir.get()) != 0)
    throw std::runtime_error("cannot durably publish stop intent: " + path);
  temporary.name.clear();
  return value;
}

inline void erase_counter(const std::string& session, const std::string& hash) {
  if (session.empty()) return;
  if (!valid_hash(hash)) throw std::runtime_error("invalid stop intent hash");
  const int directory = open_directory(session, false);
  if (directory < 0) return;
  Fd dir(directory);
  const std::string path = core::session_path_child(session, ".rutorrent-stop-intents/" + hash);
  const int file = ::openat(dir.get(), hash.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0 && errno == ENOENT) return;
  if (file < 0) throw std::runtime_error("cannot open stop intent journal: " + path + ": " + std::strerror(errno));
  Fd journal(file);
  read_counter(journal.get(), path);
  if (::unlinkat(dir.get(), hash.c_str(), 0) != 0 || ::fsync(dir.get()) != 0)
    throw std::runtime_error("cannot retire stop intent journal: " + path);
}

inline void cleanup_startup(const std::string& session) {
  if (session.empty()) return;
  const int directory = open_directory(session, false);
  if (directory < 0) return;
  Fd dir(directory);
  Fd parent(core::open_trusted_session_directory(session));
  DIR* entries = ::fdopendir(::dup(dir.get()));
  if (!entries) throw std::runtime_error("cannot scan stop intent directory");
  std::vector<std::string> names;
  while (const auto* entry = ::readdir(entries)) names.emplace_back(entry->d_name);
  ::closedir(entries);
  bool changed = false;
  for (const auto& name : names) {
    if (name.size() < 40 || !valid_hash(name.substr(0, 40))) continue;
    struct stat st{};
    if (::fstatat(dir.get(), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 07777) != 0600) continue;
    const std::string suffix = name.substr(40);
    const bool temporary = suffix.size() > 4 && suffix.compare(suffix.size() - 4, 4, ".tmp") == 0 &&
        (suffix.rfind(".claim.", 0) == 0 || suffix.rfind(".counter.", 0) == 0);
    bool orphan = false;
    if (suffix.empty() && st.st_nlink == 1) {
      const std::string hash = name;
      struct stat saved{}, claim{};
      const bool no_session = ::fstatat(parent.get(), (hash + ".torrent").c_str(), &saved, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT;
      const bool no_claim = ::fstatat(dir.get(), (hash + ".claim").c_str(), &claim, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT;
      orphan = no_session && no_claim && !retrackers_migration_source_reserved(hash) &&
          !retrackers_migration_pending(hash) && retrackers_migration_target_source(hash).empty();
    }
    if (((temporary && st.st_nlink == 1) || orphan) && ::unlinkat(dir.get(), name.c_str(), 0) == 0) changed = true;
  }
  if (changed && ::fsync(dir.get()) != 0) throw std::runtime_error("cannot sync retired stop intents");
}

inline std::string token(const std::string& local_id, uint64_t value) {
  char suffix[18];
  std::snprintf(suffix, sizeof(suffix), ":%016llx", static_cast<unsigned long long>(value));
  return local_id + suffix;
}
inline thread_local unsigned suppress_depth = 0;

struct ScopedSuppress {
  explicit ScopedSuppress(bool enabled = true) : enabled_(enabled) { if (enabled_) ++suppress_depth; }
  ~ScopedSuppress() { if (enabled_) --suppress_depth; }
  ScopedSuppress(const ScopedSuppress&) = delete;
  ScopedSuppress& operator=(const ScopedSuppress&) = delete;
private:
  bool enabled_;
};

inline bool suppressed() { return suppress_depth != 0; }

inline void prepare_user_record(core::Download* download);
inline std::string held(core::Download* download);

inline std::string record(core::Download* download, bool require_durable = false) {
  const auto target = rpc::make_target(download);
  const auto hash = rpc::call_command_string("d.hash", target);
  const auto local_id = rpc::call_command_string("d.local_id", target);
  const auto session = rpc::call_command_string("session.path");
  if (session.empty()) {
    if (require_durable) throw torrent::input_error("stop intent: session directory required");
    return token(local_id, 0);
  }
  prepare_user_record(download);
  uint64_t value;
  try { value = counter(session, hash, true); }
  catch (const std::exception& e) {
    // A held claim needs the generation to revoke its owner's later restart.
    if (require_durable || !held(download).empty())
      throw torrent::input_error(std::string("stop intent: ") + e.what());
    const std::string warning = "Stop intent for " + hash + " could not be saved: " + e.what() +
      "; stopped only in memory and may resume after restart";
    lt_log_print(torrent::LOG_ERROR, "%s", warning.c_str());
    control->core()->push_log_std(warning);
    // Container supervisors capture stderr even when the configured file logger is buffered.
    const std::string line = warning + "\n";
    (void)::write(STDERR_FILENO, line.data(), line.size());
    download->set_message("Stop intent could not be saved; stopped in memory and may resume after restart");
    return token(local_id, 0);
  }
  download->bencode()->get_key("rtorrent").insert_key("stop_generation", static_cast<int64_t>(value));
  return token(local_id, value);
}


inline thread_local unsigned restore_path_depth = 0;
struct ScopedRestorePath {
  explicit ScopedRestorePath(bool enabled) : enabled_(enabled) { if (enabled_) ++restore_path_depth; }
  ~ScopedRestorePath() { if (enabled_) --restore_path_depth; }
  ScopedRestorePath(const ScopedRestorePath&) = delete;
  ScopedRestorePath& operator=(const ScopedRestorePath&) = delete;
private:
  bool enabled_;
};
inline bool restoring_path() { return restore_path_depth != 0; }

struct Lease {
  std::string token;
  std::string job;
  std::string local_id;
  int state = 0;
  int open = 0;
  int active = 0;
  char phase = 'A';
  char mode = '-';
  std::string path;
};

inline Lease read_lease(const std::string& session, const std::string& hash) {
  if (session.empty()) return {}; // No lease can exist without a session directory.
  if (hash.size() != 40 || hash.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    throw std::runtime_error("invalid session path or hash");
  const int directory = open_directory(session, false);
  if (directory < 0) return {};
  Fd dir(directory);
  const std::string filename = hash + ".claim";
  const std::string path = core::session_path_child(session, ".rutorrent-stop-intents/" + filename);
  int file = ::openat(dir.get(), filename.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0 && errno == ENOENT) return {};
  if (file < 0) throw std::runtime_error("cannot open stop claim file: " + path + ": " + std::strerror(errno));
  Fd file_handle(file);
  struct stat st{};
  if (::fstat(file_handle.get(), &st) != 0)
    throw std::runtime_error("cannot inspect stop claim file: " + path + ": " + std::strerror(errno));
  if (!S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 07777) != 0600)
    throw std::runtime_error("unsafe stop claim file ownership or mode: " + path + " " +
        core::session_path_metadata(st) + "; require regular file uid=" + std::to_string(::geteuid()) + " mode=0600");
  if (st.st_size != 50 && st.st_size != 128 && (st.st_size < 137 || st.st_size > 4233))
    throw std::runtime_error("invalid stop claim file: " + path);
  std::string value(static_cast<size_t>(st.st_size), '\0');
  if (::pread(file_handle.get(), value.data(), value.size(), 0) != st.st_size)
    throw std::runtime_error("unreadable stop claim file: " + path);
  Lease result;
  const bool terminal = st.st_size != 50 && st.st_size != 128;
  const size_t offset = terminal ? 2 : 0;
  if (terminal && value.substr(0, 2) != "T\n" && value.substr(0, 2) != "D\n")
    throw std::runtime_error("invalid terminal claim phase: " + path);
  if (value[offset + 32] != ':' || value[offset + 49] != '\n')
    throw std::runtime_error("unreadable stop claim file: " + path);
  result.token = value.substr(offset, 49);
  if (result.token.substr(0, 32).find_first_not_of("0123456789abcdef") != std::string::npos ||
      result.token.substr(33).find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::runtime_error("malformed stop claim file: " + path);
  if (st.st_size == 50 && st.st_nlink != 1)
    throw std::runtime_error("unsafe legacy stop claim links: " + path);
  if (st.st_size == 50) return result; // v5 lease: guarded, but no recoverable receipt.
  result.job = value.substr(offset + 50, 32);
  result.local_id = value.substr(offset + 87, 40);
  if (value[offset + 82] != '\n' || value[offset + 86] != '\n' || value[offset + 127] != '\n' ||
      result.job.find_first_not_of("0123456789abcdef") != std::string::npos ||
      result.local_id.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos ||
      value[offset + 83] < '0' || value[offset + 83] > '1' ||
      value[offset + 84] < '0' || value[offset + 84] > '1' ||
      value[offset + 85] < '0' || value[offset + 85] > '1')
    throw std::runtime_error("malformed stop claim receipt: " + path);
  result.state = value[offset + 83] - '0';
  result.open = value[offset + 84] - '0';
  result.active = value[offset + 85] - '0';
  if (terminal) {
    result.phase = value[0];
    result.mode = value[130];
    if ((result.mode != 'S' && result.mode != 'O' && result.mode != 'R' && result.mode != 'K') ||
        value[131] != '\n' || value[136] != ':')
      throw std::runtime_error("malformed terminal claim: " + path);
    unsigned length = 0;
    for (size_t i = 132; i < 136; ++i) {
      const char c = value[i];
      const unsigned digit = c >= '0' && c <= '9' ? c - '0' :
                             c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
      if (digit == 16) throw std::runtime_error("malformed terminal path length: " + path);
      length = (length << 4) | digit;
    }
    if (length > 4096 || value.size() != 137 + length)
      throw std::runtime_error("malformed terminal path: " + path);
    result.path = value.substr(137, length);
  }
  if (st.st_nlink != 1) {
    // Only the former linkat publisher can explain this complete active pair.
    // Other aliases remain a visible refusal; never prune by filename alone.
    const std::string temporary = filename + "." + result.token.substr(0, 32) + ".tmp";
    struct stat staged{}, current{};
    if (st.st_nlink != 2 || st.st_size != 128 || result.phase != 'A' ||
        ::fstatat(dir.get(), temporary.c_str(), &staged, AT_SYMLINK_NOFOLLOW) != 0 ||
        ::fstatat(dir.get(), filename.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(staged.st_mode) || staged.st_uid != st.st_uid ||
        (staged.st_mode & 07777) != 0600 || staged.st_nlink != 2 ||
        staged.st_dev != st.st_dev || staged.st_ino != st.st_ino ||
        current.st_dev != st.st_dev || current.st_ino != st.st_ino)
      throw std::runtime_error("unsafe stop claim links: " + path + " " + core::session_path_metadata(st) +
          "; require single-link receipt or attested interrupted publication pair");
    if (::unlinkat(dir.get(), temporary.c_str(), 0) != 0 || ::fsync(dir.get()) != 0 ||
        ::fstat(file_handle.get(), &current) != 0 || current.st_nlink != 1)
      throw std::runtime_error("cannot recover interrupted stop claim publication: " + path);
  }
  return result;
}

inline std::string lease_token(const std::string& session, const std::string& hash) {
  return read_lease(session, hash).token;
}

inline std::string startup_hold_token(const std::string& session, const std::string& hash) {
  const auto claim = read_lease(session, hash);
  return claim.phase == 'D' ? std::string() : claim.token;
}

inline std::pair<std::string, std::string> identity(core::Download* download) {
  const auto target = rpc::make_target(download);
  return {rpc::call_command_string("session.path"), rpc::call_command_string("d.hash", target)};
}

inline std::string held(core::Download* download) {
  const auto [session, hash] = identity(download);
  try { return lease_token(session, hash); }
  catch (const std::exception& e) { throw torrent::input_error(std::string("stop claim: ") + e.what()); }
}

inline Lease lease(core::Download* download) {
  const auto [session, hash] = identity(download);
  try { return read_lease(session, hash); }
  catch (const std::exception& e) { throw torrent::input_error(std::string("stop claim: ") + e.what()); }
}

inline std::string next_claim_token(core::Download* download, const std::string& local_id) {
  const auto [session, hash] = identity(download);
  try {
    const auto current = counter(session, hash, false);
    if (current >= INT64_MAX) throw std::runtime_error("stop intent counter exhausted");
    return token(local_id, current + 1);
  } catch (const std::exception& e) {
    throw torrent::input_error(std::string("stop claim: ") + e.what());
  }
}

inline void ensure_claim_generation(core::Download* download, const Lease& claim) {
  const auto [session, hash] = identity(download);
  try {
    const auto expected = std::stoull(claim.token.substr(33), nullptr, 16);
    auto current = counter(session, hash, false);
    if (current + 1 == expected) current = counter(session, hash, true);
    if (current < expected || current > INT64_MAX)
      throw std::runtime_error("stop claim generation is inconsistent");
    download->bencode()->get_key("rtorrent").insert_key("stop_generation", static_cast<int64_t>(current));
  } catch (const std::exception& e) {
    throw torrent::input_error(std::string("stop claim: ") + e.what());
  }
}

inline void prepare_user_record(core::Download* download) {
  const auto claim = lease(download);
  if (!claim.token.empty() && claim.phase == 'A') ensure_claim_generation(download, claim);
}

inline bool terminal_activity_allowed(core::Download* download) {
  const auto claim = lease(download);
  if ((claim.phase != 'T' && claim.phase != 'D') || claim.mode != 'S' ||
      download->file_list()->root_dir() != claim.path)
    return false;
  const auto [session, hash] = identity(download);
  try {
    return token(claim.token.substr(0, 32), counter(session, hash, false)) == claim.token &&
           rpc::call_command_value("d.state", rpc::make_target(download)) == 1;
  } catch (const std::exception& e) {
    throw torrent::input_error(std::string("stop claim: ") + e.what());
  }
}

inline std::string receipt(const Lease& claim) {
  return claim.token + "|" + std::to_string(claim.state) + "|" + std::to_string(claim.open) +
         "|" + std::to_string(claim.active) + "|" + claim.local_id;
}

inline thread_local unsigned owner_restore_depth = 0;
struct ScopedOwnerRestore {
  ScopedOwnerRestore() { ++owner_restore_depth; }
  ~ScopedOwnerRestore() { --owner_restore_depth; }
  ScopedOwnerRestore(const ScopedOwnerRestore&) = delete;
  ScopedOwnerRestore& operator=(const ScopedOwnerRestore&) = delete;
};
inline bool owner_restoring() { return owner_restore_depth != 0; }

inline void require_unleased(core::Download* download) {
  if (!owner_restoring() && !held(download).empty()) throw torrent::input_error("stop claim: download is reserved for a file operation");
}

inline bool checker_marker_key(const std::string& key) {
  return key == "retrackers-recovery" || key == "retrackers-recovery-ack" || key == "chk-replacement" || key == "chk-replaces" || key == "chk-replacing" ||
         key == "chk-meta-old" || key == "chk-meta-new" || key == "chk-meta-until" ||
         key == "chk-meta-topic";
}

inline bool checker_service_label(const std::string& encoded) {
  std::string decoded;
  decoded.reserve(encoded.size());
  for (size_t i = 0; i < encoded.size(); ++i) {
    if (encoded[i] == '%' && i + 2 < encoded.size()) {
      auto nibble = [](char c) -> int {
        return c >= '0' && c <= '9' ? c - '0' :
               c >= 'a' && c <= 'f' ? c - 'a' + 10 :
               c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      };
      const int high = nibble(encoded[i + 1]);
      const int low = nibble(encoded[i + 2]);
      if (high >= 0 && low >= 0) {
        decoded.push_back(static_cast<char>((high << 4) | low));
        i += 2;
        continue;
      }
    }
    decoded.push_back(encoded[i]);
  }
  return decoded == ".chk-meta";
}

inline bool checker_owned(core::Download* download) {
  const auto& rtorrent = download->bencode()->get_key("rtorrent");
  if (checker_service_label(rtorrent.get_key_string("custom1"))) return true;
  if (!rtorrent.has_key("custom")) return false;
  for (const auto& marker : rtorrent.get_key("custom").as_map())
    if (checker_marker_key(marker.first) && !marker.second.as_string().empty()) return true;
  return false;
}

inline thread_local unsigned owner_open_depth = 0;
struct ScopedOwnerOpen {
  ScopedOwnerOpen() { ++owner_open_depth; }
  ~ScopedOwnerOpen() { --owner_open_depth; }
  ScopedOwnerOpen(const ScopedOwnerOpen&) = delete;
  ScopedOwnerOpen& operator=(const ScopedOwnerOpen&) = delete;
};
inline bool owner_open() { return owner_open_depth != 0 || owner_restoring(); }

inline Lease finish_claim(core::Download* download, const std::string& expected, bool& current) {
  Lease claim = lease(download);
  if (claim.phase != 'A' || claim.token != expected || expected.empty()) return {};
  const auto target = rpc::make_target(download);
  if (rpc::call_command_value("d.state", target) != 0 || download->is_open() || download->is_active())
    throw torrent::input_error("stop claim: download is no longer quiescent");
  if (checker_owned(download))
    throw torrent::input_error("stop claim: checker ownership appeared during file operation");
  const auto [session, hash] = identity(download);
  uint64_t generation;
  try { generation = counter(session, hash, false); }
  catch (const std::exception& e) { throw torrent::input_error(std::string("stop claim: ") + e.what()); }
  current = token(expected.substr(0, 32), generation) == expected;
  return claim;
}

inline void publish_claim_file(int directory, const std::string& staging, const std::string& name) {
  if (core::migration_test_cut("before-stop-claim-publication")) ::_exit(137);
  if (core::rename_session_entry_noreplace(directory, staging, name) != 0)
    throw std::runtime_error("cannot exclusively publish stop claim receipt: " + std::string(std::strerror(errno)));
  if (::fsync(directory) != 0)
    throw std::runtime_error("cannot durably publish stop claim receipt");
  if (core::migration_test_cut("after-stop-claim-publication")) ::_exit(137);
}

inline Lease create_lease(core::Download* download, const std::string& stop_token,
                          const std::string& job, int state, int open, int active,
                          const std::string& local_id) {
  const auto [session, hash] = identity(download);
  if (retrackers_migration_original_claim_active(hash))
    throw torrent::input_error("stop claim: RR original claim is active");
  if (!held(download).empty()) throw torrent::input_error("stop claim: already reserved");
  if (job.size() != 32 || job.find_first_not_of("0123456789abcdef") != std::string::npos ||
      local_id.size() != 40 || local_id.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    throw torrent::input_error("stop claim: invalid job or local identity");
  try {
    std::string token = core::random_hex16<std::runtime_error>(
        nullptr, "cannot generate stop claim nonce");
    token += ":" + stop_token.substr(stop_token.size() - 16);
    Lease claim{token, job, local_id, state, open, active};
    const std::string payload = token + "\n" + job + "\n" +
      std::to_string(state) + std::to_string(open) + std::to_string(active) + "\n" + local_id + "\n";
    if (payload.size() != 128) throw std::runtime_error("invalid stop claim receipt");
    Fd dir(open_directory(session, true));
    const std::string filename = hash + ".claim";
    const std::string temporary = filename + "." + token.substr(0, 32) + ".tmp";
    TemporaryEntry cleanup{dir.get(), temporary};
    Fd file(::openat(dir.get(), temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    cleanup.owned = true;
    if (::write(file.get(), payload.data(), payload.size()) != static_cast<ssize_t>(payload.size()) ||
        ::fdatasync(file.get()) != 0)
      throw std::runtime_error("cannot durably write stop claim receipt");
    publish_claim_file(dir.get(), temporary, filename);
    cleanup.name.clear();
    return claim;
  } catch (const std::exception& e) { throw torrent::input_error(std::string("stop claim: ") + e.what()); }
}

inline Lease publish_phase(core::Download* download, const Lease& claim, char phase,
                           char mode, const std::string& path) {
  if (!((phase == 'T' && claim.phase == 'A') || (phase == 'D' && claim.phase == 'T')) ||
      held(download) != claim.token || path.size() > 4096 ||
      (mode != 'S' && mode != 'O' && mode != 'R' && mode != 'K'))
    throw torrent::input_error("stop claim: invalid terminal transition");
  const auto [session, hash] = identity(download);
  try {
    char size[5];
    std::snprintf(size, sizeof(size), "%04x", static_cast<unsigned>(path.size()));
    const std::string body = claim.token + "\n" + claim.job + "\n" +
      std::to_string(claim.state) + std::to_string(claim.open) + std::to_string(claim.active) +
      "\n" + claim.local_id + "\n";
    const std::string payload = std::string(1, phase) + "\n" + body + mode + "\n" + size + ":" + path;
    Fd dir(open_directory(session, false));
    const std::string filename = hash + ".claim";
    const std::string temporary = filename + "." + claim.token.substr(0, 32) + "." + phase + ".tmp";
    TemporaryEntry cleanup{dir.get(), temporary};
    Fd file(::openat(dir.get(), temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    cleanup.owned = true;
    if (::write(file.get(), payload.data(), payload.size()) != static_cast<ssize_t>(payload.size()) ||
        ::fdatasync(file.get()) != 0 ||
        ::renameat(dir.get(), temporary.c_str(), dir.get(), filename.c_str()) != 0 ||
        ::fsync(dir.get()) != 0)
      throw std::runtime_error("cannot durably publish terminal claim");
    cleanup.name.clear();
    Lease terminal = claim;
    terminal.phase = phase;
    terminal.mode = mode;
    terminal.path = path;
    return terminal;
  } catch (const std::exception& e) { throw torrent::input_error(std::string("stop claim: ") + e.what()); }
}

inline Lease publish_terminal(core::Download* download, const Lease& claim, char mode, const std::string& path) {
  return publish_phase(download, claim, 'T', mode, path);
}
inline Lease publish_done(core::Download* download, const Lease& claim) {
  return publish_phase(download, claim, 'D', claim.mode, claim.path);
}

inline bool release_lease(core::Download* download, const std::string& token) {
  if (held(download) != token) return false;
  const auto [session, hash] = identity(download);
  try {
    Fd dir(open_directory(session, false));
    if (::unlinkat(dir.get(), (hash + ".claim").c_str(), 0) != 0 || ::fsync(dir.get()) != 0)
      throw std::runtime_error("cannot durably release stop claim");
  } catch (const std::exception& e) { throw torrent::input_error(std::string("stop claim: ") + e.what()); }
  return true;
}

} // namespace stop_intent
