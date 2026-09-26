#include "BackendProbe.h"
#include "BackendStartup.h"
#include "InstallLayout.h"

#include <Availability.h>
#include <errno.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
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

uint16_t backend_port() {
#ifdef HOLDER_LAUNCHER_INTEGRATION_TEST
  // Only the test executable accepts an isolated endpoint; never enabled on Holder.
  const char* port = std::getenv("HOLDER_TEST_PORT");
  if (!port) throw std::runtime_error("Missing integration test port");
  return static_cast<uint16_t>(std::stoi(port));
#else
  return 11499;
#endif
}

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
#ifdef HOLDER_LAUNCHER_INTEGRATION_TEST
  std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
#else
  const std::string script =
      "display alert \"Holder\" message " + apple_script_quote(message);
  const std::string command = "osascript -e " + shell_quote(script);
  (void)std::system(command.c_str());
#endif
}

void configure_runtime_environment(const std::filesystem::path& working_dir) {
  setenv("GSETTINGS_SCHEMA_DIR", (working_dir / "share" / "glib-2.0" / "schemas").c_str(), 1);
  setenv("GIO_MODULE_DIR", (working_dir / "lib" / "gio" / "modules").c_str(), 1);
  setenv(
      "GDK_PIXBUF_MODULE_FILE",
      (working_dir / "lib" / "gdk-pixbuf-2.0" / "2.10.0" / "loaders.cache").c_str(),
      1
  );
  setenv("GTK_PATH", (working_dir / "lib" / "gtk-4.0").c_str(), 1);
  setenv("XDG_DATA_DIRS", (working_dir / "share").c_str(), 1);
  setenv("ENCHANT_CONFIG_DIR", (working_dir / "share" / "enchant-2").c_str(), 1);
  setenv("DICPATH", (working_dir / "share" / "enchant" / "hunspell").c_str(), 1);
}

bool start_process(
    const std::filesystem::path& exe,
    const std::filesystem::path& working_dir,
    std::string* error,
    pid_t& pid
) {
  configure_runtime_environment(working_dir);

  posix_spawn_file_actions_t actions;
  int action_rc = posix_spawn_file_actions_init(&actions);
  if (action_rc != 0) {
    if (error) {
      *error = "Failed to initialize spawn actions: " + std::string(std::strerror(action_rc));
    }
    return false;
  }

  // Older SDKs only declare the extension; older systems still need it at runtime.
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
  if (__builtin_available(macOS 26.0, *)) {
    action_rc = posix_spawn_file_actions_addchdir(&actions, working_dir.c_str());
  } else
#endif
  {
    // Required on pre-26 macOS; suppress deprecation only for this fallback.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    action_rc = posix_spawn_file_actions_addchdir_np(&actions, working_dir.c_str());
#pragma clang diagnostic pop
  }
  if (action_rc != 0) {
    posix_spawn_file_actions_destroy(&actions);
    if (error) {
      *error = "Failed to configure working directory " + working_dir.string() + ": " +
               std::strerror(action_rc);
    }
    return false;
  }

  pid = 0;
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

bool exec_process(
    const std::filesystem::path& exe,
    const std::filesystem::path& working_dir,
    std::string* error
) {
  configure_runtime_environment(working_dir);

  if (chdir(working_dir.c_str()) != 0) {
    if (error) {
      *error = "Failed to enter " + working_dir.string() + ": " + std::strerror(errno);
    }
    return false;
  }

  std::string executable = exe.string();
  char* argv[] = {executable.data(), nullptr};
  execve(executable.c_str(), argv, environ);

  if (error) {
    *error = "Failed to start " + executable + ": " + std::strerror(errno);
  }
  return false;
}

int run_launcher() {
  const auto layout = holder::resolve_layout(executable_path());
  append_log("Holder launcher starting");

  if (const auto error = holder::validate_layout(layout); !error.empty()) {
    show_error(error);
    return 1;
  }

  pid_t backend_pid = 0;
  const holder::StartupActions actions{
      [] { return holder::StartupClock::now(); },
      [](auto delay) { std::this_thread::sleep_for(delay); },
      [](auto timeout) { return holder::backend_ping(backend_port(), timeout); },
      [&] {
        append_log("Backend is not healthy; starting holderd (60-second readiness budget)");
        std::string error;
        start_process(layout.backend_exe, layout.root_dir, &error, backend_pid);
        return error;
      },
      [&] { return holder::backend_exit_status(backend_pid); },
  };
  if (const auto error = holder::ensure_backend(actions); !error.empty()) {
    show_error(error + "\n\nLauncher log:\n" + log_path().string());
    return 1;
  }
  // Reap our child if it exited while a competing backend became ready.
  // A still-running daemon is left alone and survives the desktop exec handoff.
  (void)holder::backend_exit_status(backend_pid);

  append_log("Starting holder-desktop");
  std::string desktop_error;
  if (!exec_process(layout.desktop_exe, layout.root_dir, &desktop_error)) {
    show_error(desktop_error);
    return 1;
  }

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
