#pragma once
#include <chrono>
#include <functional>
#include <string>
#include <sys/types.h>

namespace holder {
using StartupClock = std::chrono::steady_clock;
struct StartupActions {
  std::function<StartupClock::time_point()> now;
  std::function<void(std::chrono::milliseconds)> sleep;
  std::function<bool(std::chrono::milliseconds)> probe;
  // Empty string means success/still running; otherwise a diagnostic.
  std::function<std::string()> start;
  std::function<std::string()> child_exit;
};
// Returns an empty string when ready. The budget includes the initial probe.
std::string ensure_backend(const StartupActions& actions);
// Nonblocking, reaps an exited child and clears pid; never signals the process.
std::string backend_exit_status(pid_t& pid);
}
