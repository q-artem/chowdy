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

namespace chowdy::daemon {

struct PipelineConfig {
    CameraConfig          camera;
    std::filesystem::path detector_model = "/var/lib/chowdy/models/detector.onnx";
    std::filesystem::path embedder_model = "/var/lib/chowdy/models/embedder.onnx";
    int                   intra_op_threads = 4;

    // Drop the first N frames after the camera STREAMs on. Originally 3
    // for UVC exposure stabilisation, but profiling (M5+) showed
    // dark_threshold + detector_conf_threshold already filter the mush;
    // throwing away usable frames costs ~67 ms each. Default now 0.
    int                   warmup_frames = 0;

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

    // Safety net for "lazy": even though each request closes the camera
    // immediately, a watchdog force-closes the stream if it's somehow still
    // on this many ms after the last use. Backstops any edge case that
    // leaves the indicator LED lit. 0 disables the watchdog.
    int                   lazy_safety_close_ms = 5000;
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

    // Same, but off the calling thread. Spawns a detached worker that grabs
    // the pipeline mutex and closes — so the handler can return its
    // response immediately and the indicator LED goes dark in parallel
    // with the JSON reply hitting the socket.
    void release_camera_async();

    // Kick off ensure_camera_open() off-thread. Called by Server right after
    // accept() so start_stream + a slice of the driver's first-frame wait
    // overlap with read_message + request parsing — the earliest we can
    // start the camera. No-op for warm/idle_keep (they own the lifecycle).
    //
    // Race-free: prewarm captures the "use generation" at spawn and skips
    // opening if a request has closed the camera in the meantime — so a
    // delayed prewarm can never re-open a stream the request already shut,
    // which is what used to leave the LED lit ~1-2% of the time.
    void prewarm_async();

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

    // RAII helper for handler request bodies. On "lazy" policy its destructor
    // closes the camera; for "warm" / "idle_keep" it's a no-op. Declare AFTER
    // the lock_guard on mutex() so unwinding releases camera before mutex.
    struct RequestScope {
        Pipeline& p;
        ~RequestScope() {
            // Async on lazy — handler returns the response first; the close
            // happens off-thread so STREAMOFF/munmap/close don't delay
            // write_message on the client socket.
            if (p.cfg_.camera_policy == "lazy") p.release_camera_async();
        }
    };
    [[nodiscard]] RequestScope request_scope() noexcept { return RequestScope{*this}; }

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

    // Bumped on every camera close. prewarm_async() captures it at spawn and
    // bails if it changed before the (delayed) open runs — the guard that
    // makes early prewarm safe against re-opening a just-closed stream.
    std::atomic<uint64_t>        use_gen_{0};

    // Background watchdog. For "idle_keep" it closes the camera after
    // idle_keep_ms of inactivity; for "lazy" it's a safety net closing the
    // stream lazy_safety_close_ms after last use if it's somehow still on.
    // Window is resolved once in the ctor.
    std::chrono::milliseconds    watchdog_window_{0};
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

} // namespace chowdy::daemon
