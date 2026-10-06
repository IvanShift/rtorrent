// A local BEP-9 fixture; it serves only the synthetic info dictionary supplied by the test.
#include <arpa/inet.h>
#include <fstream>
#include <iostream>
#include <openssl/sha.h>
#include <regex>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

static std::string read_bytes(int fd, size_t count) {
  std::string bytes(count, '\0');
  size_t offset = 0;
  while (offset < count) {
    const auto received = ::read(fd, bytes.data() + offset, count - offset);
    if (received <= 0) throw std::runtime_error("fixture peer closed");
    offset += received;
  }
  return bytes;
}

static void write_bytes(int fd, const std::string& bytes) {
  size_t offset = 0;
  while (offset < bytes.size()) {
    const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written <= 0) throw std::runtime_error("fixture peer write failed");
    offset += written;
  }
}

static void message(int fd, const std::string& body) {
  const uint32_t length = htonl(body.size());
  write_bytes(fd, std::string(reinterpret_cast<const char*>(&length), 4) + body);
}

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  try {
    std::ifstream input(argv[1], std::ios::binary);
    const std::string info(std::istreambuf_iterator<char>{input}, {});
    if (!input || info.empty() || info.size() > 16384) return 2;
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(info.data()), info.size(), hash);
    const int server = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) throw std::runtime_error("fixture socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(std::stoi(argv[2]));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || ::listen(server, 1))
      throw std::runtime_error("fixture listen failed");
    std::ofstream(argv[3]) << "ready";
    const int peer = ::accept(server, nullptr, nullptr);
    if (peer < 0) throw std::runtime_error("fixture accept failed");
    timeval timeout{20, 0};
    ::setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    read_bytes(peer, 68);
    write_bytes(peer, std::string("\x13" "BitTorrent protocol") + std::string("\0\0\0\0\0\x10\0\0", 8) +
        std::string(reinterpret_cast<const char*>(hash), 20) + "-RRTEST-000000000000");
    message(peer, std::string("\x14\0", 2) + "d1:md11:ut_metadatai1ee13:metadata_sizei" +
        std::to_string(info.size()) + "e1:v11:native-teste");
    int remote_id = 0;
    while (true) {
      const auto length = read_bytes(peer, 4);
      uint32_t size = 0;
      for (unsigned char byte : length) size = (size << 8) | byte;
      if (!size) continue;
      if (size > 65536) throw std::runtime_error("fixture message oversized");
      const auto body = read_bytes(peer, size);
      if (body.size() < 2 || body[0] != 20) continue;
      std::smatch match;
      if (body[1] == 0 && std::regex_search(body, match, std::regex("11:ut_metadatai([0-9]+)e")))
        remote_id = std::stoi(match[1]);
      if (body[1] == 1 && remote_id) {
        message(peer, std::string{char(20), char(remote_id)} + "d8:msg_typei1e5:piecei0e10:total_sizei" +
            std::to_string(info.size()) + "ee" + info);
        ::sleep(1);
        break;
      }
    }
    ::close(peer);
    ::close(server);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
