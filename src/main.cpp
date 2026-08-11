#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr wchar_t kBackendHost[] = L"127.0.0.1";
constexpr INTERNET_PORT kBackendPort = 11499;
constexpr int kHealthAttempts = 32;
constexpr auto kHealthDelay = std::chrono::milliseconds(250);

struct InstallLayout {
  std::filesystem::path root_dir;
  std::filesystem::path backend_exe;
  std::filesystem::path desktop_exe;
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
  return message;
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

std::optional<std::filesystem::path> env_path(const wchar_t* name) {
  const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
  if (required == 0) {
    return std::nullopt;
  }
  std::wstring value(required, L'\0');
  const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
  if (written == 0 || written >= required) {
    return std::nullopt;
  }
  value.resize(written);
  return std::filesystem::path(value);
}

std::filesystem::path log_path() {
  auto local_app_data = env_path(L"LOCALAPPDATA");
  if (!local_app_data) {
    local_app_data = env_path(L"TEMP");
  }
  if (!local_app_data) {
    return std::filesystem::path(L"holder-launcher.log");
  }
  return *local_app_data / L"holder" / L"launcher.log";
}

void append_log(std::wstring_view message) {
  try {
    const auto path = log_path();
    std::filesystem::create_directories(path.parent_path());
    std::wofstream out(path, std::ios::app);
    out << message << L"\n";
  } catch (...) {
  }
}

void show_error(std::wstring_view message) {
  append_log(message);
  const std::wstring text(message);
  MessageBoxW(nullptr, text.c_str(), L"Holder", MB_OK | MB_ICONERROR);
}

bool file_exists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec);
}

InstallLayout resolve_layout() {
  const auto self = executable_path();
  const auto self_dir = self.parent_path();

  InstallLayout root_layout{
      self_dir,
      self_dir / L"bin" / L"holderd.exe",
      self_dir / L"bin" / L"holder-desktop.exe",
  };
  if (file_exists(root_layout.backend_exe) && file_exists(root_layout.desktop_exe)) {
    return root_layout;
  }

  InstallLayout side_by_side_layout{
      self_dir.parent_path(),
      self_dir / L"holderd.exe",
      self_dir / L"holder-desktop.exe",
  };
  return side_by_side_layout;
}

bool backend_health_check() {
  HINTERNET session = WinHttpOpen(
      L"Holder Launcher/1.0",
      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS,
      0
  );
  if (!session) {
    return false;
  }

  HINTERNET connect = WinHttpConnect(session, kBackendHost, kBackendPort, 0);
  if (!connect) {
    WinHttpCloseHandle(session);
    return false;
  }

  HINTERNET request = WinHttpOpenRequest(
      connect,
      L"GET",
      L"/health",
      nullptr,
      WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES,
      0
  );
  if (!request) {
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return false;
  }

  DWORD timeout_ms = 1000;
  WinHttpSetTimeouts(request, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

  bool ok = false;
  if (WinHttpSendRequest(
          request,
          WINHTTP_NO_ADDITIONAL_HEADERS,
          0,
          WINHTTP_NO_REQUEST_DATA,
          0,
          0,
          0
      ) &&
      WinHttpReceiveResponse(request, nullptr)) {
    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    ok = WinHttpQueryHeaders(
             request,
             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
             WINHTTP_HEADER_NAME_BY_INDEX,
             &status_code,
             &status_size,
             WINHTTP_NO_HEADER_INDEX
         ) &&
         status_code == 200;
  }

  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return ok;
}

bool start_process(
    const std::filesystem::path& exe,
    const std::filesystem::path& working_dir,
    DWORD creation_flags,
    std::wstring* error
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
    if (error) {
      *error = L"Failed to start " + exe.wstring() + L": " +
               last_error_message(GetLastError());
    }
    return false;
  }

  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

bool wait_for_backend_health() {
  for (int attempt = 0; attempt < kHealthAttempts; ++attempt) {
    if (backend_health_check()) {
      return true;
    }
    std::this_thread::sleep_for(kHealthDelay);
  }
  return false;
}

int run_launcher() {
  const auto layout = resolve_layout();
  append_log(L"Holder launcher starting");

  if (!file_exists(layout.backend_exe)) {
    show_error(L"Holder backend was not found:\n\n" + layout.backend_exe.wstring());
    return 1;
  }
  if (!file_exists(layout.desktop_exe)) {
    show_error(L"Holder desktop app was not found:\n\n" + layout.desktop_exe.wstring());
    return 1;
  }

  if (!backend_health_check()) {
    append_log(L"Backend is not healthy; starting holderd.exe");
    std::wstring backend_error;
    if (!start_process(layout.backend_exe, layout.root_dir, CREATE_NO_WINDOW, &backend_error)) {
      show_error(backend_error);
      return 1;
    }

    if (!wait_for_backend_health()) {
      show_error(
          L"Holder backend did not become ready.\n\n"
          L"Try starting Holder Backend from the Start Menu, or check:\n\n" +
          log_path().wstring()
      );
      return 1;
    }
  }

  append_log(L"Starting holder-desktop.exe");
  std::wstring desktop_error;
  if (!start_process(layout.desktop_exe, layout.root_dir, 0, &desktop_error)) {
    show_error(desktop_error);
    return 1;
  }

  append_log(L"Holder launcher complete");
  return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
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
