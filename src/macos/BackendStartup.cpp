#include "BackendStartup.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/wait.h>

namespace holder {
std::string ensure_backend(const StartupActions& a, std::chrono::milliseconds budget) {
  using namespace std::chrono_literals;
  const auto deadline = a.now() + budget;
  auto remaining = [&] {
    return std::max(0ms, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - a.now()));
  };
  auto probe = [&] {
    const auto budget = std::min(1000ms, remaining());
    return budget > 0ms && a.probe(budget) && a.now() <= deadline;
  };
  auto probe_error = [&] { return a.probe_error ? a.probe_error() : std::string{}; };
  const std::string timeout = "Holder backend did not become ready within " +
      std::to_string(std::chrono::duration_cast<std::chrono::seconds>(budget).count()) + " seconds.";
  std::string contention_exit;
  if (probe()) return {};
  if (auto error = probe_error(); !error.empty()) return error;
  if (remaining() == 0ms) return timeout;
  if (const auto error = a.start(); !error.empty()) return error;
  while (remaining() > 0ms) {
    if (probe()) return {};
    if (auto error = probe_error(); !error.empty()) return error;
    if (const auto exit = a.child_exit(); !exit.message.empty()) {
      // A competing launcher may have started the daemon that won its lock.
      if (probe()) return {};
      if (auto error = probe_error(); !error.empty()) return error;
      if (!exit.possible_lock_contention) return exit.message;
      // Daemon exit 2 can mean its instance lock is held. It also covers other
      // startup errors, so retain the diagnostic if no winner becomes ready.
      contention_exit = exit.message;
    }
    const auto delay = std::min(250ms, remaining());
    if (delay > 0ms) a.sleep(delay);
  }
  if (const auto exit = a.child_exit(); !exit.message.empty()) contention_exit = exit.message;
  return contention_exit.empty() ? timeout : contention_exit + " " + timeout;
}

ChildExit backend_exit_status(pid_t& pid) {
  if (pid <= 0) return {};
  int status = 0;
  const auto result = waitpid(pid, &status, WNOHANG);
  if (result == 0 || (result < 0 && errno == EINTR)) return {};
  if (result < 0) {
    const auto error = std::string(std::strerror(errno));
    pid = 0;
    return {"Could not check Holder backend process: " + error};
  }
  pid = 0;
  if (WIFEXITED(status))
    return {"Holder backend exited before becoming ready (exit code " + std::to_string(WEXITSTATUS(status)) + ").", WEXITSTATUS(status) == 2};
  if (WIFSIGNALED(status))
    return {"Holder backend exited before becoming ready (signal " + std::to_string(WTERMSIG(status)) + ")."};
  return {"Holder backend exited before becoming ready."};
}
}
