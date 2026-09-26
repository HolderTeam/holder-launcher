#include "BackendProbe.h"

#include <iostream>
#include <string>
#include <windows.h>

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  const auto port = static_cast<std::uint16_t>(std::stoul(argv[1]));
  const auto timeout = std::chrono::milliseconds(std::stoul(argv[2]));
  const auto repetitions = std::stoul(argv[3]);
  for (unsigned long i = 0; i < repetitions; ++i) {
    const auto start = std::chrono::steady_clock::now();
    const auto result = holder::backend_ping(port, timeout);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    const char* name = result == holder::ProbeResult::healthy ? "healthy" :
        result == holder::ProbeResult::incompatible ? "incompatible" : "unavailable";
    std::cout << name << " " << elapsed << "\n";
  }
  // WinHTTP retains internal process-wide resources. Check our actual request
  // contexts/events/buffers instead of assuming the OS handle count is stable.
  for (int i = 0; i < 100 && holder::outstanding_probe_requests() != 0; ++i)
    Sleep(10);
  if (holder::outstanding_probe_requests() != 0) {
    std::cerr << "Request contexts still alive: " << holder::outstanding_probe_requests() << "\n";
    return 1;
  }
}
