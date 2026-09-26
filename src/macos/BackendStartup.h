#pragma once
#include <chrono>
#include <functional>
#include <string>
#include <sys/types.h>

namespace holder {
using StartupClock = std::chrono::steady_clock;
struct ChildExit {
  std::string message;
  bool possible_lock_contention = false;
};
struct StartupActions {
  std::function<StartupClock::time_point()> now;
  std::function<void(std::chrono::milliseconds)> sleep;
  std::function<bool(std::chrono::milliseconds)> probe;
  // Empty string means success/still running; otherwise a diagnostic.
  std::function<std::string()> start;
  std::function<ChildExit()> child_exit;
  // Optional actionable error from the last probe (e.g. incompatible responder).
  std::function<std::string()> probe_error = {};
};
// Returns an empty string when ready. The budget includes the initial probe.
std::string ensure_backend(const StartupActions& actions,
                           std::chrono::milliseconds budget = std::chrono::seconds(60));
// Nonblocking, reaps an exited child and clears pid; never signals the process.
ChildExit backend_exit_status(pid_t& pid);
}
