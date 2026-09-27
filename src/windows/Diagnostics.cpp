#include "Diagnostics.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <limits>
#include <optional>

namespace holder {
namespace {
constexpr std::size_t kMaxMessage = 8192;
constexpr std::uintmax_t kMaxLog = 256 * 1024;
struct Handle {
  HANDLE value;
  ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

std::optional<std::filesystem::path> env_path(const wchar_t* name) {
  const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
  if (size == 0) return {};
  std::wstring value(size, L'\0');
  const DWORD written = GetEnvironmentVariableW(name, value.data(), size);
  if (written == 0 || written >= size) return {};
  value.resize(written);
  return std::filesystem::path(value);
}

std::string bounded_message(std::wstring_view message) {
  bool truncated = message.size() > kMaxMessage;
  auto length = std::min(message.size(), kMaxMessage);
  // Never cut a UTF-16 surrogate pair at the input boundary.
  if (length < message.size() && length && message[length - 1] >= 0xD800 && message[length - 1] <= 0xDBFF)
    --length;
  const auto encoded = utf8(message.substr(0, length));
  std::string escaped;
  for (const unsigned char byte : encoded) {
    if (byte == '\n') escaped += "\\n";
    else if (byte == '\r') escaped += "\\r";
    else if (byte == '\t') escaped += "\\t";
    else if (byte < 32 || byte == 127) escaped += '?';
    else escaped += static_cast<char>(byte);
  }
  if (escaped.size() > kMaxMessage) {
    auto end = kMaxMessage;
    while ((static_cast<unsigned char>(escaped[end]) & 0xC0) == 0x80) --end;
    escaped.resize(end);
    truncated = true;
  }
  if (truncated) escaped += " [truncated]";
  return escaped;
}
}

std::string utf8(std::wstring_view text) {
  if (text.empty()) return {};
  if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return "[text too large]";
  const int length = static_cast<int>(text.size());
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
  if (size == 0) return "[text conversion failed]";
  std::string result(size, '\0');
  if (!WideCharToMultiByte(CP_UTF8, 0, text.data(), length, result.data(), size, nullptr, nullptr))
    return "[text conversion failed]";
  return result;
}

std::filesystem::path launcher_log_path() {
  auto base = env_path(L"LOCALAPPDATA");
  if (!base) base = env_path(L"TEMP");
  return base ? *base / L"holder" / L"launcher.log" : std::filesystem::path(L"holder-launcher.log");
}

std::filesystem::path backend_log_path(const std::filesystem::path& working_dir) {
  // Mirrors holder-daemon platform/BaseDir.cpp and Paths.h for support guidance
  // only; it does not select or modify the daemon's data location.
  auto data = env_path(L"XDG_DATA_HOME");
  if (!data || !data->is_absolute()) {
    auto home = env_path(L"HOME");
    if (!home) home = env_path(L"USERPROFILE");
    data = home.value_or(std::filesystem::path(L".")) / L".local" / L"share";
  }
  return (working_dir / *data / L"holder" / L"server" / L"logs" / L"server.log").lexically_normal();
}

void append_launcher_log(const std::filesystem::path& path, std::wstring_view message) noexcept {
  try {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    auto lock_path = path;
    lock_path += L".lock";
    // A share-denied open fails immediately if another launcher owns this lock.
    Handle lock{CreateFileW(lock_path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (lock.value == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME now{};
    GetSystemTime(&now);
    char prefix[160]{};
    std::snprintf(prefix, sizeof(prefix), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ [pid=%lu] [launcher=%s] ",
        static_cast<unsigned>(now.wYear), static_cast<unsigned>(now.wMonth), static_cast<unsigned>(now.wDay),
        static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond),
        static_cast<unsigned>(now.wMilliseconds), GetCurrentProcessId(), HOLDER_LAUNCHER_VERSION);
    const std::string record = std::string(prefix) + bounded_message(message) + '\n';
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (!error && size > kMaxLog - record.size()) {
      auto backup = path;
      backup += L".1";
      if (!MoveFileExW(path.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
    }
    Handle file{CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file.value, record.data(), static_cast<DWORD>(record.size()), &written, nullptr);
  } catch (...) {
    // Logging is best-effort, including allocation, filesystem and rotation failures.
  }
}
}
