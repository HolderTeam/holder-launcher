#include "BackendProbe.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <string_view>

namespace holder {
namespace {
using Clock = std::chrono::steady_clock;
#ifdef HOLDER_PROBE_TEST_DIAGNOSTICS
std::atomic<unsigned> outstanding_requests{0};
#endif

struct InternetHandle {
  HINTERNET value = nullptr;
  ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
  InternetHandle(const InternetHandle&) = delete;
  InternetHandle& operator=(const InternetHandle&) = delete;
  explicit InternetHandle(HINTERNET handle) : value(handle) {}
};

// The caller and request callback each own a reference. Cancellation returns
// promptly; HANDLE_CLOSING releases the callback's reference after the last
// notification, keeping the event and pending read buffer alive until then.
struct RequestState {
  std::atomic<unsigned> references{1};
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  std::atomic<DWORD> status{0};
  std::atomic<DWORD> error{0};
  std::atomic<DWORD> bytes{0};
  std::array<char, 8192> buffer{};

  RequestState() {
#ifdef HOLDER_PROBE_TEST_DIAGNOSTICS
    ++outstanding_requests;
#endif
  }
  ~RequestState() {
    if (event) CloseHandle(event);
#ifdef HOLDER_PROBE_TEST_DIAGNOSTICS
    --outstanding_requests;
#endif
  }
  void release() { if (references.fetch_sub(1) == 1) delete this; }
};

struct StateOwner {
  RequestState* value = new RequestState;
  ~StateOwner() { value->release(); }
};

void CALLBACK on_status(HINTERNET, DWORD_PTR context, DWORD status,
                        void* information, DWORD length) {
  auto* state = reinterpret_cast<RequestState*>(context);
  if (!state) return;
  if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
    state->release();
    return;
  }
  switch (status) {
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
      state->error = static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError;
      break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
      state->bytes = length;
      break;
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
      break;
    default:
      return;
  }
  state->status = status;
  SetEvent(state->event);
}

bool await_completion(RequestState& state, DWORD expected, Clock::time_point deadline) {
  const auto now = Clock::now();
  if (now >= deadline) return false;
  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
  const auto wait = static_cast<DWORD>(std::min<long long>(remaining.count(), MAXDWORD - 1));
  return WaitForSingleObject(state.event, wait) == WAIT_OBJECT_0 &&
         Clock::now() < deadline && state.status == expected;
}

ProbeResult failure(DWORD error) {
  return error == ERROR_WINHTTP_INVALID_SERVER_RESPONSE ||
                 error == ERROR_WINHTTP_HEADER_SIZE_OVERFLOW
             ? ProbeResult::incompatible : ProbeResult::unavailable;
}
} // namespace

#ifdef HOLDER_PROBE_TEST_DIAGNOSTICS
unsigned outstanding_probe_requests() { return outstanding_requests.load(); }
#endif

ProbeResult backend_ping(std::uint16_t port, std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0) return ProbeResult::unavailable;
  // Production supplies one second. Clamp the internal seam to WinHTTP's range.
  const auto bounded_timeout = std::min<long long>(timeout.count(), std::numeric_limits<int>::max());
  const auto deadline = Clock::now() + std::chrono::milliseconds(bounded_timeout);
  InternetHandle session(WinHttpOpen(L"Holder Launcher", WINHTTP_ACCESS_TYPE_NO_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
  if (!session.value) return ProbeResult::unavailable;
  const int milliseconds = static_cast<int>(bounded_timeout);
  if (!WinHttpSetTimeouts(session.value, milliseconds, milliseconds, milliseconds, milliseconds))
    return ProbeResult::unavailable;
  InternetHandle connection(WinHttpConnect(session.value, L"127.0.0.1", port, 0));
  if (!connection.value) return ProbeResult::unavailable;

  StateOwner state;
  if (!state.value->event) return ProbeResult::unavailable;
  InternetHandle request(WinHttpOpenRequest(connection.value, L"GET", L"/ping", nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
  if (!request.value) return ProbeResult::unavailable;

  DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
  DWORD header_limit = 8192;
  DWORD_PTR context = reinterpret_cast<DWORD_PTR>(state.value);
  if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
      !WinHttpSetOption(request.value, WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE, &header_limit, sizeof(header_limit)) ||
      !WinHttpSetOption(request.value, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)))
    return ProbeResult::unavailable;
  if (WinHttpSetStatusCallback(request.value, on_status,
          WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) ==
      WINHTTP_INVALID_STATUS_CALLBACK)
    return ProbeResult::unavailable;
  state.value->references.fetch_add(1);

  if (Clock::now() >= deadline) return ProbeResult::unavailable;
  if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
          WINHTTP_NO_REQUEST_DATA, 0, 0, context))
    return failure(GetLastError());
  if (!await_completion(*state.value, WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, deadline))
    return failure(state.value->error);
  if (!WinHttpReceiveResponse(request.value, nullptr)) return failure(GetLastError());
  if (!await_completion(*state.value, WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, deadline))
    return failure(state.value->error);

  DWORD status = 0;
  DWORD size = sizeof(status);
  if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200)
    return ProbeResult::incompatible;

  constexpr std::string_view expected = "pong";
  std::size_t received = 0;
  for (;;) {
    if (Clock::now() >= deadline) return ProbeResult::unavailable;
    if (!WinHttpReadData(request.value, state.value->buffer.data(),
                        static_cast<DWORD>(state.value->buffer.size()), nullptr))
      return failure(GetLastError());
    if (!await_completion(*state.value, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, deadline))
      return failure(state.value->error);
    const DWORD bytes = state.value->bytes;
    if (bytes == 0)
      return received == expected.size() ? ProbeResult::healthy : ProbeResult::incompatible;
    if (bytes > expected.size() - received ||
        std::string_view(state.value->buffer.data(), bytes) != expected.substr(received, bytes))
      return ProbeResult::incompatible;
    received += bytes;
  }
}
} // namespace holder
