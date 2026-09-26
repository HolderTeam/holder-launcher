#include "InstallLayout.h"
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct TempDirectory {
  fs::path path;
  TempDirectory() {
    auto pattern = (fs::temp_directory_path() / "holder-layout-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const char* created = mkdtemp(buffer.data());
    require(created != nullptr, "create temporary directory");
    path = fs::canonical(created);
  }
  ~TempDirectory() { std::error_code ec; fs::remove_all(path, ec); }
};

void touch(const fs::path& path) {
  fs::create_directories(path.parent_path());
  std::ofstream file(path);
  require(file.good(), "create fixture");
}

void check_paths(const holder::InstallLayout& layout, const fs::path& root, const fs::path& bin) {
  require(layout.root_dir == root, "incorrect working directory");
  require(layout.backend_exe == bin / "holderd", "incorrect backend path");
  require(layout.desktop_exe == bin / "holder-desktop", "incorrect desktop path");
}
} // namespace

int main() {
  int failed = 0;
  auto test = [&](const char* name, auto fn) {
    try { fn(); std::cout << "PASS " << name << '\n'; }
    catch (const std::exception& error) {
      ++failed;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  };
  test("bundle stays selected for every combination of missing components", [] {
    TempDirectory temp;
    for (int present = 0; present < 4; ++present) {
      const auto contents = temp.path / std::to_string(present) / "Holder.app" / "Contents";
      const auto self = contents / "MacOS" / "Holder";
      const auto resources = contents / "Resources";
      touch(self);
      // Complete adjacent developer binaries must never mask an incomplete bundle.
      touch(self.parent_path() / "holderd");
      touch(self.parent_path() / "holder-desktop");
      if (present & 1) touch(resources / "bin" / "holderd");
      if (present & 2) touch(resources / "bin" / "holder-desktop");
      const auto layout = holder::resolve_layout(self);
      check_paths(layout, resources, resources / "bin");
      const auto error = holder::validate_layout(layout);
      if (!(present & 1))
        require(error == "Holder backend was not found:\n\n" + (resources / "bin" / "holderd").string(), "wrong backend diagnostic");
      else if (!(present & 2))
        require(error == "Holder desktop app was not found:\n\n" + (resources / "bin" / "holder-desktop").string(), "wrong desktop diagnostic");
      else require(error.empty(), "complete bundle rejected");
    }
  });
  test("developer layout ignores nearby Resources", [] {
    TempDirectory temp;
    const auto bin = temp.path / "bin";
    touch(bin / "Holder");
    touch(bin / "holderd");
    touch(bin / "holder-desktop");
    touch(temp.path / "Resources" / "bin" / "holderd");
    touch(temp.path / "Resources" / "bin" / "holder-desktop");
    const auto layout = holder::resolve_layout(bin / "Holder");
    check_paths(layout, temp.path, bin);
    require(holder::validate_layout(layout).empty(), "complete developer layout rejected");
    fs::remove(bin / "holder-desktop");
    require(holder::validate_layout(layout) == "Holder desktop app was not found:\n\n" + (bin / "holder-desktop").string(), "developer missing file masked");
  });
  test("spaces Unicode apostrophes and renamed app", [] {
    TempDirectory temp;
    for (const auto& relative : {fs::path("Zoë's Apps/持有 Preview.app/Contents/MacOS/Holder"), fs::path("Zoë's 开发/bin/Holder")}) {
      const auto self = temp.path / relative;
      const auto layout = holder::resolve_layout(self);
      const bool bundle = self.parent_path().filename() == "MacOS";
      const auto root = bundle ? self.parent_path().parent_path() / "Resources" : self.parent_path().parent_path();
      check_paths(layout, root, bundle ? root / "bin" : self.parent_path());
      touch(layout.backend_exe);
      touch(layout.desktop_exe);
      require(holder::validate_layout(layout).empty(), "special path rejected");
    }
  });
  test("directories are not executables", [] {
    TempDirectory temp;
    const auto layout = holder::resolve_layout(temp.path / "bin" / "Holder");
    fs::create_directories(layout.backend_exe);
    touch(layout.desktop_exe);
    require(holder::validate_layout(layout) == "Holder backend was not found:\n\n" + layout.backend_exe.string(), "directory accepted as backend");
  });
  test("bundle shape required", [] {
    for (const auto& self : {fs::path("/tmp/dev/Contents/MacOS/Holder"), fs::path("/tmp/Holder.app/bin/Holder"), fs::path("/tmp/Holder.app/Other/MacOS/Holder")}) {
      check_paths(holder::resolve_layout(self), self.parent_path().parent_path(), self.parent_path());
    }
  });
  test("selection independent of current directory", [] {
    TempDirectory temp;
    struct RestoreCwd {
      fs::path original = fs::current_path();
      ~RestoreCwd() { std::error_code ec; fs::current_path(original, ec); }
    } restore;
    fs::current_path(temp.path);
    const fs::path self = "/Applications/Holder.app/Contents/MacOS/Holder";
    check_paths(holder::resolve_layout(self), "/Applications/Holder.app/Contents/Resources", "/Applications/Holder.app/Contents/Resources/bin");
  });
  return failed == 0 ? 0 : 1;
}
