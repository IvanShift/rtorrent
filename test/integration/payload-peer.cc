// A bounded loopback BEP-3 fixture. No payload is sent before the release marker.
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
Clock::time_point deadline;
std::string prefix;

struct Fd {
  explicit Fd(int value) : value(value) {}
  ~Fd() { if (value >= 0) ::close(value); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int value;
};

void check_deadline() {
  if (Clock::now() >= deadline) throw std::runtime_error("fixture deadline expired");
}

void ready(int fd, short events) {
  while (true) {
    check_deadline();
    pollfd item{fd, events, 0};
    const int result = ::poll(&item, 1, 100);
    if (result < 0 && errno != EINTR) throw std::runtime_error("fixture poll failed");
    if (result > 0) {
      if (item.revents & (events | POLLHUP)) return;
      if (item.revents & (POLLERR | POLLNVAL)) throw std::runtime_error("fixture socket unavailable");
    }
  }
}

std::string read_bytes(int fd, size_t count) {
  std::string bytes(count, '\0');
  for (size_t offset = 0; offset < count;) {
    ready(fd, POLLIN);
    const auto received = ::recv(fd, bytes.data() + offset, count - offset, MSG_DONTWAIT);
    if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (received <= 0) throw std::runtime_error("fixture peer closed");
    offset += static_cast<size_t>(received);
  }
  return bytes;
}

void write_bytes(int fd, const std::string& bytes) {
  for (size_t offset = 0; offset < bytes.size();) {
    ready(fd, POLLOUT);
    const auto written = ::send(fd, bytes.data() + offset, bytes.size() - offset,
                                MSG_DONTWAIT | MSG_NOSIGNAL);
    if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (written <= 0) throw std::runtime_error("fixture peer write failed");
    offset += static_cast<size_t>(written);
  }
}

std::string number(uint32_t value) {
  const auto network = htonl(value);
  return std::string(reinterpret_cast<const char*>(&network), sizeof(network));
}

uint32_t number(const std::string& bytes, size_t offset) {
  uint32_t value = 0;
  for (size_t i = offset; i < offset + 4; ++i) value = (value << 8) | uint8_t(bytes[i]);
  return value;
}

void message(int fd, const std::string& body) {
  write_bytes(fd, number(static_cast<uint32_t>(body.size())) + body);
}

uint32_t argument(const char* text, uint32_t maximum) {
  uint64_t value = 0;
  for (const unsigned char c : std::string(text)) {
    if (c < '0' || c > '9') throw std::runtime_error("fixture nondecimal argument");
    value = value * 10 + c - '0';
    if (value > maximum) throw std::runtime_error("fixture argument out of bounds");
  }
  if (!value) throw std::runtime_error("fixture zero argument");
  return static_cast<uint32_t>(value);
}

std::string info_hash(const std::string& hex) {
  if (hex.size() != 40) throw std::runtime_error("fixture info hash length invalid");
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    throw std::runtime_error("fixture info hash alphabet invalid");
  };
  std::string result;
  for (size_t i = 0; i < hex.size(); i += 2) result += char(digit(hex[i]) * 16 + digit(hex[i + 1]));
  return result;
}

void marker(const std::string& suffix, const std::string& text) {
  Fd fd(::open((prefix + suffix).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644));
  if (fd.value < 0) throw std::runtime_error("fixture marker publication failed");
  size_t offset = 0;
  while (offset < text.size()) {
    const auto written = ::write(fd.value, text.data() + offset, text.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) throw std::runtime_error("fixture marker write failed");
    offset += static_cast<size_t>(written);
  }
}

void await_release() {
  const auto path = prefix + ".release";
  while (true) {
    check_deadline();
    Fd fd(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (fd.value >= 0) {
      struct stat stat{};
      char text[9]{};
      if (::fstat(fd.value, &stat) || !S_ISREG(stat.st_mode) || stat.st_size != 8 ||
          ::read(fd.value, text, sizeof(text)) != 8 || std::string(text, 8) != "release\n")
        throw std::runtime_error("fixture release marker invalid");
      return;
    }
    if (errno != ENOENT) throw std::runtime_error("fixture release marker unreadable");
    ::poll(nullptr, 0, 10);
  }
}
} // namespace

int main(int argc, char** argv) {
  if (argc != 6 && argc != 7) {
    std::cerr << "usage: payload-peer PAYLOAD INFO_HASH_HEX PIECE_LENGTH PORT MARKER_PREFIX [TIMEOUT_SECONDS]\n";
    return 2;
  }
  try {
    const auto piece_length = argument(argv[3], 8 * 1024 * 1024);
    const auto port = argument(argv[4], 65535);
    const auto timeout = argc == 7 ? argument(argv[6], 300) : 90;
    deadline = Clock::now() + std::chrono::seconds(timeout);
    const auto hash = info_hash(argv[2]);
    prefix = argv[5];
    if (prefix.empty()) throw std::runtime_error("fixture marker prefix empty");
    for (const auto* suffix : {".ready", ".connected", ".requested", ".release", ".done", ".error"}) {
      struct stat stat{};
      if (!::lstat((prefix + suffix).c_str(), &stat) || errno != ENOENT)
        throw std::runtime_error("fixture marker prefix not fresh");
    }
    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("fixture payload unavailable");
    const auto size = input.tellg();
    if (size <= 0 || size > 8 * 1024 * 1024) throw std::runtime_error("fixture payload size invalid");
    std::string payload(static_cast<size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(payload.data(), static_cast<std::streamsize>(payload.size())))
      throw std::runtime_error("fixture payload read failed");
    const auto pieces = (payload.size() + piece_length - 1) / piece_length;

    Fd server(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (server.value < 0) throw std::runtime_error("fixture socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(server.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
        ::listen(server.value, 1)) throw std::runtime_error("fixture listen failed");
    marker(".ready", "ready\n");
    ready(server.value, POLLIN);
    Fd peer(::accept4(server.value, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
    if (peer.value < 0) throw std::runtime_error("fixture accept failed");
    const auto handshake = read_bytes(peer.value, 68);
    if (handshake.substr(0, 20) != std::string("\x13" "BitTorrent protocol", 20) ||
        handshake.substr(28, 20) != hash) throw std::runtime_error("fixture handshake mismatch");
    write_bytes(peer.value, std::string("\x13" "BitTorrent protocol", 20) + std::string(8, '\0') +
                hash + "-RRDATA-" + std::string(12, '0'));
    std::string bitfield((pieces + 7) / 8, '\0');
    for (size_t i = 0; i < pieces; ++i) bitfield[i / 8] |= char(0x80 >> (i % 8));
    message(peer.value, std::string(1, char(5)) + bitfield);
    message(peer.value, std::string(1, char(1)));
    marker(".connected", "connected\n");

    bool released = false;
    std::vector<bool> covered(payload.size());
    size_t sent = 0;
    while (sent < payload.size()) {
      const auto length = number(read_bytes(peer.value, 4), 0);
      if (!length) continue;
      if (length > 65536) throw std::runtime_error("fixture message oversized");
      const auto body = read_bytes(peer.value, length);
      if (uint8_t(body[0]) != 6) continue;
      if (body.size() != 13) throw std::runtime_error("fixture request length invalid");
      const auto piece = number(body, 1), begin = number(body, 5), count = number(body, 9);
      const uint64_t offset = uint64_t(piece) * piece_length + begin;
      if (piece >= pieces || begin >= piece_length || count == 0 || count > 16384 ||
          uint64_t(begin) + count > piece_length || offset + count > payload.size())
        throw std::runtime_error("fixture request bounds invalid");
      if (!released) {
        marker(".requested", "piece=" + std::to_string(piece) + " begin=" + std::to_string(begin) +
               " length=" + std::to_string(count) + "\n");
        await_release();
        released = true;
      }
      message(peer.value, std::string(1, char(7)) + number(piece) + number(begin) +
              payload.substr(static_cast<size_t>(offset), count));
      for (size_t i = static_cast<size_t>(offset); i < offset + count; ++i) {
        if (!covered[i]) { covered[i] = true; ++sent; }
      }
    }
    marker(".done", "payload-bytes=" + std::to_string(sent) + "\n");
    // The marker counts sent bytes, not client consumption. Queue FIN after
    // every payload byte and consume late client messages before closing.
    // Closing with unread inbound bytes can reset TCP and discard queued output.
    if (::shutdown(peer.value, SHUT_WR)) throw std::runtime_error("fixture shutdown failed");
    char discarded[4096];
    while (true) {
      ready(peer.value, POLLIN);
      const auto received = ::recv(peer.value, discarded, sizeof(discarded), MSG_DONTWAIT);
      if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
      if (received < 0) throw std::runtime_error("fixture drain failed");
      if (!received) break;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    if (!prefix.empty()) {
      try { marker(".error", std::string(error.what()) + '\n'); } catch (const std::exception&) {}
    }
    return 1;
  }
}
