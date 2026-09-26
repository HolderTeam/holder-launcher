#pragma once

#include <chrono>
#include <cstdint>

namespace holder {
enum class ProbeResult { healthy, unavailable, incompatible };

// Internal endpoint/deadline seam for tests. The shipped launcher uses 11499.
ProbeResult backend_ping(std::uint16_t port, std::chrono::milliseconds timeout);
#ifdef HOLDER_PROBE_TEST_DIAGNOSTICS
unsigned outstanding_probe_requests();
#endif
}
