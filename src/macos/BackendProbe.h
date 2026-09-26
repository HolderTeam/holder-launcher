#pragma once

#include <chrono>
#include <cstdint>

namespace holder {
// Internal endpoint seam for native tests; production always uses port 11499.
bool backend_ping(uint16_t port, std::chrono::milliseconds timeout,
                  bool* incompatible_response = nullptr);
}
