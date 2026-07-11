cmake_minimum_required(VERSION 3.25)
project(chowdy
    VERSION 0.1.0
    DESCRIPTION "Fast face-auth daemon for Linux, IR-only"
    LANGUAGES C CXX
)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Generate compile_commands.json" FORCE)

option(CHOWDY_BUILD_TOOLS "Build milestone testbeds in tools/" ON)
option(CHOWDY_BUILD_PAM   "Build pam_chowdy.so"               ON)
option(CHOWDY_BUILD_TESTS "Build unit tests (GoogleTest)"       OFF)

include(cmake/CompileOptions.cmake)

# === third-party (shared) ===
find_package(onnxruntime CONFIG REQUIRED)

# OpenCV 5 split the old `calib3d` module: estimateAffinePartial2D (our
# 5-point face alignment) now lives in the `geometry` module. Pick the
# right component + link lib per major version so we build against both
# OpenCV 4 and 5.
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
if(OpenCV_VERSION_MAJOR GREATER_EQUAL 5)
    find_package(OpenCV REQUIRED COMPONENTS geometry)
    set(CHOWDY_OPENCV_AFFINE_LIB opencv_geometry)
else()
    find_package(OpenCV REQUIRED COMPONENTS calib3d)
    set(CHOWDY_OPENCV_AFFINE_LIB opencv_calib3d)
endif()
message(STATUS "chowdy: OpenCV ${OpenCV_VERSION}, affine lib = ${CHOWDY_OPENCV_AFFINE_LIB}")

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

if(CHOWDY_BUILD_PAM)
    add_subdirectory(pam)
endif()

if(CHOWDY_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(CHOWDY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Convenience: print where the artifacts are once configured.
message(STATUS "chowdy: BUILD_TOOLS=${CHOWDY_BUILD_TOOLS} BUILD_PAM=${CHOWDY_BUILD_PAM} BUILD_TESTS=${CHOWDY_BUILD_TESTS}")
