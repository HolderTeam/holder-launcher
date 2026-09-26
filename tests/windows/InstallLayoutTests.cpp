#include "InstallLayout.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void touch(const fs::path& path) { std::ofstream(path) << "fixture"; }
struct Workspace {
  fs::path original = fs::current_path();
  fs::path root = fs::temp_directory_path() / (L"holder-layout-" + std::to_wstring(GetCurrentProcessId()));
  Workspace() { check(fs::create_directory(root), "temporary directory must be new"); }
  ~Workspace() {
    std::error_code error;
    fs::current_path(original, error);
    fs::remove_all(root, error);
  }
};
void verify(const holder::InstallLayout& layout, const fs::path& root, const fs::path& bin, int mask) {
  check(layout.root_dir == root, "runtime root");
  check(layout.backend_exe == bin / L"holderd.exe", "backend path");
  check(layout.desktop_exe == bin / L"holder-desktop.exe", "desktop path");
  const auto expected = !(mask & 1) ? L"Holder backend was not found:\n\n" + layout.backend_exe.wstring() :
      !(mask & 2) ? L"Holder desktop app was not found:\n\n" + layout.desktop_exe.wstring() : std::wstring{};
  check(holder::validate_layout(layout) == expected, "exact missing-component diagnostic");
}
void populate(const fs::path& bin, int mask) {
  fs::create_directories(bin);
  if (mask & 1) touch(bin / L"holderd.exe");
  if (mask & 2) touch(bin / L"holder-desktop.exe");
}
}
int main() {
  try {
    Workspace workspace;
    const auto& root = workspace.root;
    for (int mask = 0; mask < 4; ++mask) {
      const auto package = root / (L"package" + std::to_wstring(mask));
      populate(package / L"bin", mask);
      touch(package / L"holderd.exe");
      touch(package / L"holder-desktop.exe");
      verify(holder::resolve_layout(package / L"Holder.exe"), package, package / L"bin", mask);
    }
    for (int mask = 0; mask < 4; ++mask) {
      const auto developer = root / (L"developer" + std::to_wstring(mask));
      populate(developer / L"BiN", mask);
      verify(holder::resolve_layout(developer / L"BiN" / L"Holder.exe"), developer, developer / L"BiN", mask);
    }
    const auto missing = root / L"missing-bin";
    populate(missing, 3); // Adjacent decoys must not hide a missing package bin.
    verify(holder::resolve_layout(missing / L"Holder.exe"), missing, missing / L"bin", 0);
    touch(missing / L"bin"); // A file cannot turn this into a developer layout.
    verify(holder::resolve_layout(missing / L"Holder.exe"), missing, missing / L"bin", 0);

    const auto competing = root / L"competing" / L"bin";
    populate(competing, 3);
    populate(competing / L"bin", 1);
    verify(holder::resolve_layout(competing / L"Holder.exe"), competing, competing / L"bin", 1);

    const auto directories = root / L"directories";
    fs::create_directories(directories / L"bin" / L"holderd.exe");
    fs::create_directories(directories / L"bin" / L"holder-desktop.exe");
    verify(holder::resolve_layout(directories / L"Holder.exe"), directories, directories / L"bin", 0);
    fs::remove(directories / L"bin" / L"holderd.exe");
    touch(directories / L"bin" / L"holderd.exe");
    verify(holder::resolve_layout(directories / L"Holder.exe"), directories, directories / L"bin", 1);

    const auto unicode = root / L"Holder \u96ea's space";
    populate(unicode / L"bin", 3);
    fs::current_path(directories); // Selection must not depend on the launch cwd.
    verify(holder::resolve_layout(unicode / L"Renamed.exe"), unicode, unicode / L"bin", 3);
    std::cout << "6 layout groups passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
