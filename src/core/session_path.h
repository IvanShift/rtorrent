#pragma once

#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <map>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <torrent/utils/log.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

#include "core/migration_test_cut.h"

namespace core {

#ifdef __linux__
// Test builds inject the flag-rejection errors that a 9p mount returns.
inline int injected_rename_noreplace_error() {
  static constexpr struct { const char* cut; int error; } cuts[] = {
    {"rename-noreplace-einval", EINVAL}, {"rename-noreplace-enosys", ENOSYS},
    {"rename-noreplace-eopnotsupp", EOPNOTSUPP}, {"rename-noreplace-eacces", EACCES}};
  for (const auto& cut : cuts)
    if (migration_test_cut(cut.cut)) return cut.error;
  return 0;
}

// The first fallback of a process is reported once, by errno class only: the
// session filesystem then lacks atomic no-replace rename. Containers capture
// stderr even when the rc routes only errors to a log file.
inline void report_rename_noreplace_fallback(int error) {
  static std::atomic<bool> reported{false};
  if (reported.exchange(true)) return;
  const char* reason = error == EINVAL ? "EINVAL" : error == ENOSYS ? "ENOSYS" : "EOPNOTSUPP";
  const std::string line = std::string("rtorrent: session publication uses the checked rename "
      "fallback: renameat2 RENAME_NOREPLACE unsupported (") + reason + "); reported once per process";
  lt_log_print(torrent::LOG_ERRORS, "%s", line.c_str());
  const std::string output = line + "\n";
  (void)::write(STDERR_FILENO, output.data(), output.size());
}
#endif

// Portable no-replace publication by hard link, the primitive outside Linux.
// The link is the publication; removing the staging name is cleanup. A failed
// removal leaves a two-link pair: stop claim recovery retires the claim's exact
// nonce-named staging entry, and startup reports a capsule temporary, so it is
// not reported as a failed publication. Test builds can skip the removal.
inline int link_session_entry_noreplace(int directory, const std::string& source,
                                         const std::string& destination) {
  if (::linkat(directory, source.c_str(), directory, destination.c_str(), 0) != 0) return -1;
  if (!migration_test_cut("link-publication-keep-staging"))
    (void)::unlinkat(directory, source.c_str(), 0);
  return 0;
}

// Publishes a fully written private session entry without replacing an
// existing name. The main-thread caller owns this attested directory: the
// daemon is its only writer, so no lock is taken and only trusted same-UID
// processes can race the absence check on the fallback path. File and
// directory durability stay at the publication call site.
//
// Twin rule: ruTorrent plugins/erasedata/rename-noreplace.c rename_no_replace()
// applies the same EINVAL/ENOSYS/EOPNOTSUPP fallback for payload renames, and
// its --probe status for that fallback is reported by docker-rutorrent
// rootfs/usr/local/bin/startup. Change the three together.
inline int rename_session_entry_noreplace(int directory, const std::string& source,
                                           const std::string& destination) {
#ifdef __linux__
  int result = -1;
  if (const int injected = injected_rename_noreplace_error())
    errno = injected;
  else
    result = ::syscall(SYS_renameat2, directory, source.c_str(), directory,
                       destination.c_str(), 1 /* RENAME_NOREPLACE */);
  if (result == 0 || (errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP))
    return result;
  // WSL drvfs over 9p rejects rename flags even on a current Linux kernel.
  report_rename_noreplace_fallback(errno);
  // A dangling symlink is also an existing target and must never be replaced.
  struct stat target{};
  if (::fstatat(directory, destination.c_str(), &target, AT_SYMLINK_NOFOLLOW) == 0) {
    errno = EEXIST;
    return -1;
  }
  if (errno != ENOENT) return -1;
  return ::renameat(directory, source.c_str(), directory, destination.c_str());
#else
  return link_session_entry_noreplace(directory, source, destination);
#endif
}

inline std::string session_path_absolute(const std::string& session) {
  if (session.empty()) throw std::runtime_error("session directory required");
  return session[0] == '/' ? session : std::filesystem::current_path().string() + "/" + session;
}

inline std::string session_path_child(const std::string& session, const std::string& name) {
  return (std::filesystem::path(session_path_absolute(session)) / name).string();
}

inline std::string session_path_metadata(const struct stat& st) {
  char mode[8];
  std::snprintf(mode, sizeof(mode), "0%03o", static_cast<unsigned>(st.st_mode & 07777));
  return "uid=" + std::to_string(st.st_uid) + " mode=" + mode +
      " nlink=" + std::to_string(st.st_nlink);
}

// The admission of a private sidecar directory (.rutorrent-stop-intents,
// .retrackers-migration) shared by its gateways.
inline bool private_sidecar_directory(const struct stat& st) {
  return S_ISDIR(st.st_mode) && st.st_uid == ::geteuid() && (st.st_mode & 0777) == 0700;
}

class SessionPathFd {
public:
  explicit SessionPathFd(int fd) : fd_(fd) {
    if (fd_ < 0) throw std::runtime_error(std::strerror(errno));
  }
  ~SessionPathFd() { if (fd_ >= 0) ::close(fd_); }
  SessionPathFd(const SessionPathFd&) = delete;
  SessionPathFd& operator=(const SessionPathFd&) = delete;
  int get() const { return fd_; }
  void reset(int fd) { ::close(fd_); fd_ = fd; }
  int release() { const int fd = fd_; fd_ = -1; return fd; }
private:
  int fd_;
};

// Open a private session through trusted directories, without following a symlink component.
// A writable non-sticky ancestor lets another UID replace the whole private session.
// `identity` receives the attested session directory's stat.
inline int walk_trusted_session_directory(const std::string& session, struct stat* identity = nullptr) {
  if (session.empty()) throw std::runtime_error("session directory required");
  const std::string path = session_path_absolute(session);
  const int root = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (root < 0) throw std::runtime_error(std::string("cannot open session path root /: ") + std::strerror(errno));
  SessionPathFd parent(root);
  std::string parent_path = "/";
  const std::string trusted_owner = "require directory uid=0 or uid=" + std::to_string(::geteuid());
  for (size_t begin = 1; begin <= path.size();) {
    const size_t end = path.find('/', begin);
    const std::string component = path.substr(begin, end - begin);
    if (!component.empty() && component != ".") {
      struct stat ancestor{}, child{};
      if (::fstat(parent.get(), &ancestor) != 0)
        throw std::runtime_error("cannot inspect session path ancestor: " + parent_path + ": " + std::strerror(errno));
      if ((ancestor.st_uid != 0 && ancestor.st_uid != ::geteuid()) ||
          ((ancestor.st_mode & (S_IWGRP | S_IWOTH)) != 0 && (ancestor.st_mode & S_ISVTX) == 0))
        throw std::runtime_error("unsafe session path ancestor ownership or mode: " + parent_path + " " +
            session_path_metadata(ancestor) + "; " + trusted_owner + ", no group/other write unless sticky");
      const std::string child_path = path.substr(0, end);
      const int next = ::openat(parent.get(), component.c_str(),
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (next < 0) throw std::runtime_error("cannot open session path component: " + child_path + ": " + std::strerror(errno));
      if (::fstat(next, &child) != 0) {
        const int error = errno;
        ::close(next);
        throw std::runtime_error("cannot inspect session path component: " + child_path + ": " + std::strerror(error));
      }
      if (!S_ISDIR(child.st_mode) || (child.st_uid != 0 && child.st_uid != ::geteuid())) {
        ::close(next);
        throw std::runtime_error("unsafe session path component ownership or mode: " + child_path + " " +
            session_path_metadata(child) + "; " + trusted_owner);
      }
      parent.reset(next);
      parent_path = child_path;
    }
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  struct stat st{};
  if (::fstat(parent.get(), &st) != 0)
    throw std::runtime_error("cannot inspect session directory: " + path + ": " + std::strerror(errno));
  if (!S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH)) != 0)
    throw std::runtime_error("unsafe session directory ownership or mode: " + path + " " +
        session_path_metadata(st) + "; require directory uid=" + std::to_string(::geteuid()) +
        ", no group/other write (setgid is allowed)");
  if (identity) *identity = st;
  return parent.release();
}

// A private sidecar directory opened for reading names: on the thread of the
// pass it is the pass's descriptor, which end(), forget() and a check() that
// ends the pass close, so a reader uses it only within the call that got it;
// elsewhere it is owned and closed here. Readers only open, stat or unlink
// names through it; reading its entries would move an offset shared with
// other readers.
class SidecarDirectory {
public:
  SidecarDirectory(int fd, bool owned) : m_fd(fd), m_owned(owned) {}
  ~SidecarDirectory() { if (m_owned && m_fd >= 0) ::close(m_fd); }
  SidecarDirectory(const SidecarDirectory&) = delete;
  SidecarDirectory& operator=(const SidecarDirectory&) = delete;
  int get() const { return m_fd; }  // -1: the directory does not exist.
private:
  int  m_fd;
  bool m_owned;
};

// The verified session directory of one pass of the main thread. Session load
// runs one task per torrent in a single scheduler pass, and every sidecar probe
// used to walk the ancestors again. The owner walks once per pass, hands out
// private reopened copies of the pinned descriptor and keeps, for each private
// sidecar directory a reader asked for, its attested descriptor or the fact
// that it does not exist. Every probe of a name stays a live openat/fstatat
// relative to these descriptors, so a name that the daemon or another process
// of its UID creates in a held directory is seen at once.
// A failed walk or a refused sidecar directory is not cached: every caller
// gets its own refusal.
//
// A pass begins at the first use on the serving thread after the previous one
// ended. It ends at the start of each main-loop iteration (client_perform(),
// before main-thread callbacks and scheduled tasks), before each RPC request
// (RpcManager::process(), which runs after the event wait), when another
// session path is asked for, and when check() refuses what the pass holds or
// finds that it was replaced. Each torrent load calls check() first
// (DownloadFactory::receive_success()). Other events handled after the wait,
// such as keyboard input of the interactive UI, keep the pass of the
// iteration's scheduled work.
class SessionDirectoryPass {
public:
  ~SessionDirectoryPass() { end(); }

  // Only the thread named here (the main thread, before its event loop) uses
  // the pass; every other thread walks. A default id names no thread.
  void serve(std::thread::id thread) { end(); m_thread = thread; }
  bool serves_this_thread() const { return std::this_thread::get_id() == m_thread; }

  // A reopened copy with its own directory offset; the caller closes it.
  int open(const std::string& session) {
    const int copy = ::openat(pinned(session), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (copy < 0)
      throw std::runtime_error("cannot reopen session directory: " + m_path + ": " + std::strerror(errno));
    return copy;
  }

  // The pass's descriptor of the sidecar directory `name`, -1 when it does not
  // exist. open_at(session_fd, identity) is the directory's own opener: it
  // attests the directory, stores its stat, returns -1 when it is missing and
  // throws a refusal.
  template <typename OpenAt>
  int sidecar(const std::string& session, const std::string& name, OpenAt&& open_at) {
    const int session_fd = pinned(session);
    const auto cached = m_sidecars.find(name);
    if (cached != m_sidecars.end()) return cached->second.fd;
    Sidecar entry;
    entry.fd = open_at(session_fd, &entry.identity);
    m_sidecars.emplace(name, entry);
    return entry.fd;
  }

  // A writable gateway may have created the directory `name`.
  void forget(const std::string& name) {
    const auto cached = m_sidecars.find(name);
    if (cached == m_sidecars.end()) return;
    if (cached->second.fd >= 0) ::close(cached->second.fd);
    m_sidecars.erase(cached);
  }

  // Before a unit of work the pass attests again what a walk attests: the
  // session path is walked again and must name the pinned inode, and each held
  // sidecar directory must still be admitted under its name. A refusal, a
  // replacement, or a missing directory that is no longer ENOENT ends the
  // pass, so the next use walks and refuses or follows as a walk does.
  void check() {
    if (m_fd < 0) return;
    struct stat st{};
    try {
      ::close(walk_trusted_session_directory(m_path, &st));
    } catch (const std::runtime_error&) {
      end();
      return;
    }
    bool current = same_inode(st, m_identity);
    for (auto entry = m_sidecars.begin(); current && entry != m_sidecars.end(); ++entry) {
      const bool found = ::fstatat(m_fd, entry->first.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0;
      struct stat held{};
      current = entry->second.fd < 0 ? !found && errno == ENOENT :
          found && same_inode(st, entry->second.identity) &&
          ::fstat(entry->second.fd, &held) == 0 && private_sidecar_directory(held);
    }
    if (!current) end();
  }

  void end() {
    for (const auto& entry : m_sidecars)
      if (entry.second.fd >= 0) ::close(entry.second.fd);
    m_sidecars.clear();
    if (m_fd >= 0) ::close(m_fd);
    m_fd = -1;
    m_path.clear();
  }

private:
  struct Sidecar {
    int         fd = -1;
    struct stat identity{};
  };

  static bool same_inode(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
  }

  int pinned(const std::string& session) {
    const std::string path = session_path_absolute(session);
    if (m_fd >= 0 && path == m_path) return m_fd;
    end();
    m_fd = walk_trusted_session_directory(session, &m_identity);
    m_path = path;
    return m_fd;
  }

  std::thread::id                m_thread;
  std::string                    m_path;
  int                            m_fd = -1;
  struct stat                    m_identity{};
  std::map<std::string, Sidecar> m_sidecars;
};

inline SessionDirectoryPass& session_directory_pass() {
  static SessionDirectoryPass pass;
  return pass;
}

inline int open_trusted_session_directory(const std::string& session) {
  auto& pass = session_directory_pass();
  return pass.serves_this_thread() ? pass.open(session) : walk_trusted_session_directory(session);
}

// The read-only gateway of the private sidecar directory `name`; open_at as in
// SessionDirectoryPass::sidecar(). It never creates the directory.
template <typename OpenAt>
inline SidecarDirectory read_session_sidecar(const std::string& session, const std::string& name,
                                             OpenAt&& open_at) {
  auto& pass = session_directory_pass();
  if (pass.serves_this_thread()) return SidecarDirectory(pass.sidecar(session, name, open_at), false);
  const SessionPathFd parent(walk_trusted_session_directory(session));
  return SidecarDirectory(open_at(parent.get(), nullptr), true);
}

// A writable gateway of the sidecar directory `name` is about to create it.
inline void session_sidecar_created(const std::string& name) {
  auto& pass = session_directory_pass();
  if (pass.serves_this_thread()) pass.forget(name);
}

// One unit of work, a torrent load, begins.
inline void session_directory_check() {
  auto& pass = session_directory_pass();
  if (pass.serves_this_thread()) pass.check();
}

// Work that may follow an event wait begins: nothing of the pass carries over.
inline void session_directory_end() {
  auto& pass = session_directory_pass();
  if (pass.serves_this_thread()) pass.end();
}

} // namespace core
