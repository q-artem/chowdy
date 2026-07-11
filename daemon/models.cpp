#include "daemon/models.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

#include <opencv2/core/version.hpp>
#if CV_VERSION_MAJOR >= 5
#  include <opencv2/geometry.hpp>   // estimateAffinePartial2D moved here in OpenCV 5
#else
#  include <opencv2/calib3d.hpp>
#endif
#include <opencv2/imgproc.hpp>

namespace chowdy::daemon {

namespace {

// SCRFD-bnkps wire layout: nine outputs, three strides, two anchors per cell.
// See tools/m2_detect_test.cpp and DESIGN.md Appendix A "M2 findings" for the
// reverse-engineering notes; this is the production version.
constexpr int   kInputSize       = 640;
constexpr int   kEmbedSize       = 112;
constexpr std::array<int, 3> kStrides{8, 16, 32};
constexpr int   kAnchorsPerCell  = 2;

const std::array<cv::Point2f, 5> kRefLandmarks{{
    {38.2946f, 51.6963f},
    {73.5318f, 51.5014f},
    {56.0252f, 71.7366f},
    {41.5493f, 92.3655f},
    {70.7299f, 92.2041f},
}};

Ort::SessionOptions make_session_opts(int threads) {
    Ort::SessionOptions o;
    o.SetIntraOpNumThreads(threads);
    o.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    return o;
}

void resolve_io_names(Ort::Session& s,
                      std::vector<Ort::AllocatedStringPtr>& in_h,
                      std::vector<Ort::AllocatedStringPtr>& out_h,
                      std::vector<const char*>& in_names,
                      std::vector<const char*>& out_names) {
    Ort::AllocatorWithDefaultOptions alloc;
    for (size_t i = 0; i < s.GetInputCount(); ++i) {
        in_h.push_back(s.GetInputNameAllocated(i, alloc));
        in_names.push_back(in_h.back().get());
    }
    for (size_t i = 0; i < s.GetOutputCount(); ++i) {
        out_h.push_back(s.GetOutputNameAllocated(i, alloc));
        out_names.push_back(out_h.back().get());
    }
}

std::vector<float> preprocess_scrfd(const cv::Mat& grey,
                                    float& out_scale,
                                    int& out_pad_x,
                                    int& out_pad_y) {
    const float fcols = static_cast<float>(grey.cols);
    const float frows = static_cast<float>(grey.rows);
    const float scale = std::min(static_cast<float>(kInputSize) / fcols,
                                 static_cast<float>(kInputSize) / frows);
    const int new_w = static_cast<int>(fcols * scale);
    const int new_h = static_cast<int>(frows * scale);

    cv::Mat resized;
    cv::resize(grey, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);

    cv::Mat canvas(kInputSize, kInputSize, CV_8UC1, cv::Scalar(0));
    const int pad_x = (kInputSize - new_w) / 2;
    const int pad_y = (kInputSize - new_h) / 2;
    resized.copyTo(canvas(cv::Rect(pad_x, pad_y, new_w, new_h)));

    out_scale = scale;
    out_pad_x = pad_x;
    out_pad_y = pad_y;

    std::vector<float> tensor(static_cast<size_t>(3) * kInputSize * kInputSize);
    const float norm = 1.0f / 128.f;
    for (int c = 0; c < 3; ++c) {
        float* dst = tensor.data() + static_cast<size_t>(c) * kInputSize * kInputSize;
        for (int y = 0; y < kInputSize; ++y) {
            const uint8_t* src = canvas.ptr<uint8_t>(y);
            for (int x = 0; x < kInputSize; ++x) {
                dst[y * kInputSize + x] = (static_cast<float>(src[x]) - 127.5f) * norm;
            }
        }
    }
    return tensor;
}

struct ScrfdHeads {
    std::array<const float*, 3> scores{};
    std::array<const float*, 3> bboxes{};
    std::array<const float*, 3> kpss{};
};

ScrfdHeads categorize_outputs(const std::vector<Ort::Value>& outs) {
    ScrfdHeads result{};
    constexpr size_t kInvalid = static_cast<size_t>(-1);
    std::array<size_t, 3> score_idx{kInvalid, kInvalid, kInvalid};
    std::array<size_t, 3> bbox_idx {kInvalid, kInvalid, kInvalid};
    std::array<size_t, 3> kps_idx  {kInvalid, kInvalid, kInvalid};
    for (size_t i = 0; i < outs.size(); ++i) {
        auto shape = outs[i].GetTensorTypeAndShapeInfo().GetShape();
        int64_t n = -1, last = -1;
        if      (shape.size() == 2) { n = shape[0]; last = shape[1]; }
        else if (shape.size() == 3 && shape[0] == 1) { n = shape[1]; last = shape[2]; }
        else continue;
        size_t si = kInvalid;
        if      (n == 80 * 80 * kAnchorsPerCell) si = 0;
        else if (n == 40 * 40 * kAnchorsPerCell) si = 1;
        else if (n == 20 * 20 * kAnchorsPerCell) si = 2;
        if (si == kInvalid) continue;
        if      (last == 1)  score_idx[si] = i;
        else if (last == 4)  bbox_idx [si] = i;
        else if (last == 10) kps_idx  [si] = i;
    }
    for (size_t s = 0; s < 3; ++s) {
        if (score_idx[s] == kInvalid || bbox_idx[s] == kInvalid || kps_idx[s] == kInvalid)
            throw std::runtime_error("SCRFD output layout not understood");
        result.scores[s] = outs[score_idx[s]].GetTensorData<float>();
        result.bboxes[s] = outs[bbox_idx [s]].GetTensorData<float>();
        result.kpss  [s] = outs[kps_idx  [s]].GetTensorData<float>();
    }
    return result;
}

std::vector<Detection> decode(const ScrfdHeads& outs, float conf_thr,
                              float scale, int pad_x, int pad_y) {
    std::vector<Detection> dets;
    dets.reserve(64);
    const float fpad_x = static_cast<float>(pad_x);
    const float fpad_y = static_cast<float>(pad_y);
    auto remap_x = [&](float v) { return (v - fpad_x) / scale; };
    auto remap_y = [&](float v) { return (v - fpad_y) / scale; };

    for (size_t si = 0; si < 3; ++si) {
        const float fstride = static_cast<float>(kStrides[si]);
        const size_t side   = static_cast<size_t>(kInputSize / kStrides[si]);
        const float* sc = outs.scores[si];
        const float* bb = outs.bboxes[si];
        const float* kp = outs.kpss  [si];
        for (size_t y = 0; y < side; ++y) {
            for (size_t x = 0; x < side; ++x) {
                for (size_t a = 0; a < kAnchorsPerCell; ++a) {
                    const size_t idx = (y * side + x) * kAnchorsPerCell + a;
                    const float score = sc[idx];
                    if (score < conf_thr) continue;
                    const float cx = (static_cast<float>(x) + 0.5f) * fstride;
                    const float cy = (static_cast<float>(y) + 0.5f) * fstride;
                    const float l = bb[idx*4+0] * fstride;
                    const float t = bb[idx*4+1] * fstride;
                    const float r = bb[idx*4+2] * fstride;
                    const float b = bb[idx*4+3] * fstride;
                    Detection d;
                    d.score = score;
                    d.box = cv::Rect2f(
                        cv::Point2f(remap_x(cx - l), remap_y(cy - t)),
                        cv::Point2f(remap_x(cx + r), remap_y(cy + b)));
                    for (size_t k = 0; k < 5; ++k) {
                        const float kx = kp[idx*10 + k*2 + 0] * fstride + cx;
                        const float ky = kp[idx*10 + k*2 + 1] * fstride + cy;
                        d.kps[k] = cv::Point2f(remap_x(kx), remap_y(ky));
                    }
                    dets.push_back(d);
                }
            }
        }
    }
    return dets;
}

float iou(const cv::Rect2f& a, const cv::Rect2f& b) {
    const float ix1 = std::max(a.x, b.x);
    const float iy1 = std::max(a.y, b.y);
    const float ix2 = std::min(a.x + a.width,  b.x + b.width);
    const float iy2 = std::min(a.y + a.height, b.y + b.height);
    const float iw  = std::max(0.f, ix2 - ix1);
    const float ih  = std::max(0.f, iy2 - iy1);
    const float inter = iw * ih;
    const float uni   = a.area() + b.area() - inter;
    return uni > 0.f ? inter / uni : 0.f;
}

std::vector<Detection> nms(std::vector<Detection> dets, float iou_thr) {
    std::sort(dets.begin(), dets.end(),
              [](const Detection& a, const Detection& b) { return a.score > b.score; });
    std::vector<Detection> keep;
    std::vector<char> dropped(dets.size(), 0);
    for (size_t i = 0; i < dets.size(); ++i) {
        if (dropped[i]) continue;
        keep.push_back(dets[i]);
        for (size_t j = i + 1; j < dets.size(); ++j) {
            if (!dropped[j] && iou(dets[i].box, dets[j].box) > iou_thr) dropped[j] = 1;
        }
    }
    return keep;
}

std::vector<float> preprocess_embed_aligned(const cv::Mat& grey,
                                            const std::array<cv::Point2f, 5>& kps) {
    std::vector<cv::Point2f> src(kps.begin(), kps.end());
    std::vector<cv::Point2f> dst(kRefLandmarks.begin(), kRefLandmarks.end());
    cv::Mat M = cv::estimateAffinePartial2D(src, dst, cv::noArray(), cv::LMEDS);
    if (M.empty()) M = cv::Mat::eye(2, 3, CV_64F);

    cv::Mat aligned;
    cv::warpAffine(grey, aligned, M, cv::Size(kEmbedSize, kEmbedSize),
                   cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0));

    std::vector<float> tensor(static_cast<size_t>(3) * kEmbedSize * kEmbedSize);
    const float norm = 1.0f / 127.5f;
    for (int c = 0; c < 3; ++c) {
        float* dst_ch = tensor.data() + static_cast<size_t>(c) * kEmbedSize * kEmbedSize;
        for (int y = 0; y < kEmbedSize; ++y) {
            const uint8_t* src_row = aligned.ptr<uint8_t>(y);
            for (int x = 0; x < kEmbedSize; ++x) {
                dst_ch[y * kEmbedSize + x] = (static_cast<float>(src_row[x]) - 127.5f) * norm;
            }
        }
    }
    return tensor;
}

} // namespace

// === Detector ===

Detector::Detector(Ort::Env& env, const std::filesystem::path& path, int threads)
    : session_(env, path.c_str(), make_session_opts(threads)),
      mem_info_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)) {
    resolve_io_names(session_, in_name_holders_, out_name_holders_, in_names_, out_names_);
    model_id_ = common::encoding::simple_model_id(path);
}

void Detector::warmup() {
    // Single Run on a zero tensor — same shape SCRFD expects in production.
    // Output is discarded; we only care about ORT internal JIT/specialisation
    // happening here instead of on the first real frame.
    std::vector<float> zeros(static_cast<size_t>(3) * kInputSize * kInputSize, 0.f);
    const std::array<int64_t, 4> shape{1, 3, kInputSize, kInputSize};
    Ort::Value t = Ort::Value::CreateTensor<float>(
        mem_info_, zeros.data(), zeros.size(), shape.data(), shape.size());
    session_.Run(Ort::RunOptions{}, in_names_.data(), &t, 1,
                 out_names_.data(), out_names_.size());
}

std::vector<Detection> Detector::detect(const cv::Mat& grey,
                                        float conf_threshold, float nms_iou) {
    float scale = 1.f; int pad_x = 0; int pad_y = 0;
    auto in = preprocess_scrfd(grey, scale, pad_x, pad_y);
    const std::array<int64_t, 4> shape{1, 3, kInputSize, kInputSize};
    Ort::Value t = Ort::Value::CreateTensor<float>(
        mem_info_, in.data(), in.size(), shape.data(), shape.size());
    auto outs = session_.Run(Ort::RunOptions{}, in_names_.data(), &t, 1,
                             out_names_.data(), out_names_.size());
    auto heads = categorize_outputs(outs);
    auto raw = decode(heads, conf_threshold, scale, pad_x, pad_y);
    return nms(std::move(raw), nms_iou);
}

// === Embedder ===

Embedder::Embedder(Ort::Env& env, const std::filesystem::path& path, int threads)
    : session_(env, path.c_str(), make_session_opts(threads)),
      mem_info_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)) {
    resolve_io_names(session_, in_name_holders_, out_name_holders_, in_names_, out_names_);
    model_id_ = common::encoding::simple_model_id(path);
}

void Embedder::warmup() {
    std::vector<float> zeros(static_cast<size_t>(3) * kEmbedSize * kEmbedSize, 0.f);
    const std::array<int64_t, 4> shape{1, 3, kEmbedSize, kEmbedSize};
    Ort::Value t = Ort::Value::CreateTensor<float>(
        mem_info_, zeros.data(), zeros.size(), shape.data(), shape.size());
    session_.Run(Ort::RunOptions{}, in_names_.data(), &t, 1,
                 out_names_.data(), out_names_.size());
}

common::encoding::Embedding Embedder::embed_aligned(const cv::Mat& grey,
                                                    const Detection& det) {
    auto in = preprocess_embed_aligned(grey, det.kps);
    const std::array<int64_t, 4> shape{1, 3, kEmbedSize, kEmbedSize};
    Ort::Value t = Ort::Value::CreateTensor<float>(
        mem_info_, in.data(), in.size(), shape.data(), shape.size());
    auto outs = session_.Run(Ort::RunOptions{}, in_names_.data(), &t, 1,
                             out_names_.data(), out_names_.size());
    auto out_shape = outs[0].GetTensorTypeAndShapeInfo().GetShape();
    const size_t dim = static_cast<size_t>(
        std::accumulate(out_shape.begin(), out_shape.end(), int64_t{1}, std::multiplies<>()));
    if (dim != common::encoding::kEmbDim)
        throw std::runtime_error("embedder output dim != 512");
    const float* p = outs[0].GetTensorData<float>();
    common::encoding::Embedding emb(p, p + dim);
    common::encoding::l2_normalize(emb);
    return emb;
}

} // namespace chowdy::daemon
