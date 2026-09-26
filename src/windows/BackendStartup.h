#pragma once

#include "BackendProbe.h"
#include <windows.h>
#include <chrono>
#include <functional>
#include <string>

namespace holder {
using StartupClock = std::chrono::steady_clock;
struct ChildExit {
  std::wstring message;
  bool possible_lock_contention = false;
};
struct StartupActions {
  std::function<StartupClock::time_point()> now;
  std::function<void(std::chrono::milliseconds)> sleep;
  std::function<ProbeResult(std::chrono::milliseconds)> probe;
  // Empty means successfully started, or still running, respectively.
  std::function<std::wstring()> start;
  std::function<ChildExit()> child_exit;
};

// Empty means ready. The budget includes the initial probe and process startup;
// OS process creation itself is not interruptible by this polling deadline.
std::wstring ensure_backend(const StartupActions& actions,
    std::chrono::milliseconds budget = std::chrono::seconds(60));

// Nonblocking. Closes and clears an observed exited process handle, without
// terminating the process. The caller retains ownership while it is running.
ChildExit backend_exit_status(HANDLE& process);
}
