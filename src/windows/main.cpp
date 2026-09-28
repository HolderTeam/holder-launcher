#include <windows.h>

#include "BackendStartup.h"
#include "InstallLayout.h"
#include "Diagnostics.h"
#include "BuildIdentity.h"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#ifdef HOLDER_LAUNCHER_INTEGRATION_TEST
#include <cwchar>
#include <iostream>
#endif

namespace {

#ifdef HOLDER_LAUNCHER_INTEGRATION_TEST
std::uint16_t kBackendPort = 0;
#else
constexpr std::uint16_t kBackendPort = 11499;
#endif
#ifdef HOLDER_LAUNCHER_INTEGRATION_TEST
std::chrono::milliseconds kStartupBudget = std::chrono::seconds(60);
#else
constexpr auto kStartupBudget = std::chrono::seconds(60);
#endif

struct ProcessHandle {
  HANDLE value = nullptr;
  ProcessHandle() = default;
  ProcessHandle(const ProcessHandle&) = delete;
  ProcessHandle& operator=(const ProcessHandle&) = delete;
  ~ProcessHandle() { if (value) CloseHandle(value); }
};

std::wstring last_error_message(DWORD error_code) {
  wchar_t* buffer = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD length = FormatMessageW(
      flags,
      nullptr,
      error_code,
      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<wchar_t*>(&buffer),
      0,
      nullptr
  );

  if (length == 0 || buffer == nullptr) {
    return L"Windows error " + std::to_wstring(error_code);
  }

  std::wstring message(buffer, length);
  LocalFree(buffer);
  while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
    message.pop_back();
  }
  return L"Windows error " + std::to_wstring(error_code) + L": " + message;
}

std::filesystem::path executable_path() {
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size())
    );
    if (length == 0) {
      throw std::runtime_error("GetModuleFileNameW failed");
    }
    if (length < buffer.size() - 1) {
      buffer.resize(length);
      return std::filesystem::path(buffer);
    }
    buffer.resize(buffer.size() * 2);
  }
}

const auto launch_started = std::chrono::steady_clock::now();

std::wstring elapsed_ms() {
  return std::to_wstring(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - launch_started).count());
}

void append_log(std::wstring_view message) noexcept {
  try { holder::append_launcher_log(holder::launcher_log_path(), message); }
  catch (...) { }
}

void show_error(std::wstring_view message) {
  append_log(L"stage=error elapsed_ms=" + elapsed_ms() + L" " + std::wstring(message));
#ifdef HOLDER_LAUNCHER_INTEGRATION_TEST
  std::cerr << holder::utf8(message) << "\n";
#else
  const std::wstring text(message);
  MessageBoxW(nullptr, text.c_str(), L"Holder", MB_OK | MB_ICONERROR);
#endif
}

bool start_process(
    const std::filesystem::path& exe,
    const std::filesystem::path& working_dir,
    DWORD creation_flags,
    std::wstring* error,
    HANDLE* retained_process = nullptr
) {
  std::wstring command_line = L"\"" + exe.wstring() + L"\"";

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};

  const BOOL started = CreateProcessW(
      exe.c_str(),
      command_line.data(),
      nullptr,
      nullptr,
      FALSE,
      creation_flags,
      nullptr,
      working_dir.c_str(),
      &startup,
      &process
  );

  if (!started) {
    const DWORD code = GetLastError();
    if (error) {
      *error = L"Failed to start " + exe.wstring() + L": " +
               last_error_message(code);
    }
    return false;
  }

  CloseHandle(process.hThread);
  if (retained_process) *retained_process = process.hProcess;
  else CloseHandle(process.hProcess);
  return true;
}

int run_launcher() {
  const auto layout = holder::resolve_layout(executable_path());
  append_log(L"stage=starting source=" HOLDER_BUILD_SOURCE_W L" root=" + layout.root_dir.wstring() +
             L" backend=" + layout.backend_exe.wstring() + L" desktop=" + layout.desktop_exe.wstring());

  if (const auto error = holder::validate_layout(layout); !error.empty()) {
    show_error(error);
    return 1;
  }

  ProcessHandle backend;
  holder::StartupActions actions{
      [] { return holder::StartupClock::now(); },
      [](auto delay) { std::this_thread::sleep_for(delay); },
      [](auto allowance) { return holder::backend_ping(kBackendPort, allowance); },
      [&] {
        append_log(L"stage=backend_start Starting holderd.exe");
        std::wstring error;
        start_process(layout.backend_exe, layout.root_dir, CREATE_NO_WINDOW, &error, &backend.value);
        return error;
      },
      [&] {
        auto exit = holder::backend_exit_status(backend.value);
        if (!exit.message.empty()) append_log(exit.message);
        return exit;
      },
  };
  if (const auto error = holder::ensure_backend(actions, kStartupBudget); !error.empty()) {
    show_error(error + L"\n\nLauncher log:\n" + holder::launcher_log_path().wstring() +
               L"\n\nBackend log (if created with this environment):\n" +
               holder::backend_log_path(layout.root_dir).wstring());
    return 1;
  }

  append_log(L"stage=desktop_start elapsed_ms=" + elapsed_ms() + L" Starting holder-desktop.exe");
  std::wstring desktop_error;
  if (!start_process(layout.desktop_exe, layout.root_dir, 0, &desktop_error)) {
    show_error(desktop_error);
    return 1;
  }

  append_log(L"stage=complete elapsed_ms=" + elapsed_ms());
  return 0;
}

} // namespace

#ifdef HOLDER_LAUNCHER_INTEGRATION_TEST
int wmain(int argc, wchar_t** argv) {
  if (argc != 2 && argc != 3) return 2;
  const auto port = std::wcstoul(argv[1], nullptr, 10);
  if (port == 0 || port > 65535) return 2;
  kBackendPort = static_cast<std::uint16_t>(port);
  if (argc == 3) {
    const auto milliseconds = std::wcstoul(argv[2], nullptr, 10);
    if (milliseconds == 0 || milliseconds > 60000) return 2;
    kStartupBudget = std::chrono::milliseconds(milliseconds);
  }
#else
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
#endif
  try {
    return run_launcher();
  } catch (const std::exception& e) {
    const std::string narrow = e.what();
    std::wstring wide(narrow.begin(), narrow.end());
    show_error(L"Holder launcher failed:\n\n" + wide);
    return 1;
  } catch (...) {
    show_error(L"Holder launcher failed with an unknown error.");
    return 1;
  }
}
