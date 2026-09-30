#include "BackendProbe.h"
#include "BackendStartup.h"
#include "InstallLayout.h"
#include "RuntimeSupport.h"
#include "BuildIdentity.h"

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
  holder::append_launcher_log(log_path(), message);
}

void show_error(std::string_view message) {
  append_log(message);
  // stderr remains useful when launched from a terminal or if the alert fails.
  std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
#ifdef HOLDER_LAUNCHER_INTEGRATION_TEST
  const char* helper = std::getenv("HOLDER_TEST_ALERT_HELPER");
  if (!helper) return; // Automated tests never open dialogs.
#else
  const char* helper = "/usr/bin/osascript";
#endif
  if (!holder::present_alert(message, helper))
    append_log("Could not display error alert; diagnostic written to stderr.");
}

bool start_process(
    const std::filesystem::path& exe,
    const std::filesystem::path& working_dir,
    std::string* error,
    pid_t& pid
) {
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
  if (const auto setup_error = holder::configure_desktop_environment(working_dir); !setup_error.empty()) {
    if (error) *error = setup_error;
    return false;
  }

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
  const auto started = holder::StartupClock::now();
    append_log("Holder launcher " HOLDER_LAUNCHER_VERSION " source=" HOLDER_BUILD_SOURCE " starting; runtime root: " + layout.root_dir.string());

  if (const auto error = holder::validate_layout(layout); !error.empty()) {
    show_error(error);
    return 1;
  }

  pid_t backend_pid = 0;
  bool incompatible = false;
  const holder::StartupActions actions{
      [] { return holder::StartupClock::now(); },
      [](auto delay) { std::this_thread::sleep_for(delay); },
      [&](auto timeout) { return holder::backend_ping(backend_port(), timeout, &incompatible); },
      [&] {
        append_log("Backend is not healthy; starting holderd (60-second readiness budget)");
        std::string error;
        start_process(layout.backend_exe, layout.root_dir, &error, backend_pid);
        return error;
      },
      [&] { return holder::backend_exit_status(backend_pid); },
      [&] { return incompatible ?
          "The service at 127.0.0.1:" + std::to_string(backend_port()) +
              " did not return Holder's expected ping response. Check for another service or an incompatible backend." : std::string{}; },
  };
  auto budget = std::chrono::milliseconds(60000);
#ifdef HOLDER_LAUNCHER_INTEGRATION_TEST
  if (const char* value = std::getenv("HOLDER_TEST_STARTUP_MS"))
    budget = std::chrono::milliseconds(std::stoi(value));
#endif
  if (const auto error = holder::ensure_backend(actions, budget); !error.empty()) {
    show_error(error + "\n\nLauncher log:\n" + log_path().string());
    return 1;
  }
  // Reap our child if it exited while a competing backend became ready.
  // A still-running daemon is left alone and survives the desktop exec handoff.
  (void)holder::backend_exit_status(backend_pid);

  append_log("Backend ready after " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
      holder::StartupClock::now() - started).count()) + " ms; starting holder-desktop");
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
