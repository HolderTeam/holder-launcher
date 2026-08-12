#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

extern char** environ;

namespace {

constexpr const char* kBackendHost = "127.0.0.1";
constexpr uint16_t kBackendPort = 11499;
constexpr int kHealthAttempts = 32;
constexpr auto kHealthDelay = std::chrono::milliseconds(250);

struct InstallLayout {
  std::filesystem::path root_dir;
  std::filesystem::path backend_exe;
  std::filesystem::path desktop_exe;
};

std::filesystem::path executable_path() {
  std::vector<char> buffer(4096);
  uint32_t size = static_cast<uint32_t>(buffer.size());
  while (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    buffer.resize(size + 1);
  }
  return std::filesystem::weakly_canonical(buffer.data());
}

std::filesystem::path home_dir() {
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    return std::filesystem::path(home);
  }
  return std::filesystem::temp_directory_path();
}

std::filesystem::path log_path() {
  return home_dir() / "Library" / "Logs" / "Holder" / "launcher.log";
}

void append_log(std::string_view message) {
  try {
    const auto path = log_path();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::app);
    out << message << "\n";
  } catch (...) {
  }
}

std::string shell_quote(std::string_view value) {
  std::string quoted = "'";
  for (const char ch : value) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted += ch;
    }
  }
  quoted += "'";
  return quoted;
}

std::string apple_script_quote(std::string_view value) {
  std::string quoted = "\"";
  for (const char ch : value) {
    if (ch == '\\' || ch == '"') {
      quoted += '\\';
    }
    quoted += ch;
  }
  quoted += "\"";
  return quoted;
}

void show_error(std::string_view message) {
  append_log(message);
  const std::string script =
      "display alert \"Holder\" message " + apple_script_quote(message);
  const std::string command = "osascript -e " + shell_quote(script);
  (void)std::system(command.c_str());
}

bool file_exists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec);
}

InstallLayout resolve_layout() {
  const auto self = executable_path();
  const auto self_dir = self.parent_path();

  InstallLayout app_layout{
      self_dir.parent_path() / "Resources",
      self_dir.parent_path() / "Resources" / "bin" / "holderd",
      self_dir.parent_path() / "Resources" / "bin" / "holder-desktop",
  };
  if (file_exists(app_layout.backend_exe) && file_exists(app_layout.desktop_exe)) {
    return app_layout;
  }

  InstallLayout side_by_side_layout{
      self_dir.parent_path(),
      self_dir / "holderd",
      self_dir / "holder-desktop",
  };
  return side_by_side_layout;
}

bool backend_ping() {
  const int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    return false;
  }

  timeval timeout{};
  timeout.tv_sec = 1;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kBackendPort);
  if (inet_pton(AF_INET, kBackendHost, &addr.sin_addr) != 1) {
    close(sock);
    return false;
  }

  if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(sock);
    return false;
  }

  constexpr std::string_view request =
      "GET /ping HTTP/1.1\r\n"
      "Host: 127.0.0.1:11499\r\n"
      "Connection: close\r\n"
      "\r\n";

  if (send(sock, request.data(), request.size(), 0) < 0) {
    close(sock);
    return false;
  }

  char buffer[128] = {};
  const ssize_t received = recv(sock, buffer, sizeof(buffer) - 1, 0);
  close(sock);

  if (received <= 0) {
    return false;
  }

  return std::string_view(buffer, static_cast<size_t>(received)).find("HTTP/1.1 200") !=
         std::string_view::npos;
}

bool start_process(
    const std::filesystem::path& exe,
    const std::filesystem::path& working_dir,
    std::string* error
) {
  setenv("GSETTINGS_SCHEMA_DIR", (working_dir / "share" / "glib-2.0" / "schemas").c_str(), 1);
  setenv("GIO_MODULE_DIR", (working_dir / "lib" / "gio" / "modules").c_str(), 1);
  setenv(
      "GDK_PIXBUF_MODULE_FILE",
      (working_dir / "lib" / "gdk-pixbuf-2.0" / "2.10.0" / "loaders.cache").c_str(),
      1
  );
  setenv("GTK_PATH", (working_dir / "lib" / "gtk-4.0").c_str(), 1);
  setenv("XDG_DATA_DIRS", (working_dir / "share").c_str(), 1);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addchdir_np(&actions, working_dir.c_str());

  pid_t pid = 0;
  std::string executable = exe.string();
  char* argv[] = {executable.data(), nullptr};

  const int rc = posix_spawn(
      &pid,
      executable.c_str(),
      &actions,
      nullptr,
      argv,
      environ
  );
  posix_spawn_file_actions_destroy(&actions);

  if (rc != 0) {
    if (error) {
      *error = "Failed to start " + executable + ": " + std::strerror(rc);
    }
    return false;
  }

  return true;
}

bool wait_for_backend_health() {
  for (int attempt = 0; attempt < kHealthAttempts; ++attempt) {
    if (backend_ping()) {
      return true;
    }
    std::this_thread::sleep_for(kHealthDelay);
  }
  return false;
}

int run_launcher() {
  const auto layout = resolve_layout();
  append_log("Holder launcher starting");

  if (!file_exists(layout.backend_exe)) {
    show_error("Holder backend was not found:\n\n" + layout.backend_exe.string());
    return 1;
  }
  if (!file_exists(layout.desktop_exe)) {
    show_error("Holder desktop app was not found:\n\n" + layout.desktop_exe.string());
    return 1;
  }

  if (!backend_ping()) {
    append_log("Backend is not healthy; starting holderd");
    std::string backend_error;
    if (!start_process(layout.backend_exe, layout.root_dir, &backend_error)) {
      show_error(backend_error);
      return 1;
    }

    if (!wait_for_backend_health()) {
      show_error(
          "Holder backend did not become ready.\n\n"
          "Check:\n\n" +
          log_path().string()
      );
      return 1;
    }
  }

  append_log("Starting holder-desktop");
  std::string desktop_error;
  if (!start_process(layout.desktop_exe, layout.root_dir, &desktop_error)) {
    show_error(desktop_error);
    return 1;
  }

  append_log("Holder launcher complete");
  return 0;
}

} // namespace

int main() {
  try {
    return run_launcher();
  } catch (const std::exception& e) {
    show_error(std::string("Holder launcher failed:\n\n") + e.what());
    return 1;
  } catch (...) {
    show_error("Holder launcher failed with an unknown error.");
    return 1;
  }
}
