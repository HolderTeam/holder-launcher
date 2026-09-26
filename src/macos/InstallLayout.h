#pragma once

#include <filesystem>
#include <string>

namespace holder {
struct InstallLayout {
  std::filesystem::path root_dir;
  std::filesystem::path backend_exe;
  std::filesystem::path desktop_exe;
};

// self is the absolute, resolved executable path, independent of the launch cwd.
InstallLayout resolve_layout(const std::filesystem::path& self);
// Empty on success; otherwise the diagnostic for the first missing component.
std::string validate_layout(const InstallLayout& layout);
} // namespace holder
