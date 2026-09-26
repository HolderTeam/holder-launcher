#include "BackendStartup.h"
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;
using holder::ProbeResult;

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
struct Scenario {
  holder::StartupClock::time_point now{};
  int starts = 0, probes = 0, exit_checks = 0;
  int ready_on = 0, incompatible_on = 0;
  std::chrono::milliseconds probe_cost{0}, start_cost{0};
  std::wstring spawn_error;
  holder::ChildExit exit;
  std::vector<std::chrono::milliseconds> allowances, sleeps;
  holder::StartupActions actions() {
    return {
        [&] { return now; },
        [&](auto delay) { sleeps.push_back(delay); now += delay; },
        [&](auto allowance) {
          allowances.push_back(allowance);
          now += probe_cost;
          ++probes;
          if (probes == ready_on) return ProbeResult::healthy;
          return probes == incompatible_on ? ProbeResult::incompatible : ProbeResult::unavailable;
        },
        [&] { ++starts; now += start_cost; return spawn_error; },
        [&] {
          ++exit_checks;
          const auto result = exit;
          exit = {};
          return result;
        },
    };
  }
  std::wstring run(std::chrono::milliseconds budget = 60s) {
    return holder::ensure_backend(actions(), budget);
  }
};

void policy_tests() {
  {
    Scenario s; s.ready_on = 1;
    check(s.run().empty() && s.starts == 0 && s.sleeps.empty(), "warm reuse");
  }
  {
    Scenario s; s.ready_on = 3;
    check(s.run().empty() && s.starts == 1 && s.sleeps.size() == 1, "cold readiness");
  }
  {
    Scenario s; s.incompatible_on = 1;
    check(s.run().find(L"expected Holder") != std::wstring::npos && s.starts == 0, "initial collision");
  }
  {
    Scenario s; s.incompatible_on = 2;
    check(s.run().find(L"expected Holder") != std::wstring::npos && s.starts == 1, "late collision");
  }
  {
    Scenario s; s.spawn_error = L"spawn failed";
    check(s.run() == s.spawn_error && s.probes == 1, "spawn failure");
  }
  {
    Scenario s;
    check(!s.run(0ms).empty() && s.starts == 0 && s.probes == 0, "zero budget");
  }
  {
    Scenario s; s.probe_cost = 100ms;
    check(!s.run(100ms).empty() && s.starts == 0 && s.sleeps.empty(), "initial probe consumes budget");
  }
  {
    Scenario s; s.start_cost = 100ms;
    check(!s.run(100ms).empty() && s.probes == 1 && s.exit_checks == 1, "spawn consumes budget");
  }
  {
    Scenario s; s.probe_cost = 50ms;
    check(!s.run(425ms).empty(), "timeout required");
    check(s.allowances == std::vector{425ms, 375ms, 75ms}, "remaining probe allowances");
    check(s.sleeps == std::vector{250ms, 25ms}, "sleep clipped to remaining budget");
    check(s.now.time_since_epoch() == 425ms, "exact shared budget");
  }
  {
    Scenario s; s.probe_cost = 100ms;
    check(!s.run(200ms).empty() && s.sleeps.empty(), "no final sleep after probe exhaustion");
  }
  {
    Scenario s; s.exit = {L"exit code 7", false};
    check(s.run() == L"exit code 7" && s.probes == 3 && s.sleeps.empty(), "early exit and final recheck");
  }
  {
    Scenario s; s.exit = {L"exit code 7", false}; s.ready_on = 3;
    check(s.run().empty() && s.probes == 3, "healthy concurrent winner after exit");
  }
  {
    Scenario s; s.exit = {L"exit code 2", true}; s.ready_on = 5;
    check(s.run().empty() && s.starts == 1 && s.sleeps.size() == 2, "wait for cold winner");
  }
  {
    Scenario s; s.exit = {L"exit code 2", true};
    const auto error = s.run(500ms);
    check(error.find(L"exit code 2") != std::wstring::npos && error.find(L"500 milliseconds") != std::wstring::npos,
          "contention exit preserved at deadline");
    check(s.starts == 1, "never restart child");
  }
  {
    Scenario s; s.exit = {L"exit code 2", true}; s.incompatible_on = 3;
    check(s.run().find(L"expected Holder") != std::wstring::npos, "collision during final recheck");
  }
  {
    Scenario s; s.ready_on = 2; s.probe_cost = 60ms;
    check(!s.run(100ms).empty(), "late healthy result must not pass deadline");
  }
  {
    Scenario s;
    check(s.run().find(L"60 seconds") != std::wstring::npos && s.now.time_since_epoch() == 60s,
          "default 60-second budget");
    check(s.allowances.front() == 1000ms, "individual probe cap");
  }
  std::cout << "17 startup policy cases passed\n";
}

void native_process_tests(const wchar_t* self) {
  for (const DWORD code : {0UL, 2UL, 7UL, 259UL}) {
    std::wstring command = L"\"" + std::wstring(self) + L"\" --child " + std::to_wstring(code);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    check(CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                         nullptr, nullptr, &startup, &child) != 0, "spawn native test child");
    HANDLE process = child.hProcess;
    const bool running = holder::backend_exit_status(process).message.empty() && process != nullptr;
    const auto resumed = ResumeThread(child.hThread);
    CloseHandle(child.hThread);
    check(running && resumed != static_cast<DWORD>(-1), "running child");
    check(WaitForSingleObject(process, 3000) == WAIT_OBJECT_0, "native child completion");
    const auto exit = holder::backend_exit_status(process);
    check(process == nullptr && exit.message.find(std::to_wstring(code)) != std::wstring::npos, "exit code and close");
    check(exit.possible_lock_contention == (code == 2), "contention classification");
    DWORD flags = 0;
    check(!GetHandleInformation(child.hProcess, &flags) && GetLastError() == ERROR_INVALID_HANDLE,
          "observed process handle closed");
    check(holder::backend_exit_status(process).message.empty(), "cleared handle not observed twice");
  }
  std::cout << "4 native child exit cases passed (including exit 259)\n";
}
}

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && std::wstring(argv[1]) == L"--child") {
    Sleep(150);
    return std::stoi(argv[2]);
  }
  try {
    policy_tests();
    native_process_tests(argv[0]);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
