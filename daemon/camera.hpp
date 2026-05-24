// V4L2 capture front-end for fastauthd.
//
// Promotes the throwaway code from tools/m3_pipeline_test.cpp to a proper
// RAII class. GREY-only (the target IR sensor doesn't offer anything else,
// see DESIGN.md Appendix A) and mmap-based.
//
// Thread-safety: a Camera instance is NOT thread-safe. The daemon either
// confines all capture calls to a single capture thread (warm policy), or
// holds an external mutex when policy=idle_keep wakes the camera on demand.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace fastauth::daemon {

struct CameraConfig {
    std::string device      = "/dev/video2";
    int         width       = 640;
    int         height      = 360;
    int         fps         = 30;
    std::size_t buffer_count = 4;
};

class Camera {
public:
    Camera() = default;
    ~Camera();

    Camera(const Camera&)            = delete;
    Camera& operator=(const Camera&) = delete;

    // Open the device, configure GREY/size/fps, mmap buffers, start streaming.
    // Throws std::runtime_error with errno detail on any V4L2 failure.
    void open(const CameraConfig& cfg);

    // True if streaming.
    bool is_open() const noexcept { return fd_ >= 0; }

    // Stop streaming, unmap buffers, close the device. Safe to call repeatedly.
    void close() noexcept;

    // Wait up to `timeout` for one frame; returns an empty Mat on timeout.
    // The returned Mat owns its data (the mmap buffer is recycled before
    // returning) so it's safe to keep across further capture() calls.
    cv::Mat capture(std::chrono::milliseconds timeout);

private:
    struct MmapBuf { void* start = nullptr; std::size_t length = 0; };

    int                   fd_ = -1;
    CameraConfig          cfg_{};
    std::vector<MmapBuf>  buffers_;
};

} // namespace fastauth::daemon
