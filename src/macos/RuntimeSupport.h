#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <cstdlib>

namespace holder {
std::string configure_desktop_environment(const std::filesystem::path& root,
    int (*set_variable)(const char*, const char*, int) = ::setenv);
void append_launcher_log(const std::filesystem::path& path, std::string_view message) noexcept;
std::string apple_script_quote(std::string_view value);
// Invoke an absolute helper directly, without PATH lookup or a shell.
bool present_alert(std::string_view message, const char* helper = "/usr/bin/osascript");
}
