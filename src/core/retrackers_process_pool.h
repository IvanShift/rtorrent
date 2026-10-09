#pragma once

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#endif
#include <torrent/exceptions.h>
#include "core/session_path.h"

extern char** environ;

namespace core::retrackers_process_pool {

// Only the daemon's main thread owns these direct children. An unreaped direct
// child PID cannot be reused; no persisted PID or timeout authorizes a signal.
// Eight worker reservations plus one independent dispatcher bound all launches.
struct Child { pid_t pid = 0; std::string identity; };
inline std::array<Child, 9> children{};
inline bool shutting_down = false;
inline std::string php_executable;
inline std::string application_root;

// Read once during daemon startup, before accepting any RPC request. RPCs
// select only immutable job identities and users, never executable paths.
inline void initialize_policy() {
  const char* php = ::getenv("RUTORRENT_NATIVE_PHP");
  const char* root = ::getenv("RUTORRENT_NATIVE_APP_ROOT");
  php_executable = php ? php : "/usr/bin/php85";
  application_root = root ? root : "/rutorrent/app";
}


// Identities of reaped worker children, oldest first. A reaped exit of the
// exact native-owned child is the only proof that its worker is gone; the
// retrackers recovery takes them from here. The dispatcher slot is no worker.
struct Exit { std::string identity; int status; };
inline std::vector<Exit> exited_workers;

inline void reap() {
  for (size_t slot = 0; slot < children.size(); ++slot) {
    auto& child = children[slot];
    if (child.pid <= 0) continue;
    int status = -1;
    const pid_t result = ::waitpid(child.pid, &status, WNOHANG);
    if (result == child.pid || (result < 0 && errno == ECHILD)) {
      if (slot < 8 && !shutting_down) exited_workers.push_back({child.identity, result == child.pid ? status : -1});
      child = {};
    }
  }
}

// Whether any worker slot holds a child or a launch reservation.
inline bool workers_running() {
  for (size_t slot = 0; slot < 8; ++slot)
    if (children[slot].pid != 0) return true;
  return false;
}

inline pid_t worker_pid(const std::string& identity) {
  reap();
  for (size_t slot = 0; slot < 8; ++slot)
    if (children[slot].pid > 0 && children[slot].identity == identity)
      return children[slot].pid;
  return 0;
}

// Whether a worker launch could take a slot now; exited children are reaped.
inline bool worker_slot_free() {
  reap();
  for (size_t slot = 0; slot < 8; ++slot)
    if (children[slot].pid == 0) return true;
  return false;
}

inline bool active(const std::string& identity) {
  reap();
  // Reap releases the slot, but its owner must account the exit before this
  // exact identity can launch again. Otherwise a dispatcher can replace the
  // launch receipt and let the old exit charge the new live worker.
  for (const auto& exited : exited_workers)
    if (exited.identity == identity) return true;
  for (const auto& child : children)
    if (child.pid != 0 && child.identity == identity) return true;
  return false;
}

inline unsigned move_workers() {
  reap();
  unsigned count = 0;
  for (size_t slot = 0; slot < 8; ++slot)
    if (children[slot].pid != 0 && children[slot].identity.rfind("move:", 0) == 0) ++count;
  return count;
}

inline void validate_path(const std::string& path, bool executable) {
  if (path.empty() || path == "/" || path[0] != '/' || path.find('\0') != std::string::npos)
    throw torrent::input_error("RR process path must be absolute and canonical.");
  const std::filesystem::path parsed(path);
  if (parsed.lexically_normal().string() != path)
    throw torrent::input_error("RR process path must be absolute and canonical.");
  SessionPathFd parent(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  for (auto it = ++parsed.begin(); it != parsed.end(); ++it) {
    const bool leaf = std::next(it) == parsed.end();
    const std::string component = it->string();
    struct stat ancestor{};
    if (::fstat(parent.get(), &ancestor) < 0 ||
        (ancestor.st_uid != 0 && ancestor.st_uid != ::geteuid()) ||
        ((ancestor.st_mode & 0022) != 0 && (ancestor.st_mode & S_ISVTX) == 0))
      throw torrent::input_error("RR process path ancestor is unsafe.");
    const int opened = ::openat(parent.get(), component.c_str(),
        O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (leaf ? O_NONBLOCK : O_DIRECTORY));
    if (opened < 0) throw torrent::input_error("RR process path cannot be opened safely.");
    SessionPathFd next(opened);
    struct stat st{};
    if (::fstat(next.get(), &st) < 0 || (st.st_uid != 0 && st.st_uid != ::geteuid()) ||
        (leaf ? (!S_ISREG(st.st_mode) || st.st_nlink != 1 || (st.st_mode & 0022) != 0 ||
                 (executable && ((st.st_mode & 0111) == 0 || (st.st_mode & (S_ISUID | S_ISGID)) != 0))) : !S_ISDIR(st.st_mode)))
      throw torrent::input_error("RR process path ownership, type or mode is unsafe.");
#ifdef __linux__
    if (leaf && executable) {
      errno = 0;
      if (::fgetxattr(next.get(), "security.capability", nullptr, 0) >= 0 ||
          (errno != ENODATA && errno != ENOTSUP))
        throw torrent::input_error("RR process executable capabilities are unsafe or unreadable.");
    }
#endif
    parent.reset(next.release());
  }
}

inline std::string launch(const std::string& identity, const std::vector<std::string>& argv,
                          bool dispatcher) {
#ifndef __linux__
  throw torrent::input_error("RR bounded worker launch requires Linux parent-death support.");
#endif
  if (shutting_down) return "held";
  if (active(identity)) return "active";
  if (!dispatcher && identity.rfind("move:", 0) == 0 && move_workers() >= 2) return "full";
  validate_path(argv.at(0), true);
  validate_path(argv.at(1), false);
  const size_t first = dispatcher ? 8 : 0;
  const size_t last = dispatcher ? 9 : 8;
  size_t slot = first;
  while (slot != last && children[slot].pid != 0) ++slot;
  if (slot == last) return dispatcher ? "busy" : "full";
  std::vector<char*> arguments;
  for (const auto& argument : argv) arguments.push_back(const_cast<char*>(argument.c_str()));
  arguments.push_back(nullptr);
  struct rlimit limit{};
  if (::getrlimit(RLIMIT_NOFILE, &limit) != 0)
    throw torrent::input_error("RR process descriptor limit unavailable.");
  char** raw_arguments = arguments.data();
  const pid_t parent = ::getpid();
  children[slot] = {-1, identity}; // Reserve before fork, including failed launches.
  const pid_t pid = ::fork();
  if (pid < 0) { children[slot] = {}; throw torrent::input_error("RR process launch failed."); }
  if (pid == 0) {
    // The daemon is multithreaded. Only async-signal-safe syscalls follow fork;
    // argv, limits and all C++ storage were prepared by the parent.
#ifdef __linux__
    if (::syscall(SYS_prctl, PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) != 0 || ::getppid() != parent) ::_exit(125);
    if (::syscall(SYS_close_range, 3, std::numeric_limits<unsigned>::max(), 0) != 0)
#endif
      for (rlim_t fd = 3; fd < limit.rlim_cur; ++fd) ::close(static_cast<int>(fd));
    ::execve(raw_arguments[0], raw_arguments, environ);
    ::_exit(126);
  }
  children[slot].pid = pid;
  return "spawned";
}

inline void shutdown() {
  shutting_down = true;
  reap();
  for (auto& child : children) {
    if (child.pid <= 0) continue;
    // No intervening waitpid can reap this main-thread-owned child. SIGKILL
    // cancels execution, while durable transaction claims remain recoverable.
    (void)::kill(child.pid, SIGKILL);
    while (::waitpid(child.pid, nullptr, 0) < 0 && errno == EINTR) {}
    child = {};
  }
}

} // namespace core::retrackers_process_pool
