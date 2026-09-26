#include "BackendProbe.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <string>
#include <string_view>

namespace holder {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kMaxResponse = 8192;

struct Socket {
  int fd;
  ~Socket() { if (fd >= 0) close(fd); }
};

bool wait_for(int fd, short events, Clock::time_point deadline) {
  for (;;) {
    const auto now = Clock::now();
    if (now >= deadline) return false;
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    pollfd item{fd, events, 0};
    const int rc = poll(&item, 1, static_cast<int>(remaining.count()));
    if (rc > 0) return (item.revents & (events | POLLHUP | POLLERR)) != 0;
    if (rc == 0 || errno != EINTR) return false;
  }
}

enum class Response { incomplete, valid, invalid };

Response parse_response(std::string_view data, bool eof) {
  const auto end = data.find("\r\n\r\n");
  if (end == std::string_view::npos) return eof ? Response::invalid : Response::incomplete;
  const auto first = data.find("\r\n");
  const auto status = data.substr(0, first);
  if (!(status.starts_with("HTTP/1.1 200 ") || status.starts_with("HTTP/1.0 200 ")))
    return Response::invalid;

  bool has_length = false;
  size_t length = 0;
  for (size_t cursor = first + 2; cursor < end;) {
    const auto next = data.find("\r\n", cursor);
    const auto line = data.substr(cursor, next - cursor);
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return Response::invalid;
    std::string name(line.substr(0, colon));
    for (char& ch : name) {
      const auto c = static_cast<unsigned char>(ch);
      if (!(std::isalnum(c) || std::string_view("!#$%&'*+-.^_`|~").find(ch) != std::string_view::npos))
        return Response::invalid;
      ch = static_cast<char>(std::tolower(c));
    }
    auto value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    // Holder's /ping uses a fixed body, not transfer encoding.
    if (name == "transfer-encoding") return Response::invalid;
    if (name == "content-length") {
      if (has_length) return Response::invalid;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), length);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || length != 4)
        return Response::invalid;
      has_length = true;
    }
    cursor = next + 2;
  }
  const auto body = data.substr(end + 4);
  if (body.size() > 4 || !std::string_view("pong").starts_with(body)) return Response::invalid;
  if (body.size() == 4 && (has_length || eof)) return Response::valid;
  return eof ? Response::invalid : Response::incomplete;
}
} // namespace

bool backend_ping(uint16_t port, std::chrono::milliseconds timeout, bool* incompatible_response) {
  if (incompatible_response) *incompatible_response = false;
  const auto deadline = Clock::now() + timeout;
  Socket sock{socket(AF_INET, SOCK_STREAM, 0)};
  if (sock.fd < 0) return false;
  const int one = 1;
  if (setsockopt(sock.fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0 ||
      fcntl(sock.fd, F_SETFL, O_NONBLOCK) != 0) return false;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(sock.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (errno != EINPROGRESS && errno != EINTR) return false;
    if (!wait_for(sock.fd, POLLOUT, deadline)) return false;
    int error = 0;
    socklen_t size = sizeof(error);
    if (getsockopt(sock.fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0) return false;
  }

  constexpr std::string_view request = "GET /ping HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  size_t sent = 0;
  while (sent < request.size()) {
    if (!wait_for(sock.fd, POLLOUT, deadline)) return false;
    const auto count = send(sock.fd, request.data() + sent, request.size() - sent, 0);
    if (count > 0) sent += static_cast<size_t>(count);
    else if (count == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) return false;
  }

  std::string response;
  while (response.size() < kMaxResponse) {
    if (!wait_for(sock.fd, POLLIN, deadline)) return false;
    char buffer[1024];
    const auto count = recv(sock.fd, buffer, std::min(sizeof(buffer), kMaxResponse - response.size()), 0);
    if (count < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      return false;
    }
    response.append(buffer, static_cast<size_t>(count));
    const auto result = parse_response(response, count == 0);
    if (result != Response::incomplete) {
      if (incompatible_response) *incompatible_response = result == Response::invalid && !response.empty();
      return result == Response::valid;
    }
  }
  if (incompatible_response) *incompatible_response = true;
  return false;
}
} // namespace holder
