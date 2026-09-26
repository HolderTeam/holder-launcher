cmake_minimum_required(VERSION 3.22)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" HOLDER_LAUNCHER_VERSION)
string(STRIP "${HOLDER_LAUNCHER_VERSION}" HOLDER_LAUNCHER_VERSION)
if(NOT HOLDER_LAUNCHER_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION must contain a semantic version like 0.1.0")
endif()

project(holder_launcher VERSION "${HOLDER_LAUNCHER_VERSION}" LANGUAGES CXX)
include(CTest)

if(WIN32)
  add_executable(Holder WIN32
    src/windows/main.cpp
  )
elseif(APPLE)
  add_library(holder_runtime STATIC src/macos/RuntimeSupport.cpp)
  target_compile_features(holder_runtime PUBLIC cxx_std_20)
  target_include_directories(holder_runtime PUBLIC src/macos)
  target_compile_options(holder_runtime PRIVATE -Wall -Wextra -Wpedantic)
  add_library(holder_startup STATIC src/macos/BackendStartup.cpp)
  target_compile_features(holder_startup PUBLIC cxx_std_20)
  target_include_directories(holder_startup PUBLIC src/macos)
  target_compile_options(holder_startup PRIVATE -Wall -Wextra -Wpedantic)
  add_library(holder_layout STATIC src/macos/InstallLayout.cpp)
  target_compile_features(holder_layout PUBLIC cxx_std_20)
  target_include_directories(holder_layout PUBLIC src/macos)
  target_compile_options(holder_layout PRIVATE -Wall -Wextra -Wpedantic)
  add_library(holder_probe STATIC src/macos/BackendProbe.cpp)
  target_compile_features(holder_probe PUBLIC cxx_std_20)
  target_include_directories(holder_probe PUBLIC src/macos)
  target_compile_options(holder_probe PRIVATE -Wall -Wextra -Wpedantic)
  add_executable(Holder
    src/macos/main.cpp
  )
  target_compile_definitions(Holder PRIVATE HOLDER_LAUNCHER_VERSION="${HOLDER_LAUNCHER_VERSION}")
  target_compile_options(Holder PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(Holder PRIVATE holder_probe holder_layout holder_startup holder_runtime)
  if(BUILD_TESTING)
    add_executable(runtime_support_tests tests/macos/RuntimeSupportTests.cpp)
    target_link_libraries(runtime_support_tests PRIVATE holder_runtime)
    target_compile_options(runtime_support_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME macos_runtime_support COMMAND runtime_support_tests)
    set_tests_properties(macos_runtime_support PROPERTIES TIMEOUT 10)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(launcher_integration_driver src/macos/main.cpp)
    target_compile_definitions(launcher_integration_driver PRIVATE HOLDER_LAUNCHER_INTEGRATION_TEST=1 HOLDER_LAUNCHER_VERSION="${HOLDER_LAUNCHER_VERSION}")
    target_compile_options(launcher_integration_driver PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(launcher_integration_driver PRIVATE holder_probe holder_layout holder_startup holder_runtime)
    add_test(NAME macos_launcher_integration
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/macos/LauncherIntegration.py"
        "$<TARGET_FILE:launcher_integration_driver>")
    # Leave enough time for every bounded case to clean up even on widespread failures.
    set_tests_properties(macos_launcher_integration PROPERTIES TIMEOUT 180)
    add_executable(backend_startup_tests tests/macos/BackendStartupTests.cpp)
    target_link_libraries(backend_startup_tests PRIVATE holder_startup)
    target_compile_options(backend_startup_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME macos_backend_startup COMMAND backend_startup_tests)
    set_tests_properties(macos_backend_startup PROPERTIES TIMEOUT 10)
    add_executable(backend_probe_tests tests/macos/BackendProbeTests.cpp)
    target_link_libraries(backend_probe_tests PRIVATE holder_probe)
    target_compile_options(backend_probe_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME macos_backend_probe COMMAND backend_probe_tests)
    set_tests_properties(macos_backend_probe PROPERTIES TIMEOUT 15)
    add_executable(install_layout_tests tests/macos/InstallLayoutTests.cpp)
    target_link_libraries(install_layout_tests PRIVATE holder_layout)
    target_compile_options(install_layout_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME macos_install_layout COMMAND install_layout_tests)
    set_tests_properties(macos_install_layout PROPERTIES TIMEOUT 10)
  endif()
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
