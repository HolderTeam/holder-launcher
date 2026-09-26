#include "InstallLayout.h"

namespace holder {
InstallLayout resolve_layout(const std::filesystem::path& self) {
  const auto directory = self.parent_path();
  const auto bin = directory / L"bin";
  std::error_code error;
  const auto status = std::filesystem::symlink_status(bin, error);
  // Retain package paths for inaccessible, invalid or dangling bin entries too.
  const bool child_bin = status.type() != std::filesystem::file_type::not_found;
  const auto name = directory.filename().wstring();
  const bool developer = name.size() == 3 && (name[0] == L'b' || name[0] == L'B') &&
      (name[1] == L'i' || name[1] == L'I') && (name[2] == L'n' || name[2] == L'N');
  if (!child_bin && developer)
    return {directory.parent_path(), directory / L"holderd.exe", directory / L"holder-desktop.exe"};
  return {directory, bin / L"holderd.exe", bin / L"holder-desktop.exe"};
}

std::wstring validate_layout(const InstallLayout& layout) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(layout.backend_exe, error))
    return L"Holder backend was not found:\n\n" + layout.backend_exe.wstring();
  error.clear();
  if (!std::filesystem::is_regular_file(layout.desktop_exe, error))
    return L"Holder desktop app was not found:\n\n" + layout.desktop_exe.wstring();
  return {};
}
}
