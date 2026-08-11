cmake_minimum_required(VERSION 3.22)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" HOLDER_LAUNCHER_VERSION)
string(STRIP "${HOLDER_LAUNCHER_VERSION}" HOLDER_LAUNCHER_VERSION)
if(NOT HOLDER_LAUNCHER_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION must contain a semantic version like 0.1.0")
endif()

project(holder_launcher VERSION "${HOLDER_LAUNCHER_VERSION}" LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "holder-launcher is a Windows-only Win32 launcher")
endif()

add_executable(Holder WIN32
  src/main.cpp
)

target_compile_features(Holder PRIVATE cxx_std_20)
target_compile_definitions(Holder PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

target_link_libraries(Holder PRIVATE
  user32
  winhttp
)
