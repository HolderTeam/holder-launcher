#include "BackendStartup.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/wait.h>

namespace holder {
std::string ensure_backend(const StartupActions& a) {
  using namespace std::chrono_literals;
  const auto deadline = a.now() + 60s;
  auto remaining = [&] {
    return std::max(0ms, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - a.now()));
  };
  auto probe = [&] {
    const auto budget = std::min(1000ms, remaining());
    return budget > 0ms && a.probe(budget) && a.now() <= deadline;
  };
  const std::string timeout = "Holder backend did not become ready within 60 seconds.";
  if (probe()) return {};
  if (remaining() == 0ms) return timeout;
  if (const auto error = a.start(); !error.empty()) return error;
  while (remaining() > 0ms) {
    if (probe()) return {};
    if (const auto exit = a.child_exit(); !exit.empty()) {
      // A competing launcher may have started the daemon that won its lock.
      if (probe()) return {};
      return exit;
    }
    const auto delay = std::min(250ms, remaining());
    if (delay > 0ms) a.sleep(delay);
  }
  return timeout;
}

std::string backend_exit_status(pid_t& pid) {
  if (pid <= 0) return {};
  int status = 0;
  const auto result = waitpid(pid, &status, WNOHANG);
  if (result == 0 || (result < 0 && errno == EINTR)) return {};
  if (result < 0) {
    const auto error = std::string(std::strerror(errno));
    pid = 0;
    return "Could not check Holder backend process: " + error;
  }
  pid = 0;
  if (WIFEXITED(status))
    return "Holder backend exited before becoming ready (exit code " + std::to_string(WEXITSTATUS(status)) + ").";
  if (WIFSIGNALED(status))
    return "Holder backend exited before becoming ready (signal " + std::to_string(WTERMSIG(status)) + ").";
  return "Holder backend exited before becoming ready.";
}
}
