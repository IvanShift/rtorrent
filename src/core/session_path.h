#pragma once

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

namespace core {

// The main-thread caller owns this attested private session directory. Only
// trusted same-UID processes can race the absence check on the fallback path.
// Keep file and directory durability at the publication call site.
inline int rename_session_entry_noreplace(int directory, const std::string& source,
                                           const std::string& destination) {
#ifdef __linux__
  int result;
#ifdef ENABLE_RR_MIGRATION_TEST_CUTS
  const char* cut = ::getenv("RR_MIG_TEST_CUT");
  int injected = 0;
  if (cut && std::strcmp(cut, "rename-noreplace-einval") == 0) injected = EINVAL;
  if (cut && std::strcmp(cut, "rename-noreplace-enosys") == 0) injected = ENOSYS;
  if (cut && std::strcmp(cut, "rename-noreplace-eopnotsupp") == 0) injected = EOPNOTSUPP;
  if (cut && std::strcmp(cut, "rename-noreplace-eacces") == 0) injected = EACCES;
  if (injected) { errno = injected; result = -1; }
  else
#endif
    result = ::syscall(SYS_renameat2, directory, source.c_str(), directory,
                       destination.c_str(), 1 /* RENAME_NOREPLACE */);
  if (result == 0 || (errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP))
    return result;
  // WSL drvfs over 9p rejects rename flags even on a current Linux kernel.
  // A dangling symlink is also an existing target and must never be replaced.
  struct stat target{};
  if (::fstatat(directory, destination.c_str(), &target, AT_SYMLINK_NOFOLLOW) == 0) {
    errno = EEXIST;
    return -1;
  }
  if (errno != ENOENT) return -1;
  return ::renameat(directory, source.c_str(), directory, destination.c_str());
#else
  // Preserve the historical no-overwrite primitive outside Linux.
  if (::linkat(directory, source.c_str(), directory, destination.c_str(), 0) != 0) return -1;
  return ::unlinkat(directory, source.c_str(), 0);
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
inline int open_trusted_session_directory(const std::string& session) {
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
  return parent.release();
}

} // namespace core
