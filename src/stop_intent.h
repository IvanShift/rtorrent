#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
#include "globals.h"
#include "core/download.h"
#include "core/retrackers_migration_pending.h"
#include "rpc/parse_commands.h"

namespace stop_intent {
class Fd {
public:
  explicit Fd(int fd) : fd_(fd) {
    if (fd_ < 0) throw std::runtime_error(std::strerror(errno));
  }
  ~Fd() { ::close(fd_); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const { return fd_; }
private:
  int fd_;
};

inline uint64_t read_counter(int fd) {
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    throw std::runtime_error("invalid stop intent journal");
  if (st.st_size == 0) return 0;
  if (st.st_size < 0 || st.st_size % 17 != 0)
    throw std::runtime_error("partial stop intent journal");
  char record[17];
  if (::pread(fd, record, sizeof(record), st.st_size - sizeof(record)) != sizeof(record) || record[16] != '\n')
    throw std::runtime_error("unreadable stop intent journal");
  uint64_t value = 0;
  for (unsigned i = 0; i < 16; ++i) {
    char c = record[i];
    unsigned nibble = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
    if (nibble == 16) throw std::runtime_error("malformed stop intent journal");
    value = (value << 4) | nibble;
  }
  return value;
}

inline uint64_t counter(const std::string& session, const std::string& hash, bool increment) {
  if (session.empty() && !increment) return 0; // No session means there is no durable intent journal.
  if (session.empty() || hash.size() != 40 || hash.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    throw std::runtime_error("invalid session path or hash");
  Fd parent(::open(session.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  constexpr const char* dirname = ".rutorrent-stop-intents";
  if (increment && ::mkdirat(parent.get(), dirname, 0700) == 0) {
    if (::fsync(parent.get()) != 0) throw std::runtime_error("cannot sync intent directory entry");
  } else if (increment && errno != EEXIST) {
    throw std::runtime_error("cannot create intent directory");
  }
  int directory = ::openat(parent.get(), dirname, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (directory < 0 && !increment && errno == ENOENT) return 0;
  Fd dir(directory);
  bool created = false;
  int file = -1;
  if (increment) {
    file = ::openat(dir.get(), hash.c_str(), O_RDWR | O_APPEND | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file >= 0) created = true;
    else if (errno == EEXIST) file = ::openat(dir.get(), hash.c_str(), O_RDWR | O_APPEND | O_NOFOLLOW | O_CLOEXEC);
  } else {
    file = ::openat(dir.get(), hash.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (file < 0 && errno == ENOENT) return 0;
  }
  Fd journal(file);
  uint64_t value = read_counter(journal.get());
  if (!increment) return value;
  if (value >= INT64_MAX) throw std::runtime_error("stop intent counter exhausted");
  char record[18];
  int length = std::snprintf(record, sizeof(record), "%016llx\n", static_cast<unsigned long long>(++value));
  if (length != 17 || ::write(journal.get(), record, 17) != 17 || ::fdatasync(journal.get()) != 0)
    throw std::runtime_error("cannot durably write stop intent");
  if (created && ::fsync(dir.get()) != 0) throw std::runtime_error("cannot sync intent file entry");
  return value;
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
  Fd parent(::open(session.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  int directory = ::openat(parent.get(), ".rutorrent-stop-intents", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (directory < 0 && errno == ENOENT) return {};
  Fd dir(directory);
  const std::string filename = hash + ".claim";
  int file = ::openat(dir.get(), filename.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0 && errno == ENOENT) return {};
  Fd file_handle(file);
  struct stat st{};
  if (::fstat(file_handle.get(), &st) != 0 || !S_ISREG(st.st_mode) || (st.st_size != 50 && st.st_size != 128 && (st.st_size < 137 || st.st_size > 4233)))
    throw std::runtime_error("invalid stop claim file");
  std::string value(static_cast<size_t>(st.st_size), '\0');
  if (::pread(file_handle.get(), value.data(), value.size(), 0) != st.st_size)
    throw std::runtime_error("unreadable stop claim file");
  Lease result;
  const bool terminal = st.st_size != 50 && st.st_size != 128;
  const size_t offset = terminal ? 2 : 0;
  if (terminal && value.substr(0, 2) != "T\n" && value.substr(0, 2) != "D\n")
    throw std::runtime_error("invalid terminal claim phase");
  if (value[offset + 32] != ':' || value[offset + 49] != '\n')
    throw std::runtime_error("unreadable stop claim file");
  result.token = value.substr(offset, 49);
  if (result.token.substr(0, 32).find_first_not_of("0123456789abcdef") != std::string::npos ||
      result.token.substr(33).find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::runtime_error("malformed stop claim file");
  if (st.st_size == 50) return result; // v5 lease: guarded, but no recoverable receipt.
  result.job = value.substr(offset + 50, 32);
  result.local_id = value.substr(offset + 87, 40);
  if (value[offset + 82] != '\n' || value[offset + 86] != '\n' || value[offset + 127] != '\n' ||
      result.job.find_first_not_of("0123456789abcdef") != std::string::npos ||
      result.local_id.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos ||
      value[offset + 83] < '0' || value[offset + 83] > '1' ||
      value[offset + 84] < '0' || value[offset + 84] > '1' ||
      value[offset + 85] < '0' || value[offset + 85] > '1')
    throw std::runtime_error("malformed stop claim receipt");
  result.state = value[offset + 83] - '0';
  result.open = value[offset + 84] - '0';
  result.active = value[offset + 85] - '0';
  if (terminal) {
    result.phase = value[0];
    result.mode = value[130];
    if ((result.mode != 'S' && result.mode != 'O' && result.mode != 'R' && result.mode != 'K') ||
        value[131] != '\n' || value[136] != ':')
      throw std::runtime_error("malformed terminal claim");
    unsigned length = 0;
    for (size_t i = 132; i < 136; ++i) {
      const char c = value[i];
      const unsigned digit = c >= '0' && c <= '9' ? c - '0' :
                             c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
      if (digit == 16) throw std::runtime_error("malformed terminal path length");
      length = (length << 4) | digit;
    }
    if (length > 4096 || value.size() != 137 + length)
      throw std::runtime_error("malformed terminal path");
    result.path = value.substr(137, length);
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
    unsigned char random[16];
    Fd source(::open("/dev/urandom", O_RDONLY | O_CLOEXEC));
    size_t offset = 0;
    while (offset < sizeof(random)) {
      const ssize_t received = ::read(source.get(), random + offset, sizeof(random) - offset);
      if (received < 0 && errno == EINTR) continue;
      if (received <= 0) throw std::runtime_error("cannot generate stop claim nonce");
      offset += static_cast<size_t>(received);
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string token(32, '0');
    for (size_t i = 0; i < sizeof(random); ++i) {
      token[2 * i] = hex[random[i] >> 4];
      token[2 * i + 1] = hex[random[i] & 15];
    }
    token += ":" + stop_token.substr(stop_token.size() - 16);
    Lease claim{token, job, local_id, state, open, active};
    const std::string payload = token + "\n" + job + "\n" +
      std::to_string(state) + std::to_string(open) + std::to_string(active) + "\n" + local_id + "\n";
    if (payload.size() != 128) throw std::runtime_error("invalid stop claim receipt");
    Fd parent(::open(session.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (::mkdirat(parent.get(), ".rutorrent-stop-intents", 0700) == 0) {
      if (::fsync(parent.get()) != 0) throw std::runtime_error("cannot sync stop claim directory entry");
    } else if (errno != EEXIST) {
      throw std::runtime_error("cannot create stop claim directory");
    }
    Fd dir(::openat(parent.get(), ".rutorrent-stop-intents", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    const std::string filename = hash + ".claim";
    const std::string temporary = filename + "." + token.substr(0, 32) + ".tmp";
    ::unlinkat(dir.get(), temporary.c_str(), 0); // stale temp after SIGKILL.
    Fd file(::openat(dir.get(), temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (::write(file.get(), payload.data(), payload.size()) != static_cast<ssize_t>(payload.size()) ||
        ::fdatasync(file.get()) != 0)
      throw std::runtime_error("cannot durably write stop claim receipt");
    // Only publish a fully written receipt. A crash before link leaves no claim to recover.
    if (::linkat(dir.get(), temporary.c_str(), dir.get(), filename.c_str(), 0) != 0 ||
        ::fsync(dir.get()) != 0)
      throw std::runtime_error("cannot durably publish stop claim receipt");
    if (::unlinkat(dir.get(), temporary.c_str(), 0) != 0 || ::fsync(dir.get()) != 0)
      throw std::runtime_error("cannot clean stop claim temporary file");
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
    Fd parent(::open(session.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    Fd dir(::openat(parent.get(), ".rutorrent-stop-intents", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    const std::string filename = hash + ".claim";
    const std::string temporary = filename + "." + claim.token.substr(0, 32) + "." + phase + ".tmp";
    ::unlinkat(dir.get(), temporary.c_str(), 0); // stale temp after SIGKILL, never published.
    Fd file(::openat(dir.get(), temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (::write(file.get(), payload.data(), payload.size()) != static_cast<ssize_t>(payload.size()) ||
        ::fdatasync(file.get()) != 0 ||
        ::renameat(dir.get(), temporary.c_str(), dir.get(), filename.c_str()) != 0 ||
        ::fsync(dir.get()) != 0)
      throw std::runtime_error("cannot durably publish terminal claim");
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
    Fd parent(::open(session.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    Fd dir(::openat(parent.get(), ".rutorrent-stop-intents", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (::unlinkat(dir.get(), (hash + ".claim").c_str(), 0) != 0 || ::fsync(dir.get()) != 0)
      throw std::runtime_error("cannot durably release stop claim");
  } catch (const std::exception& e) { throw torrent::input_error(std::string("stop claim: ") + e.what()); }
  return true;
}

} // namespace stop_intent
