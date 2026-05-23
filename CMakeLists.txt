cmake_minimum_required(VERSION 3.25)
project(fastauth
    VERSION 0.1.0
    DESCRIPTION "Fast face-auth daemon for Linux, IR-only"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor -Wold-style-cast
    -Wcast-align -Wunused -Woverloaded-virtual
    -Wconversion -Wsign-conversion
    -fstack-protector-strong
)

# === M1: V4L2 capture test ===
# Standalone утилита для проверки V4L2 pipeline на IR-камере.
# Не имеет внешних зависимостей — пишет PGM руками.
add_executable(fastauth-capture-test
    tools/m1_capture_test.cpp
)

# === M2: ONNX detection + embedding test ===
# Standalone утилита, подгружает SCRFD-500MF и MobileFaceNet, гоняет
# inference на PGM-кадре с M1. Использует OpenCV только для image ops
# (resize, warpAffine).
find_package(onnxruntime CONFIG REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs calib3d)

add_executable(fastauth-detect-test
    tools/m2_detect_test.cpp
)
target_link_libraries(fastauth-detect-test PRIVATE
    onnxruntime::onnxruntime
    opencv_core
    opencv_imgproc
    opencv_imgcodecs
    opencv_calib3d
)
