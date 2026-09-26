#include "BackendProbe.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {
struct Socket {
  int fd;
  ~Socket() { if (fd >= 0) close(fd); }
};

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

bool send_all(int fd, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const auto n = send(fd, data.data() + sent, data.size() - sent, 0);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

class Server {
 public:
  Socket listener{socket(AF_INET, SOCK_STREAM, 0)};
  uint16_t port = 0;
  std::jthread worker;

  explicit Server(std::function<void(int)> respond) {
    require(listener.fd >= 0, "server socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(listener.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind");
    socklen_t size = sizeof(address);
    require(getsockname(listener.fd, reinterpret_cast<sockaddr*>(&address), &size) == 0, "getsockname");
    port = ntohs(address.sin_port);
    require(listen(listener.fd, 1) == 0, "listen");
    worker = std::jthread([this, respond] {
      pollfd p{listener.fd, POLLIN, 0};
      if (poll(&p, 1, 2000) <= 0) return;
      Socket client{accept(listener.fd, nullptr, nullptr)};
      if (client.fd < 0) return;
      const int one = 1;
      setsockopt(client.fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
      timeval timeout{1, 0};
      setsockopt(client.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      setsockopt(client.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      respond(client.fd);
    });
  }
};

bool read_request(int fd) {
  std::string request;
  char buffer[256];
  while (request.size() < 1024 && request.find("\r\n\r\n") == std::string::npos) {
    const auto n = recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0) return false;
    request.append(buffer, static_cast<size_t>(n));
  }
  return request.starts_with("GET /ping HTTP/1.1\r\n") &&
         request.find("Connection: close\r\n") != std::string::npos;
}

void response_case(const std::string& response, bool expected, bool fragment = false) {
  Server server([=](int fd) {
    if (!read_request(fd)) return;
    if (fragment) {
      for (char ch : response) {
        if (!send_all(fd, std::string(1, ch))) return;
        std::this_thread::sleep_for(1ms);
      }
    } else {
      send_all(fd, response);
    }
  });
  require(holder::backend_ping(server.port, 1s) == expected, "unexpected probe result");
}
} // namespace

int main() {
  // A reset must not kill the test executable through a globally ignored SIGPIPE.
  std::signal(SIGPIPE, SIG_DFL);
  int failed = 0;
  auto test = [&](const char* name, auto fn) {
    try { fn(); std::cout << "PASS " << name << '\n'; }
    catch (const std::exception& error) {
      ++failed;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  };
  const std::string good = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\npong";
  test("valid response and request", [&] { response_case(good, true); });
  test("fragmented status headers and body", [&] { response_case(good, true, true); });
  test("close-delimited body", [] { response_case("HTTP/1.0 200 OK\r\n\r\npong", true); });
  test("case-insensitive length", [] { response_case("HTTP/1.1 200 OK\r\ncOnTeNt-LeNgTh:\t4 \r\n\r\npong", true); });
  test("wrong body", [] { response_case("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nnope", false); });
  test("status substring is not a status", [] { response_case("garbage HTTP/1.1 200 OK\r\n\r\npong", false); });
  test("non-200 and redirect", [] { response_case("HTTP/1.1 302 Found\r\nLocation: /ping\r\n\r\npong", false); });
  test("truncated body", [] { response_case("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\npon", false); });
  test("invalid length", [] { response_case("HTTP/1.1 200 OK\r\nContent-Length: 4x\r\n\r\npong", false); });
  test("duplicate length", [] { response_case("HTTP/1.1 200 OK\r\nContent-Length: 4\r\nContent-Length: 4\r\n\r\npong", false); });
  test("unsupported transfer encoding", [] { response_case("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\npong\r\n0\r\n\r\n", false); });
  test("oversized headers", [] { response_case("HTTP/1.1 200 OK\r\nX-Large: " + std::string(9000, 'x'), false); });
  test("immediate close", [] { response_case("", false); });
  test("reset peer survives SIGPIPE", [] {
    for (int i = 0; i < 20; ++i) {
      Server server([](int fd) { linger reset{1, 0}; setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)); });
      require(!holder::backend_ping(server.port, 200ms), "reset accepted");
    }
  });
  test("silent peer deadline", [] {
    Server server([](int) { std::this_thread::sleep_for(350ms); });
    const auto start = std::chrono::steady_clock::now();
    require(!holder::backend_ping(server.port, 100ms), "silent peer accepted");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    require(elapsed >= 75ms && elapsed < 300ms, "deadline not respected");
  });
  test("slow peer cannot renew deadline", [] {
    Server server([](int fd) {
      if (!read_request(fd)) return;
      for (char ch : std::string("HTTP/1.1 200 OK")) {
        if (!send_all(fd, std::string(1, ch))) break;
        std::this_thread::sleep_for(30ms);
      }
    });
    const auto start = std::chrono::steady_clock::now();
    require(!holder::backend_ping(server.port, 100ms), "slow peer accepted");
    require(std::chrono::steady_clock::now() - start < 300ms, "slow peer extended deadline");
  });
  test("refused connection", [] {
    Socket reserved{socket(AF_INET, SOCK_STREAM, 0)};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(reserved.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "reserve port");
    socklen_t size = sizeof(address);
    require(getsockname(reserved.fd, reinterpret_cast<sockaddr*>(&address), &size) == 0, "reserved port");
    require(!holder::backend_ping(ntohs(address.sin_port), 100ms), "unlistened port accepted");
  });
  return failed == 0 ? 0 : 1;
}
