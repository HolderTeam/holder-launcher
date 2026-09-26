#include "RuntimeSupport.h"
#include <cerrno>
#include <iostream>
#include <string>

namespace {
int calls = 0;
int fail_setenv(const char*, const char*, int) {
  ++calls;
  errno = ENOMEM;
  return -1;
}
}
int main() {
  const auto error = holder::configure_desktop_environment("/private/test", fail_setenv);
  if (calls != 1 || error.find("GSETTINGS_SCHEMA_DIR") == std::string::npos ||
      error.find("Failed to configure") == std::string::npos) {
    std::cerr << "Environment failure was not reported immediately\n";
    return 1;
  }
  if (holder::apple_script_quote("a\"b\\c\n\r\t") != "\"a\\\"b\\\\c\\n\\r\\t\"") {
    std::cerr << "AppleScript string escaping failed\n";
    return 1;
  }
  if (holder::present_alert("test", "/usr/bin/false")) {
    std::cerr << "Alert failure exit status ignored\n";
    return 1;
  }
  return 0;
}
