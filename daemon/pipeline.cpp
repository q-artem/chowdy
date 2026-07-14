#include "daemon/pipeline.hpp"

#include <algorithm>
#include <cmath>

#include <opencv2/imgproc.hpp>

#include "common/logging.hpp"

namespace chowdy::daemon {

Pipeline::Pipeline(const PipelineConfig& cfg)
    : env_(ORT_LOGGING_LEVEL_WARNING, "chowdyd"),
      cfg_(cfg),
      detector_(env_, cfg.detector_model, cfg.intra_op_threads),
      embedder_(env_, cfg.embedder_model, cfg.intra_op_threads) {
    common::log::info("models loaded",
        {{"detector_id", std::to_string(detector_.model_id())},
         {"embedder_id", std::to_string(embedder_.model_id())}});

    // ORT often JITs / specialises on the first inference, costing ~40-60 ms.
    // Run a single dummy inference on each model now so the very first real
    // auth doesn't pay that cost.
    auto t_warm = std::chrono::steady_clock::now();
    try {
        detector_.warmup();
        embedder_.warmup();
        common::log::info("models warmed up",
            {{"ms", std::to_string(
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t_warm).count())}});
    } catch (const std::exception& e) {
        common::log::warn("model warmup failed", {{"err", e.what()}});
    }

    if (cfg_.camera_policy == "warm") {
        // Eager open — the very first auth pays nothing for cold open.
        try { ensure_camera_open(); }
        catch (const std::exception& e) {
            common::log::warn("warm open failed; will retry on first request",
                {{"err", e.what()}});
        }
    }
    // Start the watchdog. For idle_keep it's the primary close mechanism
    // (after idle_keep_ms); for lazy it's a safety net (after
    // lazy_safety_close_ms) backstopping the immediate per-request close.
    if (cfg_.camera_policy == "idle_keep" && cfg_.idle_keep_ms > 0) {
        watchdog_window_ = std::chrono::milliseconds(cfg_.idle_keep_ms);
        idle_thread_ = std::thread(&Pipeline::idle_keeper_loop, this);
    } else if (cfg_.camera_policy == "lazy" && cfg_.lazy_safety_close_ms > 0) {
        watchdog_window_ = std::chrono::milliseconds(cfg_.lazy_safety_close_ms);
        idle_thread_ = std::thread(&Pipeline::idle_keeper_loop, this);
    }
}

Pipeline::~Pipeline() {
    idle_stop_.store(true);
    idle_cv_.notify_all();
    if (idle_thread_.joinable()) idle_thread_.join();
    release_camera();
}

void Pipeline::ensure_camera_open() {
    // Two-tier bring-up:
    //   - if camera was never opened, do the full open() (S_FMT+mmap+...);
    //   - else, if it was opened but stream was stopped (lazy idle), just
    //     restart the stream — cheap (~10ms) compared to full open (~100ms).
    if (!camera_.is_open()) {
        camera_.open(cfg_.camera);
        frames_since_open_ = 0;
        common::log::info("camera opened", {{"device", cfg_.camera.device}});
    } else if (!camera_.is_streaming()) {
        camera_.start_stream();
        frames_since_open_ = 0;
        common::log::info("camera stream resumed");
    }
    // Mark activity on open too (not just process_one_frame) so the watchdog
    // can reclaim a stream that was opened — e.g. by a speculative prewarm —
    // but never actually used.
    last_use_ns_.store(std::chrono::steady_clock::now().time_since_epoch().count());
}

void Pipeline::release_camera() {
    // For the lazy/idle path we keep the fd + mmap buffers alive and just
    // stop the stream — the LED is driven by STREAMON state on UVC, so it
    // still goes dark. Full teardown happens in ~Pipeline().
    if (!camera_.is_streaming()) return;
    camera_.stop_stream();
    frames_since_open_ = 0;
    // Mark that a close happened so a delayed prewarm won't re-open (see
    // prewarm_async). Only bumped on an actual STREAMOFF.
    use_gen_.fetch_add(1, std::memory_order_relaxed);
    common::log::info("camera stream stopped");
}

void Pipeline::prewarm_async() {
    if (cfg_.camera_policy != "lazy") return;
    // Snapshot the close-generation NOW, on the accept thread, before the
    // request's handler runs. The detached opener below only opens if no
    // close happened in between — so a prewarm that loses the mutex race and
    // runs late can never re-open a stream the request already closed (the
    // old ~1-2% "LED stays lit" leak).
    const uint64_t gen0 = use_gen_.load(std::memory_order_relaxed);
    std::thread([this, gen0]{
        std::lock_guard<std::mutex> g(mu_);
        if (use_gen_.load(std::memory_order_relaxed) != gen0) return;  // stale — a close happened
        try { ensure_camera_open(); }
        catch (const std::exception& e) {
            common::log::warn("prewarm failed", {{"err", e.what()}});
        }
    }).detach();
}

void Pipeline::release_camera_async() {
    // Detached worker — short-lived, takes the pipeline mutex so it can't
    // race with another request grabbing process_one_frame in between.
    // If a concurrent request arrives first and resumes streaming, this
    // stop still runs after that request finishes (mutex serialises) and
    // just leaves the stream stopped. Next request resumes. Single-user
    // case never sees the contention.
    std::thread([this] {
        std::lock_guard<std::mutex> g(mu_);
        release_camera();
    }).detach();
}

void Pipeline::idle_keeper_loop() {
    using clock = std::chrono::steady_clock;
    const auto window = watchdog_window_;
    const bool is_safety_net = (cfg_.camera_policy == "lazy");
    while (!idle_stop_.load()) {
        std::unique_lock<std::mutex> lk(idle_cv_mu_);
        // Wake at half the window so the closing reaction is at most 1.5× it.
        idle_cv_.wait_for(lk, window / 2, [&]{ return idle_stop_.load(); });
        if (idle_stop_.load()) break;

        auto last_ns = last_use_ns_.load();
        if (last_ns == 0) continue;
        auto last_tp = clock::time_point(clock::duration(last_ns));
        if (clock::now() - last_tp >= window) {
            std::lock_guard<std::mutex> mu(mu_);
            // Only act if the stream is actually on ("is the LED lit?"). In
            // lazy mode the per-request scope has normally already closed it,
            // so this fires only when something left it streaming.
            if (camera_.is_streaming()) {
                common::log::info(
                    is_safety_net ? "watchdog: force-closing leaked camera stream"
                                  : "idle_keep: stopping stream",
                    {{"after_ms", std::to_string(window.count())}});
                camera_.stop_stream();
                frames_since_open_ = 0;
                use_gen_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

uint32_t Pipeline::embedder_model_id() const noexcept {
    return embedder_.model_id();
}

FrameOutcome Pipeline::process_one_frame(std::chrono::milliseconds capture_timeout,
                                         bool run_embedder) {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    auto dt_ms = [](clock::time_point a, clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    FrameOutcome out;
    const bool was_open = camera_.is_open();
    ensure_camera_open();
    auto t_open = clock::now();

    last_use_ns_.store(clock::now().time_since_epoch().count());
    cv::Mat frame = camera_.capture(capture_timeout);
    auto t_cap = clock::now();
    if (frame.empty()) {
        common::log::debug("frame.timing",
            {{"open_ms",    std::to_string(dt_ms(t0, t_open))},
             {"capture_ms", std::to_string(dt_ms(t_open, t_cap))},
             {"result",     "no_frame"}});
        return out;          // captured=false
    }
    out.captured = true;
    ++frames_since_open_;

    if (frames_since_open_ <= cfg_.warmup_frames) {
        common::log::debug("frame.timing",
            {{"open_ms",    std::to_string(dt_ms(t0, t_open))},
             {"capture_ms", std::to_string(dt_ms(t_open, t_cap))},
             {"result",     "warmup"},
             {"frame_n",    std::to_string(frames_since_open_)}});
        return out;
    }

    cv::Scalar mu = cv::mean(frame);
    out.brightness = mu[0];
    auto t_bright = clock::now();
    if (out.brightness < cfg_.dark_threshold) {
        common::log::debug("frame.timing",
            {{"open_ms",    std::to_string(dt_ms(t0, t_open))},
             {"capture_ms", std::to_string(dt_ms(t_open, t_cap))},
             {"result",     "dark"},
             {"brightness", std::to_string(out.brightness)}});
        return out;
    }

    auto dets = detector_.detect(frame, cfg_.detector_conf_threshold, cfg_.nms_iou);
    auto t_det = clock::now();
    if (dets.empty()) {
        common::log::debug("frame.timing",
            {{"open_ms",    std::to_string(dt_ms(t0, t_open))},
             {"capture_ms", std::to_string(dt_ms(t_open, t_cap))},
             {"detect_ms",  std::to_string(dt_ms(t_bright, t_det))},
             {"result",     "no_face"}});
        return out;
    }
    out.has_face  = true;
    out.detection = dets.front();
    out.quality   = frame_quality(frame, *out.detection);

    double embed_ms = 0;
    if (run_embedder) {
        auto t_eb = clock::now();
        out.embedding = embedder_.embed_aligned(frame, *out.detection);
        embed_ms = dt_ms(t_eb, clock::now());
    }
    common::log::debug("frame.timing",
        {{"open_ms",    std::to_string(was_open ? 0.0 : dt_ms(t0, t_open))},
         {"capture_ms", std::to_string(dt_ms(t_open, t_cap))},
         {"detect_ms",  std::to_string(dt_ms(t_bright, t_det))},
         {"embed_ms",   std::to_string(embed_ms)},
         {"score",      std::to_string(out.detection->score)},
         {"result",     "face"}});
    return out;
}

double frame_quality(const cv::Mat& grey, const Detection& det, double sharpness_divisor) {
    const double h_frac = std::clamp(static_cast<double>(det.box.height)
                                     / static_cast<double>(grey.rows) / 0.3, 0.0, 1.0);

    cv::Rect r(static_cast<int>(std::floor(det.box.x)),
               static_cast<int>(std::floor(det.box.y)),
               static_cast<int>(std::ceil(det.box.width)),
               static_cast<int>(std::ceil(det.box.height)));
    r &= cv::Rect(0, 0, grey.cols, grey.rows);
    double sharp = 0.0;
    if (!r.empty()) {
        cv::Mat lap;
        cv::Laplacian(grey(r), lap, CV_64F);
        cv::Scalar muL, sigmaL;
        cv::meanStdDev(lap, muL, sigmaL);
        sharp = std::clamp(sigmaL[0] * sigmaL[0] / sharpness_divisor, 0.0, 1.0);
    }
    const double conf = std::clamp(static_cast<double>(det.score), 0.0, 1.0);
    return conf * h_frac * sharp;
}

} // namespace chowdy::daemon
