find_package(Git QUIET)
set(identity_dir "${CMAKE_CURRENT_BINARY_DIR}/provenance/$<CONFIG>")
add_custom_target(build_identity
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DOUTPUT_DIR=${identity_dir}" "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteBuildIdentity.cmake"
  BYPRODUCTS "${identity_dir}/BuildIdentity.h" "${identity_dir}/source.json"
  VERBATIM)
add_dependencies(Holder build_identity)
target_include_directories(Holder PRIVATE "${identity_dir}")
if(TARGET launcher_integration_driver)
  add_dependencies(launcher_integration_driver build_identity)
  target_include_directories(launcher_integration_driver PRIVATE "${identity_dir}")
endif()

if(WIN32)
  enable_language(RC)
  foreach(part MAJOR MINOR PATCH)
    if(PROJECT_VERSION_${part} GREATER 65535)
      message(FATAL_ERROR "Windows version resource components must be <= 65535")
    endif()
  endforeach()
  configure_file(src/windows/Holder.rc.in "${CMAKE_CURRENT_BINARY_DIR}/Holder.rc" @ONLY)
  target_sources(Holder PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Holder.rc")
endif()

set(artifact_architecture "${CMAKE_SYSTEM_PROCESSOR}")
if(MSVC)
  set(artifact_architecture "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
elseif(APPLE AND CMAKE_OSX_ARCHITECTURES)
  set(artifact_architecture "${CMAKE_OSX_ARCHITECTURES}")
endif()
# JSON quoting is explicit so toolchain names and Windows paths cannot corrupt it.
function(json_quote output value)
  string(REPLACE "\\" "\\\\" value "${value}")
  string(REPLACE "\"" "\\\"" value "${value}")
  string(REPLACE "\n" "\\n" value "${value}")
  string(REPLACE "\r" "\\r" value "${value}")
  string(REPLACE "\t" "\\t" value "${value}")
  set(${output} "\"${value}\"" PARENT_SCOPE)
endfunction()
set(config "{}")
foreach(key version platform architecture compiler compiler_version generator cmake_version)
  if(key STREQUAL "version")
    set(value "${PROJECT_VERSION}")
  elseif(key STREQUAL "platform")
    set(value "${CMAKE_SYSTEM_NAME}")
  elseif(key STREQUAL "architecture")
    set(value "${artifact_architecture}")
  elseif(key STREQUAL "compiler")
    set(value "${CMAKE_CXX_COMPILER_ID}")
  elseif(key STREQUAL "compiler_version")
    set(value "${CMAKE_CXX_COMPILER_VERSION}")
  elseif(key STREQUAL "generator")
    set(value "${CMAKE_GENERATOR}")
  else()
    set(value "${CMAKE_VERSION}")
  endif()
  json_quote(quoted "${value}")
  string(JSON config SET "${config}" "${key}" "${quoted}")
endforeach()
string(JSON config SET "${config}" configuration "\"$<CONFIG>\"")
file(GENERATE OUTPUT "${identity_dir}/config.json" CONTENT "${config}\n")
add_custom_command(TARGET Holder POST_BUILD
  COMMAND "${CMAKE_COMMAND}" "-DBINARY=$<TARGET_FILE:Holder>"
    "-DIDENTITY=${identity_dir}/source.json" "-DCONFIG=${identity_dir}/config.json"
    "-DOUTPUT=${identity_dir}/built.json"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CaptureBuild.cmake"
  VERBATIM)

# Keep crash-debugging symbols for optimized Release artifacts too. They are
# staged separately and are not installed with the launcher executable.
if(MSVC)
  target_compile_options(Holder PRIVATE "$<$<CONFIG:Release>:/Zi>")
  target_link_options(Holder PRIVATE "$<$<CONFIG:Release>:/DEBUG>")
elseif(APPLE)
  target_compile_options(Holder PRIVATE "$<$<CONFIG:Release>:-g>")
endif()

if(APPLE)
  find_program(DSYMUTIL_EXECUTABLE dsymutil REQUIRED)
  add_custom_command(TARGET Holder POST_BUILD
    COMMAND "${DSYMUTIL_EXECUTABLE}" "$<TARGET_FILE:Holder>" -o "$<TARGET_FILE:Holder>.dSYM"
    VERBATIM)
endif()

# Packaging is optional for local production-only builds without Python.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  set(symbol_arguments)
  if(MSVC)
    list(APPEND symbol_arguments --symbols "$<TARGET_PDB_FILE:Holder>")
  elseif(APPLE)
    list(APPEND symbol_arguments --symbols "$<TARGET_FILE:Holder>.dSYM")
  endif()
  add_custom_target(stage_artifact
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/artifact.py" create
      --binary "$<TARGET_FILE:Holder>" --build-record "${identity_dir}/built.json"
      ${symbol_arguments} --output "${CMAKE_CURRENT_BINARY_DIR}/artifacts/$<CONFIG>"
    DEPENDS Holder VERBATIM)
  if(BUILD_TESTING)
    add_test(NAME artifact_provenance
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ArtifactTests.py")
    set_tests_properties(artifact_provenance PROPERTIES TIMEOUT 30)
    if(GIT_FOUND)
      add_test(NAME build_identity
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildIdentityTests.py"
          "${CMAKE_COMMAND}" "${GIT_EXECUTABLE}")
      set_tests_properties(build_identity PROPERTIES TIMEOUT 30)
    endif()
    if(WIN32)
      add_test(NAME windows_version_resource
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/windows/VersionResourceTests.py"
          "$<TARGET_FILE:Holder>" "${identity_dir}/built.json")
      set_tests_properties(windows_version_resource PROPERTIES TIMEOUT 10)
    endif()
  endif()
endif()
