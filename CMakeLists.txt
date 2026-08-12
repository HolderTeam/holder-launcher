cmake_minimum_required(VERSION 3.22)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" HOLDER_LAUNCHER_VERSION)
string(STRIP "${HOLDER_LAUNCHER_VERSION}" HOLDER_LAUNCHER_VERSION)
if(NOT HOLDER_LAUNCHER_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION must contain a semantic version like 0.1.0")
endif()

project(holder_launcher VERSION "${HOLDER_LAUNCHER_VERSION}" LANGUAGES CXX)

if(WIN32)
  add_executable(Holder WIN32
    src/windows/main.cpp
  )
elseif(APPLE)
  add_executable(Holder
    src/macos/main.cpp
  )
else()
  message(FATAL_ERROR "holder-launcher currently supports Windows and macOS only")
endif()

target_compile_features(Holder PRIVATE cxx_std_20)

if(WIN32)
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
endif()
