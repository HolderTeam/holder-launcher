#include "InstallLayout.h"

namespace holder {
InstallLayout resolve_layout(const std::filesystem::path& self) {
  const auto bin = self.parent_path();
  const auto contents = bin.parent_path();
  if (bin.filename() == "MacOS" && contents.filename() == "Contents" &&
      contents.parent_path().extension() == ".app") {
    const auto resources = contents / "Resources";
    return {resources, resources / "bin" / "holderd", resources / "bin" / "holder-desktop"};
  }
  return {bin.parent_path(), bin / "holderd", bin / "holder-desktop"};
}

std::string validate_layout(const InstallLayout& layout) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(layout.backend_exe, ec))
    return "Holder backend was not found:\n\n" + layout.backend_exe.string();
  ec.clear();
  if (!std::filesystem::is_regular_file(layout.desktop_exe, ec))
    return "Holder desktop app was not found:\n\n" + layout.desktop_exe.string();
  return {};
}
} // namespace holder
