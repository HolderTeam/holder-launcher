#include "RuntimeSupport.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char** environ;
namespace holder {
std::string configure_desktop_environment(const std::filesystem::path& root,
    int (*set_variable)(const char*, const char*, int)) {
  const std::pair<const char*, const char*> variables[] = {
    {"GSETTINGS_SCHEMA_DIR", "share/glib-2.0/schemas"},
    {"GIO_MODULE_DIR", "lib/gio/modules"},
    {"GDK_PIXBUF_MODULE_FILE", "lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"},
    {"GTK_PATH", "lib/gtk-4.0"}, {"XDG_DATA_DIRS", "share"},
    {"ENCHANT_CONFIG_DIR", "share/enchant-2"}, {"DICPATH", "share/enchant/hunspell"},
  };
  for (const auto& [name, relative] : variables) {
    if (set_variable(name, (root / relative).c_str(), 1) != 0)
      return "Failed to configure desktop environment (" + std::string(name) + "): " + std::strerror(errno);
  }
  return {};
}

void append_launcher_log(const std::filesystem::path& path, std::string_view message) noexcept {
  try {
    std::filesystem::create_directories(path.parent_path());
    struct Lock {
      int fd;
      ~Lock() { if (fd >= 0) close(fd); }
    } lock{open((path.string() + ".lock").c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600)};
    // Logging must never hold up startup, including simultaneous launches.
    if (lock.fd < 0 || flock(lock.fd, LOCK_EX | LOCK_NB) != 0) return;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    constexpr size_t max_message = 8192;
    if (!ec && size + std::min(message.size(), max_message) + 128 > 256 * 1024) {
      std::filesystem::rename(path, path.string() + ".1", ec);
      if (ec) return;
    }
    std::ofstream out(path, std::ios::app);
    const auto now = std::time(nullptr);
    std::tm time{};
    if (gmtime_r(&now, &time)) out << std::put_time(&time, "%Y-%m-%dT%H:%M:%SZ");
    out << " [" << getpid() << "] " << message.substr(0, max_message);
    if (message.size() > max_message) out << " [truncated]";
    out << '\n';
  } catch (...) { }
}

std::string apple_script_quote(std::string_view value) {
  std::string quoted = "\"";
  for (const char ch : value) {
    switch (ch) {
      case '\\': quoted += "\\\\"; break;
      case '"': quoted += "\\\""; break;
      case '\n': quoted += "\\n"; break;
      case '\r': quoted += "\\r"; break;
      case '\t': quoted += "\\t"; break;
      default: quoted += ch;
    }
  }
  return quoted + '"';
}

bool present_alert(std::string_view message, const char* helper) {
  std::string script = "display alert \"Holder\" message " + apple_script_quote(message);
  char* argv[] = {const_cast<char*>(helper), const_cast<char*>("-e"), script.data(), nullptr};
  pid_t child = 0;
  if (posix_spawn(&child, helper, nullptr, nullptr, argv, environ) != 0) return false;
  int status = 0;
  pid_t result;
  do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
  return result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
}
