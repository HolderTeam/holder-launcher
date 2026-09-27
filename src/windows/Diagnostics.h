#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace holder {
std::string utf8(std::wstring_view text);
std::filesystem::path launcher_log_path();
// Diagnostic hint for a daemon launched with the inherited environment/cwd.
std::filesystem::path backend_log_path(const std::filesystem::path& working_dir);
void append_launcher_log(const std::filesystem::path& path, std::wstring_view message) noexcept;
}
