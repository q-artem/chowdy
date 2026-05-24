cmake_minimum_required(VERSION 3.25)
project(fastauth
    VERSION 0.1.0
    DESCRIPTION "Fast face-auth daemon for Linux, IR-only"
    LANGUAGES C CXX
)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Generate compile_commands.json" FORCE)

option(FASTAUTH_BUILD_TOOLS "Build milestone testbeds in tools/" ON)
option(FASTAUTH_BUILD_PAM   "Build pam_fastauth.so"               ON)
option(FASTAUTH_BUILD_TESTS "Build unit tests (GoogleTest)"       OFF)

include(cmake/CompileOptions.cmake)

# === third-party (shared) ===
find_package(onnxruntime CONFIG REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs calib3d)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
pkg_check_modules(LIBPAM REQUIRED IMPORTED_TARGET pam)
find_package(Threads REQUIRED)

# === project modules ===
add_subdirectory(proto)
add_subdirectory(common)
add_subdirectory(daemon)
add_subdirectory(cli)

if(FASTAUTH_BUILD_PAM)
    add_subdirectory(pam)
endif()

if(FASTAUTH_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(FASTAUTH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Convenience: print where the artifacts are once configured.
message(STATUS "fastauth: BUILD_TOOLS=${FASTAUTH_BUILD_TOOLS} BUILD_PAM=${FASTAUTH_BUILD_PAM} BUILD_TESTS=${FASTAUTH_BUILD_TESTS}")
