// ONNX Runtime wrappers for the face detector and embedder.
//
// Both classes own their Ort::Session and pre-resolve input/output names at
// construction so the per-call hot path skips allocator churn. Single-threaded
// inference is assumed; multiple parallel sessions can be created if the
// daemon ever spreads CPU work across cores.

#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <onnxruntime_cxx_api.h>

#include "common/encoding.hpp"

namespace fastauth::daemon {

struct Detection {
    cv::Rect2f                  box;
    std::array<cv::Point2f, 5>  kps{};
    float                       score = 0.f;
};

class Detector {
public:
    explicit Detector(Ort::Env& env, const std::filesystem::path& model_path,
                      int intra_op_threads = 2);

    // Runs SCRFD on the given greyscale frame (any resolution; letterboxed
    // to 640x640 internally). Returns NMS-filtered detections.
    std::vector<Detection> detect(const cv::Mat& grey,
                                  float conf_threshold = 0.5f,
                                  float nms_iou        = 0.4f);

    // Single dummy inference on a zero tensor — pays the ORT JIT cost up
    // front so the first real detect() in production hits the warm path.
    void warmup();

    // Identity tag of the loaded model file — for embedder_id mismatches in
    // enrollment files.
    uint32_t model_id() const noexcept { return model_id_; }

private:
    Ort::Session                       session_;
    Ort::MemoryInfo                    mem_info_;
    std::vector<Ort::AllocatedStringPtr> in_name_holders_;
    std::vector<Ort::AllocatedStringPtr> out_name_holders_;
    std::vector<const char*>           in_names_;
    std::vector<const char*>           out_names_;
    uint32_t                           model_id_ = 0;
};

class Embedder {
public:
    explicit Embedder(Ort::Env& env, const std::filesystem::path& model_path,
                      int intra_op_threads = 2);

    // Aligns the face into 112x112 (5-point similarity) and runs the embedder.
    // Returns an L2-normalised 512-d embedding ready for cosine comparison.
    common::encoding::Embedding embed_aligned(const cv::Mat& grey,
                                              const Detection& det);

    // ORT JIT prewarm — see Detector::warmup().
    void warmup();

    uint32_t model_id() const noexcept { return model_id_; }

private:
    Ort::Session                       session_;
    Ort::MemoryInfo                    mem_info_;
    std::vector<Ort::AllocatedStringPtr> in_name_holders_;
    std::vector<Ort::AllocatedStringPtr> out_name_holders_;
    std::vector<const char*>           in_names_;
    std::vector<const char*>           out_names_;
    uint32_t                           model_id_ = 0;
};

} // namespace fastauth::daemon
