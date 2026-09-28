#ifndef RTORRENT_CORE_INCARNATION_H
#define RTORRENT_CORE_INCARNATION_H

#include <cerrno>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <torrent/exceptions.h>

namespace core {
inline bool valid_incarnation(const std::string& value) {
  if (value.size() != 32) return false;
  for (char c : value)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}
template <typename Error>
inline std::string random_hex16(const char* open_error, const char* read_error) {
  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw Error(open_error ? open_error : std::strerror(errno));
  unsigned char bytes[16];
  size_t offset = 0;
  while (offset < sizeof(bytes)) {
    const ssize_t count = ::read(fd, bytes + offset, sizeof(bytes) - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      ::close(fd);
      throw Error(read_error);
    }
    offset += static_cast<size_t>(count);
  }
  ::close(fd);
  static constexpr char hex[] = "0123456789abcdef";
  std::string result(32, '0');
  for (unsigned i = 0; i < sizeof(bytes); ++i) {
    const unsigned char value = bytes[i];
    result[2 * i] = hex[value >> 4];
    result[2 * i + 1] = hex[value & 15];
  }
  return result;
}
inline std::string fresh_incarnation() {
  return random_hex16<torrent::input_error>(
      "Cannot obtain daemon incarnation ID.", "Cannot obtain daemon incarnation ID.");
}
}
#endif
