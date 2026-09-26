#pragma once
#include <filesystem>
#include <string>

namespace holder {
struct InstallLayout {
  std::filesystem::path root_dir;
  std::filesystem::path backend_exe;
  std::filesystem::path desktop_exe;
};
// self is the absolute launcher path, independent of the working directory.
// A child bin entry selects a package. Otherwise, a launcher inside a directory
// named bin (case-insensitive) selects the documented developer layout.
// All other locations select a package, even if its bin directory is missing.
InstallLayout resolve_layout(const std::filesystem::path& self);
std::wstring validate_layout(const InstallLayout& layout);
}
