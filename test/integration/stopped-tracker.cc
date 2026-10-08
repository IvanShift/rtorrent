// A local HTTP tracker fixture. It answers every announce with an empty peer
// list, except event=stopped: it records that request and never answers it,
// so a daemon shutdown keeps waiting on its stopped announces.
#include <arpa/inet.h>
#include <fstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 4) return 2;  // PORT READY_FILE STOPPED_FILE
  const int server = ::socket(AF_INET, SOCK_STREAM, 0);
  if (server < 0) return 1;
  const int reuse = 1;
  ::setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(std::stoi(argv[1]));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || ::listen(server, 128))
    return 1;
  std::ofstream(argv[2]) << "ready";
  std::vector<int> held;
  const std::string body = "d8:intervali3600e5:peers0:e";
  const std::string answer = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;
  for (;;) {
    const int client = ::accept(server, nullptr, nullptr);
    if (client < 0) continue;
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 65536) {
      const auto received = ::read(client, buffer, sizeof(buffer));
      if (received <= 0) break;
      request.append(buffer, static_cast<size_t>(received));
    }
    if (request.substr(0, request.find("\r\n")).find("event=stopped") != std::string::npos) {
      std::ofstream(argv[3], std::ios::app) << "stopped\n";
      held.push_back(client);  // Held open and never answered.
      continue;
    }
    for (size_t offset = 0; offset < answer.size();) {
      const auto written = ::write(client, answer.data() + offset, answer.size() - offset);
      if (written <= 0) break;
      offset += static_cast<size_t>(written);
    }
    ::close(client);
  }
}
