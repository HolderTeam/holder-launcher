#include "Diagnostics.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::string read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void write(const fs::path& path, const std::string& data) {
  std::ofstream(path, std::ios::binary) << data;
}
struct Workspace {
  fs::path original = fs::current_path();
  fs::path root = fs::temp_directory_path() / (L"holder-diagnostics-" + std::to_wstring(GetCurrentProcessId()));
  Workspace() { check(fs::create_directory(root), "new temporary workspace"); }
  ~Workspace() {
    std::error_code error;
    fs::current_path(original, error);
    fs::remove_all(root, error);
  }
};
struct Environment {
  std::vector<std::pair<std::wstring, std::optional<std::wstring>>> saved;
  Environment() {
    for (const auto name : {L"LOCALAPPDATA", L"TEMP", L"HOME", L"USERPROFILE", L"XDG_DATA_HOME"}) {
      const auto size = GetEnvironmentVariableW(name, nullptr, 0);
      if (size) {
        std::wstring value(size, L'\0');
        value.resize(GetEnvironmentVariableW(name, value.data(), size));
        saved.emplace_back(name, value);
      } else saved.emplace_back(name, std::nullopt);
    }
  }
  ~Environment() { for (const auto& [name, value] : saved) SetEnvironmentVariableW(name.c_str(), value ? value->c_str() : nullptr); }
};
void set(const wchar_t* name, const wchar_t* value) { check(SetEnvironmentVariableW(name, value) != 0, "test environment"); }
struct Handle {
  HANDLE value;
  ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
}

int main() {
  try {
    Workspace workspace;
    const auto& root = workspace.root;
    const auto unicode = root / L"\u96ea's logs" / L"launcher.log";
    holder::append_launcher_log(unicode, L"stage=test \u96ea \U0001F680\nsecond\r\tend");
    const auto first = read(unicode);
    check(first.find("\xE9\x9B\xAA \xF0\x9F\x9A\x80\\nsecond\\r\\tend") != std::string::npos, "UTF-8 and escaped newlines");
    check(std::count(first.begin(), first.end(), '\n') == 1, "one record per line");
    check(std::regex_search(first, std::regex(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z \[pid=\d+\] \[launcher=\d+\.\d+\.\d+\])")), "timestamp PID and version");

    const auto capped = root / L"capped.log";
    holder::append_launcher_log(capped, std::wstring(20000, L'\u96ea'));
    const auto capped_text = read(capped);
    check(capped_text.size() < 8400 && capped_text.find("[truncated]") != std::string::npos, "bounded message");
    check(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, capped_text.data(), static_cast<int>(capped_text.size()), nullptr, 0) > 0, "valid UTF-8 after truncation");

    const auto rotating = root / L"rotate.log";
    write(rotating, std::string(256 * 1024 - 1, 'a'));
    holder::append_launcher_log(rotating, L"first rotation");
    check(read(root / L"rotate.log.1") == std::string(256 * 1024 - 1, 'a'), "rotation preserves previous file");
    write(rotating, std::string(256 * 1024, 'b'));
    holder::append_launcher_log(rotating, L"second rotation");
    check(read(root / L"rotate.log.1") == std::string(256 * 1024, 'b'), "replace old backup");
    check(read(rotating).find("second rotation") != std::string::npos && !fs::exists(root / L"rotate.log.2"), "single backup");

    const auto failed_rotation = root / L"failed.log";
    write(failed_rotation, std::string(256 * 1024, 'c'));
    fs::create_directory(root / L"failed.log.1");
    holder::append_launcher_log(failed_rotation, L"cannot rotate");
    check(fs::file_size(failed_rotation) == 256 * 1024, "failed rotation does not grow log");

    write(root / L"blocked", "file instead of directory");
    holder::append_launcher_log(root / L"blocked" / L"launcher.log", L"cannot create directory");
    check(read(root / L"blocked") == "file instead of directory", "blocked directory remains intact");
    const auto locked = root / L"locked.log";
    {
      Handle lock{CreateFileW((root / L"locked.log.lock").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
      check(lock.value != INVALID_HANDLE_VALUE, "hold log lock");
      const auto start = std::chrono::steady_clock::now();
      holder::append_launcher_log(locked, L"contention");
      check(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(250), "lock contention never waits");
      check(!fs::exists(locked), "contended write skipped");
    }
    holder::append_launcher_log(locked, L"lock released");
    check(read(locked).find("lock released") != std::string::npos, "recovery after lock release");

    const auto concurrent = root / L"concurrent.log";
    std::vector<std::thread> writers;
    for (int i = 0; i < 4; ++i) writers.emplace_back([&] {
      for (int j = 0; j < 50; ++j) holder::append_launcher_log(concurrent, L"complete-record \u96ea");
    });
    for (auto& writer : writers) writer.join();
    std::ifstream lines(concurrent, std::ios::binary);
    std::string line;
    int count = 0;
    while (std::getline(lines, line)) {
      check(line.ends_with("complete-record \xE9\x9B\xAA"), "concurrent record remains whole");
      ++count;
    }
    check(count > 0, "some concurrent writes succeed");

    Environment environment;
    set(L"LOCALAPPDATA", L"C:\\local"); set(L"TEMP", L"C:\\temp");
    check(holder::launcher_log_path() == fs::path(L"C:\\local\\holder\\launcher.log"), "LOCALAPPDATA log");
    set(L"LOCALAPPDATA", nullptr);
    check(holder::launcher_log_path() == fs::path(L"C:\\temp\\holder\\launcher.log"), "TEMP fallback");
    set(L"TEMP", nullptr); fs::current_path(root);
    check(holder::launcher_log_path() == fs::path(L"holder-launcher.log"), "relative log fallback");
    holder::append_launcher_log(holder::launcher_log_path(), L"fallback");
    check(fs::exists(root / L"holder-launcher.log"), "relative filename can be logged");

    const auto suffix = fs::path(L"holder/server/logs/server.log");
    set(L"USERPROFILE", L"C:\\profile"); set(L"HOME", nullptr); set(L"XDG_DATA_HOME", nullptr);
    check(holder::backend_log_path(root) == fs::path(L"C:/profile/.local/share") / suffix, "USERPROFILE backend log");
    set(L"HOME", L"C:\\home");
    check(holder::backend_log_path(root) == fs::path(L"C:/home/.local/share") / suffix, "HOME overrides profile");
    set(L"XDG_DATA_HOME", L"C:\\data");
    check(holder::backend_log_path(root) == fs::path(L"C:/data") / suffix, "absolute XDG override");
    set(L"XDG_DATA_HOME", L"relative");
    check(holder::backend_log_path(root) == fs::path(L"C:/home/.local/share") / suffix, "relative XDG ignored");
    set(L"HOME", L"relative-home");
    check(holder::backend_log_path(root) == root / L"relative-home/.local/share" / suffix, "relative HOME resolved against child cwd");
    set(L"HOME", nullptr); set(L"USERPROFILE", nullptr);
    check(holder::backend_log_path(root) == root / L".local/share" / suffix, "missing home fallback");
    std::cout << "Diagnostics: Unicode, bounds, rotation, failures, contention, concurrent writes and path policy passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
