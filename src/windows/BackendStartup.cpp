#include "BackendStartup.h"
#include <algorithm>

namespace holder {
std::wstring ensure_backend(const StartupActions& a, std::chrono::milliseconds budget) {
  using namespace std::chrono_literals;
  const auto deadline = a.now() + budget;
  const auto remaining = [&] {
    return std::max(0ms, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - a.now()));
  };
  const auto probe = [&] {
    const auto allowance = std::min(1000ms, remaining());
    if (allowance <= 0ms) return ProbeResult::unavailable;
    const auto result = a.probe(allowance);
    return a.now() <= deadline ? result : ProbeResult::unavailable;
  };
  const std::wstring collision =
      L"The service at 127.0.0.1:11499 did not return the expected Holder ping response.\n\n"
      L"Close the conflicting service and try again. Holder has not opened the desktop.";
  const auto duration = budget.count() % 1000 == 0
      ? std::to_wstring(budget.count() / 1000) + L" seconds"
      : std::to_wstring(budget.count()) + L" milliseconds";
  const std::wstring timeout = L"Holder backend did not become ready within " + duration + L".";
  std::wstring contention_exit;

  const auto initial = probe();
  if (initial == ProbeResult::healthy) return {};
  if (initial == ProbeResult::incompatible) return collision;
  if (remaining() == 0ms) return timeout;
  if (const auto error = a.start(); !error.empty()) return error;

  while (remaining() > 0ms) {
    const auto result = probe();
    if (result == ProbeResult::healthy) return {};
    if (result == ProbeResult::incompatible) return collision;
    if (const auto exit = a.child_exit(); !exit.message.empty()) {
      // The daemon owns the instance lock. A competing launch may already be
      // healthy, or may still be starting after this child exits with code 2.
      const auto final_probe = probe();
      if (final_probe == ProbeResult::healthy) return {};
      if (final_probe == ProbeResult::incompatible) return collision;
      if (!exit.possible_lock_contention) return exit.message;
      // Code 2 also covers other daemon startup errors: retain it on timeout.
      contention_exit = exit.message;
    }
    const auto delay = std::min(250ms, remaining());
    if (delay > 0ms) a.sleep(delay);
  }
  if (const auto exit = a.child_exit(); !exit.message.empty()) contention_exit = exit.message;
  return contention_exit.empty() ? timeout : contention_exit + L" " + timeout;
}

ChildExit backend_exit_status(HANDLE& process) {
  if (!process) return {};
  const DWORD wait = WaitForSingleObject(process, 0);
  if (wait == WAIT_TIMEOUT) return {};
  DWORD code = 0;
  std::wstring error;
  if (wait == WAIT_FAILED) {
    error = L"Could not wait for Holder backend process (Windows error " +
        std::to_wstring(GetLastError()) + L").";
  } else if (!GetExitCodeProcess(process, &code)) {
    error = L"Could not read Holder backend exit status (Windows error " +
        std::to_wstring(GetLastError()) + L").";
  }
  CloseHandle(process);
  process = nullptr;
  if (!error.empty()) return {error};
  // WaitForSingleObject established that it exited. 259 is a valid exit code
  // here, even though GetExitCodeProcess uses it for a still-running process.
  return {L"Holder backend exited before becoming ready (exit code " +
      std::to_wstring(code) + L").", code == 2};
}
}
