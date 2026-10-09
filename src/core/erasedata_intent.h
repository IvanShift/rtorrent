#pragma once

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <string>
#include <utility>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "stop_intent.h"

namespace core::erasedata_intent {

struct Record {
  std::string hash, incarnation, generation, user, nonce, digest;
  char phase = 'P';
};

inline constexpr size_t max_record_size = 323;

inline bool hex(const std::string& value, size_t length, bool upper = false) {
  return value.size() == length && value.find_first_not_of(
      upper ? "0123456789ABCDEF" : "0123456789abcdef") == std::string::npos;
}

inline const char* phase_name(char phase) {
  switch (phase) {
  case 'P': return "prepared";
  case 'E': return "erase-started";
  case 'C': return "committed";
  default: return "invalid";
  }
}

inline bool valid(const Record& record) {
  return hex(record.hash, 40, true) && hex(record.incarnation, 32) &&
      hex(record.generation, 16) && record.generation != std::string(16, '0') &&
      record.user.size() <= 128 &&
      record.user.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos &&
      hex(record.nonce, 32) && hex(record.digest, 64) &&
      (record.phase == 'P' || record.phase == 'E' || record.phase == 'C');
}

// The application nonce and digest identify one deletion obligation. Its phase
// advances independently, so an attested retry can recognize the same tuple.
inline bool same(const Record& left, const Record& right) {
  return left.hash == right.hash && left.incarnation == right.incarnation &&
      left.generation == right.generation && left.user == right.user &&
      left.nonce == right.nonce && left.digest == right.digest;
}

inline std::string format(const Record& record) {
  if (!valid(record)) throw torrent::input_error("erasedata: invalid-record");
  return "e1:" + std::string(1, record.phase) + ":" + record.hash + ":" +
      record.incarnation + ":" + record.generation + ":" + record.user + ":" +
      record.nonce + ":" + record.digest + "\n";
}

inline bool parse(const std::string& bytes, Record* output) {
  if (bytes.empty() || bytes.size() > max_record_size || bytes.back() != '\n') return false;
  std::array<std::string, 8> fields;
  size_t begin = 0;
  for (size_t i = 0; i < fields.size(); ++i) {
    const auto end = bytes.find(':', begin);
    if ((i + 1 < fields.size()) == (end == std::string::npos)) return false;
    fields[i] = bytes.substr(begin, i + 1 == fields.size() ? bytes.size() - begin - 1 : end - begin);
    begin = end + 1;
  }
  if (fields[0] != "e1" || fields[1].size() != 1) return false;
  Record record{fields[2], fields[3], fields[4], fields[5], fields[6], fields[7], fields[1][0]};
  if (!valid(record)) return false;
  if (output) *output = std::move(record);
  return true;
}

inline std::string path(const std::string& session, const std::string& hash, const std::string& suffix) {
  return session_path_child(session, ".rutorrent-stop-intents/" + hash + suffix);
}

[[noreturn]] inline void fail(const std::string& reason, const std::string& document) {
  throw torrent::input_error("erasedata: " + reason + "; inspect " + document);
}

inline int directory(const std::string& session, bool create) {
  try { return stop_intent::open_directory(session, create); }
  catch (const std::exception& error) { throw torrent::input_error(std::string("erasedata: ") + error.what()); }
}

inline void check_hash(const std::string& hash) {
  if (!hex(hash, 40, true)) throw torrent::input_error("erasedata: invalid-hash");
}

inline void sync_directory(int fd, const std::string& document) {
  if (::fsync(fd) != 0) fail("directory-sync-failed", document);
}

inline struct stat attest_file(int fd, const std::string& document, bool stable_lock) {
  struct stat state{};
  if (::fstat(fd, &state) != 0) fail("unreadable-document", document);
  if (!S_ISREG(state.st_mode) || state.st_uid != ::geteuid() ||
      (state.st_mode & 07777) != 0600 || state.st_nlink != 1 ||
      (stable_lock ? state.st_size != 0 : state.st_size <= 0 || state.st_size > static_cast<off_t>(max_record_size)))
    fail("unsafe-document (require single-link regular file uid=" + std::to_string(::geteuid()) +
         " mode=0600" + (stable_lock ? " size=0)" : " bounded record)"), document);
  return state;
}

// This inode is permanent. An inherited worker descriptor must continue fencing
// load admission even after completion removes the corresponding record.
// Close leases only: LOCK_UN on one duplicate would release every descendant.
inline int lock(const std::string& session, const std::string& hash, bool create) {
  check_hash(hash);
  if (session.empty() && !create) return -1;
  if (session.empty()) throw torrent::input_error("erasedata: session-directory-required");
  const int opened_directory = directory(session, create);
  if (opened_directory < 0) return -1;
  SessionPathFd dir(opened_directory);
  const auto name = hash + ".erasedata.lock";
  const auto document = path(session, hash, ".erasedata.lock");
  bool created = false;
  int opened = ::openat(dir.get(), name.c_str(), O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (opened < 0 && errno == ENOENT) {
    struct stat journal{};
    if (::fstatat(dir.get(), (hash + ".erasedata").c_str(), &journal, AT_SYMLINK_NOFOLLOW) == 0)
      fail("record-without-stable-lock", path(session, hash, ".erasedata") + " and " + document);
    if (errno != ENOENT) fail("unreadable-document", path(session, hash, ".erasedata"));
    if (!create) return -1;
    opened = ::openat(dir.get(), name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (opened >= 0) created = true;
    else if (errno == EEXIST)
      opened = ::openat(dir.get(), name.c_str(), O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  }
  if (opened < 0) fail("unreadable-stable-lock", document);
  SessionPathFd file(opened);
  if (created && ::fchmod(file.get(), 0600) != 0) fail("stable-lock-create-failed", document);
  attest_file(file.get(), document, true);
  // Sync creation even when another process obtained the new lock first.
  if (created && (::fsync(file.get()) != 0 || ::fsync(dir.get()) != 0))
    fail("stable-lock-sync-failed", document);
  if (::flock(file.get(), LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK || errno == EAGAIN)
      throw torrent::input_error("erasedata: collector-busy");
    fail("stable-lock-failed", document);
  }
  return file.release();
}

inline bool read_at(int dir, const std::string& session, const std::string& hash, Record* output) {
  const auto document = path(session, hash, ".erasedata");
  const int opened = ::openat(dir, (hash + ".erasedata").c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (opened < 0 && errno == ENOENT) return false;
  if (opened < 0) fail("unreadable-record", document);
  SessionPathFd file(opened);
  const auto state = attest_file(file.get(), document, false);
  std::string bytes(static_cast<size_t>(state.st_size), '\0');
  size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = ::read(file.get(), bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) fail("unreadable-record", document);
    offset += static_cast<size_t>(count);
  }
  char extra;
  ssize_t count;
  do { count = ::read(file.get(), &extra, 1); } while (count < 0 && errno == EINTR);
  Record record;
  if (count != 0 || !parse(bytes, &record) || record.hash != hash) fail("malformed-record", document);
  if (output) *output = std::move(record);
  return true;
}

inline bool read(const std::string& session, const std::string& hash, Record* output) {
  check_hash(hash);
  if (session.empty()) return false;
  const int opened = directory(session, false);
  if (opened < 0) return false;
  SessionPathFd dir(opened);
  return read_at(dir.get(), session, hash, output);
}

// The caller holds the stable lock across all record and destructive operations.
inline void write(const std::string& session, const std::string& hash, const Record& record, bool create = false) {
  check_hash(hash);
  const auto document = path(session, hash, ".erasedata");
  if (!valid(record) || record.hash != hash) fail("invalid-record", document);
  SessionPathFd dir(directory(session, true));
  if (!create) {
    Record current;
    if (!read_at(dir.get(), session, hash, &current)) fail("record-missing", document);
    if (!same(current, record)) fail("record-identity-changed", document);
  }
  const auto nonce_error = "erasedata: nonce-unavailable; inspect " + document;
  stop_intent::TemporaryEntry temporary{dir.get(), hash + ".erasedata." +
      random_hex16<torrent::input_error>(nonce_error.c_str(), nonce_error.c_str()) + ".tmp"};
  const int opened = ::openat(dir.get(), temporary.name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
  if (opened < 0) fail("record-stage-failed", document);
  SessionPathFd file(opened);
  temporary.owned = true;
  if (::fchmod(file.get(), 0600) != 0) fail("record-stage-failed", document);
  attest_file(file.get(), session_path_child(session, ".rutorrent-stop-intents/" + temporary.name), true);
  const auto bytes = format(record);
  size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = ::write(file.get(), bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) fail("record-stage-failed", document);
    offset += static_cast<size_t>(count);
  }
  if (::fdatasync(file.get()) != 0) fail("record-sync-failed", document);
  const int result = create ? rename_session_entry_noreplace(dir.get(), temporary.name, hash + ".erasedata") :
      ::renameat(dir.get(), temporary.name.c_str(), dir.get(), (hash + ".erasedata").c_str());
  if (result != 0) fail("record-publication-failed", document);
  temporary.name.clear();
  sync_directory(dir.get(), document);
}

inline void remove(const std::string& session, const std::string& hash, const Record& expected) {
  check_hash(hash);
  const auto document = path(session, hash, ".erasedata");
  if (!valid(expected) || expected.hash != hash) fail("invalid-record", document);
  const int opened = directory(session, false);
  if (opened < 0) return;
  SessionPathFd dir(opened);
  Record current;
  if (!read_at(dir.get(), session, hash, &current)) return;
  if (!same(current, expected)) fail("record-identity-changed", document);
  if (::unlinkat(dir.get(), (hash + ".erasedata").c_str(), 0) != 0) fail("record-remove-failed", document);
  sync_directory(dir.get(), document);
}

inline bool session_absent(const std::string& session, const std::string& hash) {
  check_hash(hash);
  int opened;
  try { opened = open_trusted_session_directory(session); }
  catch (const std::exception& error) { throw torrent::input_error(std::string("erasedata: ") + error.what()); }
  SessionPathFd dir(opened);
  for (const char* suffix : {"", ".rtorrent", ".libtorrent_resume", ".meta"}) {
    for (const char* staging : {"", ".new"}) {
      const auto name = hash + ".torrent" + suffix + staging;
      const auto document = session_path_child(session, name);
      struct stat state{};
      if (::fstatat(dir.get(), name.c_str(), &state, AT_SYMLINK_NOFOLLOW) == 0)
        fail("session-document-present", document);
      if (errno != ENOENT) fail("session-document-unreadable", document);
    }
  }
  sync_directory(dir.get(), session_path_absolute(session));
  return true;
}

} // namespace core::erasedata_intent
