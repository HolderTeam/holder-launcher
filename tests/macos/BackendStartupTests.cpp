#include "BackendStartup.h"
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>
using namespace std::chrono_literals;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Fake {
  holder::StartupClock::time_point time{};
  int starts = 0, probes = 0, sleeps = 0;
  holder::StartupActions actions{
    [&] { return time; },
    [&](auto delay) { require(delay > 0ms && delay <= 250ms, "invalid sleep"); ++sleeps; time += delay; },
    [&](auto budget) { require(budget > 0ms && budget <= 1000ms, "invalid probe budget"); ++probes; return false; },
    [&] { ++starts; return std::string{}; },
    [] { return std::string{}; }
  };
  auto elapsed() { return time.time_since_epoch(); }
};
}
int main() {
  int failed = 0;
  auto test = [&](const char* name, auto fn) {
    try { fn(); std::cout << "PASS " << name << '\n'; }
    catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
  };
  test("reuse healthy daemon", [] {
    Fake f; f.actions.probe = [](auto) { return true; };
    require(holder::ensure_backend(f.actions).empty() && f.starts == 0 && f.sleeps == 0, "healthy daemon not reused");
  });
  test("immediately ready after spawn", [] {
    Fake f; f.actions.probe = [&](auto) { return ++f.probes == 2; };
    require(holder::ensure_backend(f.actions).empty() && f.starts == 1 && f.sleeps == 0, "unnecessary delay");
  });
  test("slow cold start gets sixty seconds", [] {
    Fake f; f.actions.probe = [&](auto) { return f.elapsed() >= 45s; };
    require(holder::ensure_backend(f.actions).empty() && f.starts == 1 && f.elapsed() == 45s, "cold start abandoned");
  });
  test("timeout without final extra sleep", [] {
    Fake f;
    require(holder::ensure_backend(f.actions).find("60 seconds") != std::string::npos, "missing timeout");
    require(f.elapsed() == 60s && f.starts == 1, "wrong overall deadline");
  });
  test("initial probe spawn and retries share budget", [] {
    Fake f; bool capped = false;
    f.actions.start = [&] { ++f.starts; f.time += 137ms; return std::string{}; };
    f.actions.probe = [&](auto budget) {
      require(budget <= std::chrono::duration_cast<std::chrono::milliseconds>(60s - f.elapsed()), "probe exceeds remaining time");
      if (budget < 1000ms) capped = true;
      f.time += budget; return false;
    };
    require(!holder::ensure_backend(f.actions).empty() && f.elapsed() == 60s && capped, "budget renewed");
  });
  test("spawn failure", [] {
    Fake f; f.actions.start = [] { return std::string("spawn failed"); };
    require(holder::ensure_backend(f.actions) == "spawn failed" && f.sleeps == 0, "spawn error lost");
  });
  test("readiness exactly at deadline accepted", [] {
    Fake f;
    f.actions.start = [&] { f.time += 59s; return std::string{}; };
    f.actions.probe = [&](auto budget) {
      if (++f.probes == 1) return false;
      f.time += budget; return true;
    };
    require(holder::ensure_backend(f.actions).empty() && f.elapsed() == 60s, "boundary readiness rejected");
  });
  test("late readiness rejected", [] {
    Fake f; f.actions.probe = [&](auto) { f.time += 61s; return true; };
    require(!holder::ensure_backend(f.actions).empty() && f.starts == 0, "late probe accepted or child started");
  });
  test("native running child check is nonblocking", [] {
    int gate[2];
    require(pipe(gate) == 0, "pipe failed");
    pid_t pid = fork();
    if (pid < 0) { close(gate[0]); close(gate[1]); throw std::runtime_error("fork failed"); }
    if (pid == 0) {
      close(gate[1]); char byte; (void)read(gate[0], &byte, 1); _exit(0);
    }
    close(gate[0]);
    const auto original = pid;
    const auto result = holder::backend_exit_status(pid);
    close(gate[1]); // Release the child even if the assertion fails.
    waitpid(original, nullptr, 0);
    require(result.empty() && pid == original, "running child treated as exited");
  });
  test("early exit diagnosed after final probe", [] {
    Fake f; f.actions.child_exit = [] { return std::string("exit code 7"); };
    require(holder::ensure_backend(f.actions) == "exit code 7" && f.probes == 3 && f.sleeps == 0, "exit not checked promptly");
  });
  test("healthy concurrent winner accepted", [] {
    Fake f; f.actions.child_exit = [] { return std::string("exit code 1"); };
    f.actions.probe = [&](auto) { return ++f.probes == 3; };
    require(holder::ensure_backend(f.actions).empty() && f.starts == 1, "winner rejected");
  });
  test("no probe after deadline on child exit", [] {
    Fake f; f.actions.probe = [&](auto budget) { ++f.probes; f.time += budget; return false; };
    f.actions.start = [&] { f.time += 59s; return std::string{}; };
    require(!holder::ensure_backend(f.actions).empty() && f.probes == 1 && f.elapsed() == 60s, "probe past deadline");
  });
  test("native child exit code signal and reaping", [] {
    for (bool signal : {false, true}) {
      pid_t pid = fork();
      require(pid >= 0, "fork failed");
      if (pid == 0) {
        if (signal) { std::signal(SIGTERM, SIG_DFL); raise(SIGTERM); }
        _exit(7);
      }
      const auto original = pid;
      std::string exit;
      const auto deadline = holder::StartupClock::now() + 2s;
      while (pid > 0 && holder::StartupClock::now() < deadline) {
        exit = holder::backend_exit_status(pid);
        if (exit.empty()) std::this_thread::sleep_for(1ms);
      }
      // Clean up even if the assertion below fails.
      if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, nullptr, 0); }
      require(exit.find(signal ? "signal 15" : "exit code 7") != std::string::npos, "wrong native status");
      require(pid == 0 && waitpid(original, nullptr, WNOHANG) == -1, "child not reaped");
      require(holder::backend_exit_status(pid).empty(), "exit reported twice");
    }
  });
  return failed == 0 ? 0 : 1;
}
