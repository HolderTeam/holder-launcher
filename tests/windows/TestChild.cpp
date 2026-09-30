#include <windows.h>
#include <filesystem>
#include <fstream>
#include <cwchar>
#include <string>

int wmain(int argc, wchar_t** argv) {
  if (argc < 1) return 2;
  wchar_t marker_delay[32]{};
  if (GetEnvironmentVariableW(L"HOLDER_TEST_MARKER_DELAY", marker_delay, 32) > 0)
    Sleep(static_cast<DWORD>(std::wcstoul(marker_delay, nullptr, 10)));
  const auto name = std::filesystem::path(argv[0]).filename().string();
  const auto pid = std::to_string(GetCurrentProcessId());
  std::ofstream(name + "." + pid + ".started") << pid;
  std::ofstream(name + ".started") << pid;
  if (name != "holderd.exe") return 0;
  wchar_t exit_code[32]{};
  if (GetEnvironmentVariableW(L"HOLDER_TEST_EXIT", exit_code, 32) > 0)
    return static_cast<int>(std::wcstol(exit_code, nullptr, 10));

  HANDLE lock = INVALID_HANDLE_VALUE;
  if (GetEnvironmentVariableW(L"HOLDER_TEST_RACE", nullptr, 0) > 0) {
    lock = CreateFileW(L"backend.lock", GENERIC_READ | GENERIC_WRITE, 0,
                      nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock == INVALID_HANDLE_VALUE) {
      std::ofstream("loser." + pid) << "2";
      return 2;
    }
    const auto start = GetTickCount64();
    for (;;) {
      int count = 0;
      for (const auto& entry : std::filesystem::directory_iterator(".")) {
        const auto file = entry.path().filename().string();
        if (file != "holderd.exe.started" && file.starts_with("holderd.exe.") && file.ends_with(".started")) ++count;
      }
      if (count >= 2 || GetTickCount64() - start >= 4000) break;
      Sleep(10);
    }
    // Stay cold through the loser's first poll and final health recheck.
    Sleep(2200);
    std::ofstream("winner.ready") << pid;
  }
  // Fixtures stop on the harness's signal, even after launcher timeout/handoff.
  const auto start = GetTickCount64();
  while (!std::filesystem::exists("stop-fixtures") && GetTickCount64() - start < 10000)
    Sleep(10);
  if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
  return 0;
}
