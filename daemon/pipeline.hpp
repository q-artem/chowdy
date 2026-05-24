// Camera + Detector + Embedder, glued. One Pipeline instance per daemon
// (single capture thread). Owns the camera and the two ORT sessions.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include <opencv2/core.hpp>
#include <onnxruntime_cxx_api.h>

#include "common/encoding.hpp"
#include "daemon/camera.hpp"
#include "daemon/models.hpp"

namespace fastauth::daemon {

struct PipelineConfig {
    CameraConfig          camera;
    std::filesystem::path detector_model = "/var/lib/fastauth/models/detector.onnx";
    std::filesystem::path embedder_model = "/var/lib/fastauth/models/embedder.onnx";
    int                   intra_op_threads = 2;

    // Drop the first N frames after the camera opens — UVC exposure needs
    // a few frames to stabilise (DESIGN.md §15 / M1 findings).
    int                   warmup_frames = 3;

    // Skip frames whose mean brightness is below this — the IR emitter on
    // 13d3:56eb pulses, off-frames are unusable (M3 findings, hardware-ir-cam
    // memory entry).
    double                dark_threshold = 25.0;

    // SCRFD config.
    float                 detector_conf_threshold = 0.5f;
    float                 nms_iou                 = 0.4f;

    // Minimum face quality during enrollment (DESIGN §7 formula).
    // On this IR sensor the §7 formula clamps too aggressively — see M3
    // findings. 0.10 is the empirical floor.
    float                 enroll_quality_min      = 0.10f;

    // Camera policy (DESIGN §6):
    //   "warm"       — open at start, never close
    //   "idle_keep"  — close after `idle_keep_ms` ms of no use (default)
    //   "lazy"       — close immediately after every request
    std::string           camera_policy   = "idle_keep";
    int                   idle_keep_ms    = 10000;
};

struct FrameOutcome {
    bool                            captured     = false;
    bool                            has_face     = false;
    double                          brightness   = 0.0;
    std::optional<Detection>        detection;
    std::optional<common::encoding::Embedding> embedding;
    double                          quality      = 0.0;
};

class Pipeline {
public:
    explicit Pipeline(const PipelineConfig& cfg);
    ~Pipeline();

    const PipelineConfig& config() const noexcept { return cfg_; }

    // Make sure the camera is streaming. Idempotent. Used by warm/idle_keep
    // policies; the auth handler calls it before its capture loop.
    void ensure_camera_open();

    // Stop streaming + release the camera. Safe even if not currently open.
    void release_camera();

    // One synchronous step. Captures a frame (returns captured=false on
    // timeout), filters dark/blank frames, detects, and if a face is
    // present runs the embedder. Skipping the embedder is controlled by
    // run_embedder.
    FrameOutcome process_one_frame(std::chrono::milliseconds capture_timeout,
                                   bool run_embedder = true);

    uint32_t embedder_model_id() const noexcept;

    // Serialises capture-side work so multiple connection threads can call
    // through safely. Pipeline state is otherwise single-owner.
    std::mutex& mutex() noexcept { return mu_; }

private:
    void idle_keeper_loop();

    Ort::Env              env_;
    PipelineConfig        cfg_;
    Camera                camera_;
    Detector              detector_;
    Embedder              embedder_;
    int                   frames_since_open_ = 0;
    std::mutex            mu_;
    std::atomic<std::chrono::steady_clock::time_point::rep> last_use_ns_{0};

    // Background thread that closes the camera after idle_keep_ms ms of
    // inactivity. Only started when policy == "idle_keep" and
    // idle_keep_ms > 0.
    std::thread                  idle_thread_;
    std::atomic<bool>            idle_stop_{false};
    std::condition_variable      idle_cv_;
    std::mutex                   idle_cv_mu_;
};

// Quality scoring (DESIGN.md §7). Face-crop sharpness is via variance of
// Laplacian; the divisor (kSharpnessDivisor) is tuned for the IR sensor —
// see M3 findings.
double frame_quality(const cv::Mat& grey, const Detection& det,
                     double sharpness_divisor = 30.0);

} // namespace fastauth::daemon
