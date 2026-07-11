// M2 detection + embedding test for chowdy.
//
// Loads the SCRFD-500MF detector and the MobileFaceNet embedder, runs them
// on a single PGM IR frame produced by M1 (tools/m1_capture_test.cpp),
// prints the detected face boxes, keypoints, embedding norm, and per-step
// latency. Throwaway scaffolding for milestone validation — the production
// daemon will replace this with proper Detector/Embedder classes in
// daemon/models.cpp.
//
// SCRFD output decoding follows the canonical insightface scheme: nine
// outputs (scores/bboxes/kps × strides {8, 16, 32}), two anchors per cell,
// distance-to-bbox decoding. Reference:
//   github.com/deepinsight/insightface/blob/master/python-package/insightface/model_zoo/scrfd.py
//
// Usage:
//   chowdy-detect-test [detector.onnx] [embedder.onnx] [input.pgm]
//   defaults: models/scrfd_500m_bnkps.onnx, models/w600k_mbf.onnx,
//             /tmp/chowdy-frame-00.pgm

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include <opencv2/core/version.hpp>
#if CV_VERSION_MAJOR >= 5
#  include <opencv2/geometry.hpp>   // estimateAffinePartial2D moved here in OpenCV 5
#else
#  include <opencv2/calib3d.hpp>
#endif
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <onnxruntime_cxx_api.h>

namespace {

using clock_type = std::chrono::steady_clock;
double ms_since(clock_type::time_point t0) {
    return std::chrono::duration<double, std::milli>(clock_type::now() - t0).count();
}

constexpr int   kInputSize     = 640;
constexpr float kConfThr       = 0.5f;
constexpr float kNmsIou        = 0.4f;
constexpr int   kEmbedSize     = 112;
constexpr std::array<int, 3> kStrides{8, 16, 32};
constexpr int   kAnchorsPerCell = 2;

struct Detection {
    cv::Rect2f                box;
    std::array<cv::Point2f, 5> kps{};
    float                       score = 0.f;
};

// Letterbox an HxW greyscale image into a 640x640 RGB tensor (NCHW, fp32),
// preserving aspect. Returns scale used (input_px = orig_px * scale) and
// (pad_x, pad_y) added before scaling so we can map detections back.
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

    // NCHW fp32, normalize with insightface's (px-127.5)/128.
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

// Look up SCRFD outputs by shape. SCRFD-bnkps has 9 outputs:
//   3 × scores  ([1, N, 1])
//   3 × bboxes  ([1, N, 4])
//   3 × kps     ([1, N, 10])
// where N = (640/stride)^2 * 2. ORT output ordering varies by export, so
// pair them up by their last dim.
struct ScrfdOutputs {
    std::array<const float*, 3> scores{};
    std::array<const float*, 3> bboxes{};
    std::array<const float*, 3> kpss{};
};

ScrfdOutputs categorize_outputs(const std::vector<Ort::Value>& outs) {
    ScrfdOutputs result{};
    constexpr size_t kInvalid = static_cast<size_t>(-1);
    std::array<size_t, 3> score_idx{kInvalid, kInvalid, kInvalid};
    std::array<size_t, 3> bbox_idx {kInvalid, kInvalid, kInvalid};
    std::array<size_t, 3> kps_idx  {kInvalid, kInvalid, kInvalid};

    for (size_t i = 0; i < outs.size(); ++i) {
        auto info  = outs[i].GetTensorTypeAndShapeInfo();
        auto shape = info.GetShape();
        // SCRFD outputs from insightface ONNX are rank-2 [N, K] (no batch dim);
        // accept rank-3 [1, N, K] too just in case of a re-exported variant.
        int64_t n = -1, last = -1;
        if      (shape.size() == 2) { n = shape[0]; last = shape[1]; }
        else if (shape.size() == 3 && shape[0] == 1) { n = shape[1]; last = shape[2]; }
        else continue;
        // Map N back to stride: N = (640/stride)^2 * 2.
        size_t stride_idx = kInvalid;
        if      (n == 80 * 80 * kAnchorsPerCell) stride_idx = 0;
        else if (n == 40 * 40 * kAnchorsPerCell) stride_idx = 1;
        else if (n == 20 * 20 * kAnchorsPerCell) stride_idx = 2;
        if (stride_idx == kInvalid) continue;

        if      (last == 1)  score_idx[stride_idx] = i;
        else if (last == 4)  bbox_idx [stride_idx] = i;
        else if (last == 10) kps_idx  [stride_idx] = i;
    }

    for (size_t s = 0; s < 3; ++s) {
        if (score_idx[s] == kInvalid || bbox_idx[s] == kInvalid || kps_idx[s] == kInvalid) {
            std::cerr << "fatal: SCRFD output layout not understood (stride idx "
                      << s << ")\n";
            std::exit(1);
        }
        result.scores[s] = outs[score_idx[s]].GetTensorData<float>();
        result.bboxes[s] = outs[bbox_idx [s]].GetTensorData<float>();
        result.kpss  [s] = outs[kps_idx  [s]].GetTensorData<float>();
    }
    return result;
}

std::vector<Detection> decode_scrfd(const ScrfdOutputs& outs,
                                    float scale, int pad_x, int pad_y) {
    std::vector<Detection> dets;
    dets.reserve(64);

    const float fpad_x = static_cast<float>(pad_x);
    const float fpad_y = static_cast<float>(pad_y);
    auto remap_x = [&](float v) { return (v - fpad_x) / scale; };
    auto remap_y = [&](float v) { return (v - fpad_y) / scale; };

    for (size_t si = 0; si < 3; ++si) {
        const int    stride  = kStrides[si];
        const float  fstride = static_cast<float>(stride);
        const size_t side    = static_cast<size_t>(kInputSize / stride);
        const float* sc      = outs.scores[si];
        const float* bb      = outs.bboxes[si];
        const float* kp      = outs.kpss  [si];

        for (size_t y = 0; y < side; ++y) {
            for (size_t x = 0; x < side; ++x) {
                for (size_t a = 0; a < kAnchorsPerCell; ++a) {
                    const size_t idx   = (y * side + x) * kAnchorsPerCell + a;
                    const float  score = sc[idx];
                    if (score < kConfThr) continue;

                    // Anchor center in input pixel coords.
                    const float cx = (static_cast<float>(x) + 0.5f) * fstride;
                    const float cy = (static_cast<float>(y) + 0.5f) * fstride;

                    // distance2bbox: outputs are distances (in stride units) from
                    // anchor center to bbox edges (left, top, right, bottom).
                    const float l = bb[idx * 4 + 0] * fstride;
                    const float t = bb[idx * 4 + 1] * fstride;
                    const float r = bb[idx * 4 + 2] * fstride;
                    const float b = bb[idx * 4 + 3] * fstride;

                    Detection d;
                    d.score = score;
                    // Map back to original image coordinates.
                    d.box = cv::Rect2f(
                        cv::Point2f(remap_x(cx - l), remap_y(cy - t)),
                        cv::Point2f(remap_x(cx + r), remap_y(cy + b)));

                    for (size_t k = 0; k < 5; ++k) {
                        const float kx = kp[idx * 10 + k * 2 + 0] * fstride + cx;
                        const float ky = kp[idx * 10 + k * 2 + 1] * fstride + cy;
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
            if (!dropped[j] && iou(dets[i].box, dets[j].box) > iou_thr) {
                dropped[j] = 1;
            }
        }
    }
    return keep;
}

// Insightface canonical reference landmarks for a 112x112 aligned face.
// Source: insightface/recognition/arcface_torch/utils/utils_arcface.py.
// cv::Point2f isn't a literal type so this can't be constexpr; static const
// is the closest equivalent.
const std::array<cv::Point2f, 5> kRefLandmarks{{
    {38.2946f, 51.6963f},   // left eye
    {73.5318f, 51.5014f},   // right eye
    {56.0252f, 71.7366f},   // nose tip
    {41.5493f, 92.3655f},   // left mouth corner
    {70.7299f, 92.2041f},   // right mouth corner
}};

// Align the detected face into a 112x112 chip using a 5-point similarity
// transform (warpAffine with the partial-affine estimator). Replicates the
// preprocessing used by every insightface embedder.
std::vector<float> preprocess_embed_aligned(const cv::Mat& grey,
                                            const std::array<cv::Point2f, 5>& kps) {
    std::vector<cv::Point2f> src(kps.begin(), kps.end());
    std::vector<cv::Point2f> dst(kRefLandmarks.begin(), kRefLandmarks.end());
    cv::Mat M = cv::estimateAffinePartial2D(src, dst, cv::noArray(), cv::LMEDS);
    if (M.empty()) {
        // Fall back to identity if estimator failed — better than crashing.
        M = cv::Mat::eye(2, 3, CV_64F);
    }

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

void print_session_io(const Ort::Session& session, const char* label) {
    Ort::AllocatorWithDefaultOptions alloc;
    std::cout << "  [" << label << "] " << session.GetInputCount() << " input(s):\n";
    for (size_t i = 0; i < session.GetInputCount(); ++i) {
        auto name  = session.GetInputNameAllocated(i, alloc);
        auto info  = session.GetInputTypeInfo(i).GetTensorTypeAndShapeInfo();
        auto shape = info.GetShape();
        std::cout << "    in[" << i << "]  name=" << name.get() << "  shape=[";
        for (size_t d = 0; d < shape.size(); ++d) {
            std::cout << shape[d] << (d + 1 < shape.size() ? "," : "");
        }
        std::cout << "]\n";
    }
    std::cout << "  [" << label << "] " << session.GetOutputCount() << " output(s):\n";
    for (size_t i = 0; i < session.GetOutputCount(); ++i) {
        auto name  = session.GetOutputNameAllocated(i, alloc);
        auto info  = session.GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo();
        auto shape = info.GetShape();
        std::cout << "    out[" << i << "] name=" << name.get() << "  shape=[";
        for (size_t d = 0; d < shape.size(); ++d) {
            std::cout << shape[d] << (d + 1 < shape.size() ? "," : "");
        }
        std::cout << "]\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string detector_path =
        (argc > 1) ? argv[1] : "models/scrfd_500m_bnkps.onnx";
    const std::string embedder_path =
        (argc > 2) ? argv[2] : "models/w600k_mbf.onnx";
    const std::string input_path =
        (argc > 3) ? argv[3] : "/tmp/chowdy-frame-00.pgm";

    std::cout << "chowdy-detect-test\n"
              << "  detector: " << detector_path << "\n"
              << "  embedder: " << embedder_path << "\n"
              << "  input:    " << input_path << "\n\n";

    // === ORT setup ===
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "chowdy-m2");
    Ort::SessionOptions sopts;
    sopts.SetIntraOpNumThreads(2);
    sopts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    auto t_det_load = clock_type::now();
    Ort::Session det_session(env, detector_path.c_str(), sopts);
    const double det_load_ms = ms_since(t_det_load);

    auto t_emb_load = clock_type::now();
    Ort::Session emb_session(env, embedder_path.c_str(), sopts);
    const double emb_load_ms = ms_since(t_emb_load);

    std::cout << "[load] detector " << det_load_ms << " ms, embedder "
              << emb_load_ms << " ms\n";
    std::cout << "detector I/O:\n";
    print_session_io(det_session, "scrfd");
    std::cout << "embedder I/O:\n";
    print_session_io(emb_session, "embed");
    std::cout << "\n";

    // === read input ===
    cv::Mat grey = cv::imread(input_path, cv::IMREAD_GRAYSCALE);
    if (grey.empty()) {
        std::cerr << "fatal: cannot read " << input_path << "\n";
        return 1;
    }
    std::cout << "input: " << grey.cols << "x" << grey.rows << " grey\n";

    // === SCRFD ===
    float scale = 1.f; int pad_x = 0; int pad_y = 0;
    auto t_pre = clock_type::now();
    auto det_in = preprocess_scrfd(grey, scale, pad_x, pad_y);
    const double pre_det_ms = ms_since(t_pre);

    Ort::AllocatorWithDefaultOptions alloc;
    auto det_in_name_h  = det_session.GetInputNameAllocated(0, alloc);
    auto det_in_name    = det_in_name_h.get();
    std::vector<const char*> det_in_names{det_in_name};
    std::vector<Ort::AllocatedStringPtr> det_out_name_hs;
    std::vector<const char*> det_out_names;
    for (size_t i = 0; i < det_session.GetOutputCount(); ++i) {
        det_out_name_hs.push_back(det_session.GetOutputNameAllocated(i, alloc));
        det_out_names.push_back(det_out_name_hs.back().get());
    }

    const std::array<int64_t, 4> det_shape{1, 3, kInputSize, kInputSize};
    auto mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value det_tensor = Ort::Value::CreateTensor<float>(
        mem_info, det_in.data(), det_in.size(),
        det_shape.data(), det_shape.size());

    auto t_det_inf = clock_type::now();
    auto det_outs = det_session.Run(Ort::RunOptions{},
                                    det_in_names.data(), &det_tensor, 1,
                                    det_out_names.data(), det_out_names.size());
    const double det_inf_ms = ms_since(t_det_inf);

    auto t_decode = clock_type::now();
    auto sc_outs = categorize_outputs(det_outs);
    auto raw_dets = decode_scrfd(sc_outs, scale, pad_x, pad_y);
    auto dets = nms(std::move(raw_dets), kNmsIou);
    const double decode_ms = ms_since(t_decode);

    // Debug: max raw score per stride, useful to distinguish "no face" from
    // "preproc/decode broken".
    for (int si = 0; si < 3; ++si) {
        const int stride = kStrides[static_cast<size_t>(si)];
        const int side   = kInputSize / stride;
        const int n      = side * side * kAnchorsPerCell;
        const float* sc  = sc_outs.scores[static_cast<size_t>(si)];
        float mx = 0.f;
        for (int j = 0; j < n; ++j) mx = std::max(mx, sc[j]);
        std::cout << "  raw max score stride " << stride << " = " << mx << "\n";
    }

    std::cout << "[detector] preproc=" << pre_det_ms << " ms"
              << "  inference=" << det_inf_ms << " ms"
              << "  decode+NMS=" << decode_ms << " ms"
              << "  → " << dets.size() << " face(s) above conf "
              << kConfThr << "\n";

    for (size_t i = 0; i < dets.size(); ++i) {
        const auto& d = dets[i];
        std::cout << "  face[" << i << "] score=" << d.score
                  << " box=(" << d.box.x << "," << d.box.y
                  << " " << d.box.width << "x" << d.box.height << ")"
                  << "  kps=[";
        for (int k = 0; k < 5; ++k) {
            std::cout << "(" << d.kps[static_cast<size_t>(k)].x << ","
                      << d.kps[static_cast<size_t>(k)].y << ")"
                      << (k < 4 ? " " : "");
        }
        std::cout << "]\n";
    }

    if (dets.empty()) {
        std::cout << "\nno faces detected — skipping embedder.\n";
        return 0;
    }

    // === embedder on best detection (5-point similarity alignment) ===
    const auto& best = dets.front();
    auto t_pre_emb = clock_type::now();
    auto emb_in = preprocess_embed_aligned(grey, best.kps);
    const double pre_emb_ms = ms_since(t_pre_emb);

    auto emb_in_name_h  = emb_session.GetInputNameAllocated(0, alloc);
    auto emb_in_name    = emb_in_name_h.get();
    std::vector<const char*> emb_in_names{emb_in_name};
    std::vector<Ort::AllocatedStringPtr> emb_out_name_hs;
    std::vector<const char*> emb_out_names;
    for (size_t i = 0; i < emb_session.GetOutputCount(); ++i) {
        emb_out_name_hs.push_back(emb_session.GetOutputNameAllocated(i, alloc));
        emb_out_names.push_back(emb_out_name_hs.back().get());
    }
    const std::array<int64_t, 4> emb_shape{1, 3, kEmbedSize, kEmbedSize};
    Ort::Value emb_tensor = Ort::Value::CreateTensor<float>(
        mem_info, emb_in.data(), emb_in.size(),
        emb_shape.data(), emb_shape.size());

    auto t_emb_inf = clock_type::now();
    auto emb_outs = emb_session.Run(Ort::RunOptions{},
                                    emb_in_names.data(), &emb_tensor, 1,
                                    emb_out_names.data(), emb_out_names.size());
    const double emb_inf_ms = ms_since(t_emb_inf);

    auto emb_info  = emb_outs[0].GetTensorTypeAndShapeInfo();
    auto emb_shape_out = emb_info.GetShape();
    const float* emb = emb_outs[0].GetTensorData<float>();
    const size_t emb_dim =
        static_cast<size_t>(std::accumulate(emb_shape_out.begin(), emb_shape_out.end(),
                                            int64_t{1}, std::multiplies<>()));

    double norm_sq = 0.0;
    for (size_t i = 0; i < emb_dim; ++i) norm_sq += static_cast<double>(emb[i]) * emb[i];
    const double l2 = std::sqrt(norm_sq);

    // L2-normalize for cosine similarity — this is the form we will store
    // in the enrollment file (see DESIGN.md §8 file format).
    std::vector<float> normed(emb_dim);
    const float inv_l2 = (l2 > 1e-6) ? static_cast<float>(1.0 / l2) : 0.f;
    for (size_t i = 0; i < emb_dim; ++i) normed[i] = emb[i] * inv_l2;

    std::cout << "[embedder] preproc=" << pre_emb_ms << " ms"
              << "  inference=" << emb_inf_ms << " ms"
              << "  dim=" << emb_dim
              << "  raw L2=" << l2 << "\n"
              << "  raw emb[0..7]=";
    for (int i = 0; i < 8 && static_cast<size_t>(i) < emb_dim; ++i) {
        std::cout << emb[i] << " ";
    }
    std::cout << "\n  L2-normalized emb[0..7]=";
    for (int i = 0; i < 8 && static_cast<size_t>(i) < emb_dim; ++i) {
        std::cout << normed[static_cast<size_t>(i)] << " ";
    }
    std::cout << "\n";

    const double total_pipeline_ms = pre_det_ms + det_inf_ms + decode_ms
                                   + pre_emb_ms + emb_inf_ms;
    std::cout << "\n[total pipeline (single face, single crop)] "
              << total_pipeline_ms << " ms\n";

    return 0;
}
