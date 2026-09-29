#pragma once

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace core {

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
  const std::string path = session[0] == '/' ? session :
      std::filesystem::current_path().string() + "/" + session;
  SessionPathFd parent(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  for (size_t begin = 1; begin <= path.size();) {
    const size_t end = path.find('/', begin);
    const std::string component = path.substr(begin, end - begin);
    if (!component.empty() && component != ".") {
      struct stat ancestor{}, child{};
      if (::fstat(parent.get(), &ancestor) != 0 ||
          (ancestor.st_uid != 0 && ancestor.st_uid != ::geteuid()) ||
          ((ancestor.st_mode & (S_IWGRP | S_IWOTH)) != 0 && (ancestor.st_mode & S_ISVTX) == 0))
        throw std::runtime_error("unsafe session path ancestor ownership or mode");
      const int next = ::openat(parent.get(), component.c_str(),
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (next < 0) throw std::runtime_error(std::string("cannot open session path component: ") + std::strerror(errno));
      if (::fstat(next, &child) != 0 || !S_ISDIR(child.st_mode) ||
          (child.st_uid != 0 && child.st_uid != ::geteuid())) {
        ::close(next);
        throw std::runtime_error("unsafe session path component ownership or mode");
      }
      parent.reset(next);
    }
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  struct stat st{};
  if (::fstat(parent.get(), &st) != 0 || !S_ISDIR(st.st_mode) ||
      st.st_uid != ::geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH)) != 0)
    throw std::runtime_error("unsafe session directory ownership or mode");
  return parent.release();
}

} // namespace core
