#include "daemon/pipeline.hpp"

#include <algorithm>
#include <cmath>

#include <opencv2/imgproc.hpp>

#include "common/logging.hpp"

namespace fastauth::daemon {

Pipeline::Pipeline(const PipelineConfig& cfg)
    : env_(ORT_LOGGING_LEVEL_WARNING, "fastauthd"),
      cfg_(cfg),
      detector_(env_, cfg.detector_model, cfg.intra_op_threads),
      embedder_(env_, cfg.embedder_model, cfg.intra_op_threads) {
    common::log::info("models loaded",
        {{"detector_id", std::to_string(detector_.model_id())},
         {"embedder_id", std::to_string(embedder_.model_id())}});
}

void Pipeline::ensure_camera_open() {
    if (camera_.is_open()) return;
    camera_.open(cfg_.camera);
    frames_since_open_ = 0;
    common::log::info("camera opened", {{"device", cfg_.camera.device}});
}

void Pipeline::release_camera() {
    if (!camera_.is_open()) return;
    camera_.close();
    frames_since_open_ = 0;
    common::log::info("camera released");
}

uint32_t Pipeline::embedder_model_id() const noexcept {
    return embedder_.model_id();
}

FrameOutcome Pipeline::process_one_frame(std::chrono::milliseconds capture_timeout,
                                         bool run_embedder) {
    FrameOutcome out;
    ensure_camera_open();
    cv::Mat frame = camera_.capture(capture_timeout);
    if (frame.empty()) return out;          // captured=false
    out.captured = true;
    ++frames_since_open_;

    // Throw away the first few frames after open — exposure warmup.
    if (frames_since_open_ <= cfg_.warmup_frames) return out;

    cv::Scalar mu = cv::mean(frame);
    out.brightness = mu[0];
    if (out.brightness < cfg_.dark_threshold) return out;   // emitter off-frame

    auto dets = detector_.detect(frame, cfg_.detector_conf_threshold, cfg_.nms_iou);
    if (dets.empty()) return out;
    out.has_face  = true;
    out.detection = dets.front();
    out.quality   = frame_quality(frame, *out.detection);

    if (run_embedder) {
        out.embedding = embedder_.embed_aligned(frame, *out.detection);
    }
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

} // namespace fastauth::daemon
