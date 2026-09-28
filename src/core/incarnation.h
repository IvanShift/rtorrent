#ifndef RTORRENT_CORE_INCARNATION_H
#define RTORRENT_CORE_INCARNATION_H

#include <fstream>
#include <string>
#include <torrent/exceptions.h>

namespace core {
inline bool valid_incarnation(const std::string& value) {
  if (value.size() != 32) return false;
  for (char c : value)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}
inline std::string fresh_incarnation() {
  char bytes[16];
  std::ifstream random("/dev/urandom", std::ios::in | std::ios::binary);
  if (!random.read(bytes, sizeof(bytes)))
    throw torrent::input_error("Cannot obtain daemon incarnation ID.");
  static constexpr char hex[] = "0123456789abcdef";
  std::string result(32, '0');
  for (unsigned i = 0; i < sizeof(bytes); ++i) {
    const unsigned char value = static_cast<unsigned char>(bytes[i]);
    result[2 * i] = hex[value >> 4];
    result[2 * i + 1] = hex[value & 15];
  }
  return result;
}
}
#endif
