// M3 end-to-end pipeline test for chowdy.
//
// Live camera loop: open /dev/video2 → on each frame run SCRFD → align →
// MobileFaceNet → cosine-match against a stored enrollment, exit success
// when the match passes threshold. Two modes:
//
//   --enroll [-n N]    capture N quality-filtered frames, store as
//                       ~/.cache/chowdy/test_embedding.bin (FA01 format,
//                       see DESIGN.md §8).
//   --auth (default)   load enrollment, loop until match or timeout.
//
// Throwaway scaffolding — V4L2, ORT and OpenCV are inlined here. They get
// split into proper daemon/camera.cpp, daemon/models.cpp,
// daemon/pipeline.cpp and common/encoding.cpp on M4-M5.
//
// Usage:
//   chowdy-pipeline-test --enroll [-n 8]
//   chowdy-pipeline-test [--auth]
//   options: --device /dev/video2, --models models/,
//            --emb-file ~/.cache/chowdy/test_embedding.bin,
//            --timeout-ms 2000

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include <opencv2/core/version.hpp>
#if CV_VERSION_MAJOR >= 5
#  include <opencv2/geometry.hpp>   // estimateAffinePartial2D moved here in OpenCV 5
#else
#  include <opencv2/calib3d.hpp>
#endif
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <onnxruntime_cxx_api.h>

namespace {

using clock_type = std::chrono::steady_clock;
double ms_since(clock_type::time_point t0) {
    return std::chrono::duration<double, std::milli>(clock_type::now() - t0).count();
}

// === camera constants ===
constexpr int    kCamWidth   = 640;
constexpr int    kCamHeight  = 360;
constexpr int    kCamFps     = 30;
constexpr size_t kCamBuffers = 4;
constexpr int    kCamWarmup  = 3;

// === SCRFD constants (same as m2) ===
constexpr int   kInputSize      = 640;
constexpr float kConfThr        = 0.5f;
constexpr float kNmsIou         = 0.4f;
constexpr int   kEmbedSize      = 112;
constexpr std::array<int, 3> kStrides{8, 16, 32};
constexpr int   kAnchorsPerCell = 2;

// === recognition constants ===
constexpr float kSimFloor    = 0.40f;  // global lower bound on threshold
constexpr float kSimMargin   = 0.05f;  // subtracted from min-pairwise sim
constexpr int   kEnrollDefault = 8;
constexpr int   kEnrollMaxAttempts = 90; // up to ~6 s at 15 fps
// Sharpness on the IR sensor is consistently low (Laplacian variance ~20-50),
// so the multiplicative quality stays ~0.10-0.25 on good frames. M3 finding —
// the original 0.25 in DESIGN.md §7 is too aggressive for this camera.
constexpr float kEnrollQualityMin = 0.10f;
constexpr uint32_t kFileMagic = 0x31304146u; // "FA01" little-endian
constexpr uint16_t kFileVersion = 1;

// === V4L2 helpers ===
struct MmapBuf { void* start = nullptr; size_t length = 0; };

int xioctl(int fd, unsigned long req, void* arg) {
    int r = 0;
    do { r = ::ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

class Camera {
public:
    void open(const std::string& device) {
        device_ = device;
        fd_ = ::open(device.c_str(), O_RDWR | O_NONBLOCK, 0);
        if (fd_ < 0) throw std::runtime_error("open " + device + ": " + std::strerror(errno));

        v4l2_capability cap{};
        if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0)
            throw std::runtime_error("VIDIOC_QUERYCAP");

        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width       = kCamWidth;
        fmt.fmt.pix.height      = kCamHeight;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_GREY;
        fmt.fmt.pix.field       = V4L2_FIELD_NONE;
        if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) throw std::runtime_error("VIDIOC_S_FMT");

        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator   = 1;
        parm.parm.capture.timeperframe.denominator = kCamFps;
        xioctl(fd_, VIDIOC_S_PARM, &parm); // best effort

        v4l2_requestbuffers req{};
        req.count = kCamBuffers;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) throw std::runtime_error("VIDIOC_REQBUFS");
        buffers_.resize(req.count);
        for (size_t i = 0; i < req.count; ++i) {
            v4l2_buffer b{};
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = static_cast<__u32>(i);
            if (xioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) throw std::runtime_error("VIDIOC_QUERYBUF");
            buffers_[i].length = b.length;
            buffers_[i].start = ::mmap(nullptr, b.length, PROT_READ | PROT_WRITE,
                                       MAP_SHARED, fd_, static_cast<off_t>(b.m.offset));
            if (buffers_[i].start == MAP_FAILED) throw std::runtime_error("mmap");
        }
        for (size_t i = 0; i < buffers_.size(); ++i) {
            v4l2_buffer b{};
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = static_cast<__u32>(i);
            if (xioctl(fd_, VIDIOC_QBUF, &b) < 0) throw std::runtime_error("VIDIOC_QBUF init");
        }
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) throw std::runtime_error("VIDIOC_STREAMON");
    }

    // Wait for and return one greyscale frame as a freshly-owned cv::Mat
    // (the mmap buffer is recycled before returning). Returns empty Mat on
    // timeout.
    cv::Mat capture(int timeout_ms) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd_, &fds);
        timeval tv{};
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int r = ::select(fd_ + 1, &fds, nullptr, nullptr, &tv);
        if (r <= 0) return {};

        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_DQBUF, &b) < 0) throw std::runtime_error("VIDIOC_DQBUF");

        cv::Mat view(kCamHeight, kCamWidth, CV_8UC1, buffers_[b.index].start);
        cv::Mat copy = view.clone();

        if (xioctl(fd_, VIDIOC_QBUF, &b) < 0) throw std::runtime_error("VIDIOC_QBUF recycle");
        return copy;
    }

    void close() {
        if (fd_ < 0) return;
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
        for (auto& b : buffers_) {
            if (b.start && b.start != MAP_FAILED) ::munmap(b.start, b.length);
        }
        buffers_.clear();
        ::close(fd_);
        fd_ = -1;
    }

    ~Camera() { close(); }

private:
    std::string device_;
    int fd_ = -1;
    std::vector<MmapBuf> buffers_;
};

// === SCRFD pipeline (same as m2) ===
struct Detection {
    cv::Rect2f                 box;
    std::array<cv::Point2f, 5> kps{};
    float                       score = 0.f;
};

struct ScrfdOutputs {
    std::array<const float*, 3> scores{};
    std::array<const float*, 3> bboxes{};
    std::array<const float*, 3> kpss{};
};

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

ScrfdOutputs categorize_outputs(const std::vector<Ort::Value>& outs) {
    ScrfdOutputs result{};
    constexpr size_t kInvalid = static_cast<size_t>(-1);
    std::array<size_t, 3> score_idx{kInvalid, kInvalid, kInvalid};
    std::array<size_t, 3> bbox_idx {kInvalid, kInvalid, kInvalid};
    std::array<size_t, 3> kps_idx  {kInvalid, kInvalid, kInvalid};

    for (size_t i = 0; i < outs.size(); ++i) {
        auto info  = outs[i].GetTensorTypeAndShapeInfo();
        auto shape = info.GetShape();
        int64_t n = -1, last = -1;
        if      (shape.size() == 2) { n = shape[0]; last = shape[1]; }
        else if (shape.size() == 3 && shape[0] == 1) { n = shape[1]; last = shape[2]; }
        else continue;
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
        if (score_idx[s] == kInvalid || bbox_idx[s] == kInvalid || kps_idx[s] == kInvalid)
            throw std::runtime_error("SCRFD output layout not understood");
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
        const float* sc = outs.scores[si];
        const float* bb = outs.bboxes[si];
        const float* kp = outs.kpss  [si];

        for (size_t y = 0; y < side; ++y) {
            for (size_t x = 0; x < side; ++x) {
                for (size_t a = 0; a < kAnchorsPerCell; ++a) {
                    const size_t idx   = (y * side + x) * kAnchorsPerCell + a;
                    const float  score = sc[idx];
                    if (score < kConfThr) continue;
                    const float cx = (static_cast<float>(x) + 0.5f) * fstride;
                    const float cy = (static_cast<float>(y) + 0.5f) * fstride;
                    const float l = bb[idx * 4 + 0] * fstride;
                    const float t = bb[idx * 4 + 1] * fstride;
                    const float r = bb[idx * 4 + 2] * fstride;
                    const float b = bb[idx * 4 + 3] * fstride;

                    Detection d;
                    d.score = score;
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
            if (!dropped[j] && iou(dets[i].box, dets[j].box) > iou_thr) dropped[j] = 1;
        }
    }
    return keep;
}

// === embedder + alignment ===
const std::array<cv::Point2f, 5> kRefLandmarks{{
    {38.2946f, 51.6963f},
    {73.5318f, 51.5014f},
    {56.0252f, 71.7366f},
    {41.5493f, 92.3655f},
    {70.7299f, 92.2041f},
}};

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

// === quality (DESIGN.md §7) ===
float frame_quality(const cv::Mat& grey, const Detection& det) {
    const float h_frac = std::clamp(det.box.height / static_cast<float>(grey.rows) / 0.3f,
                                    0.f, 1.f);
    // Sharpness on the cropped face only — overall-frame Laplacian is dominated by
    // sensor noise and gives ~constant readings.
    cv::Rect r(static_cast<int>(std::floor(det.box.x)),
               static_cast<int>(std::floor(det.box.y)),
               static_cast<int>(std::ceil(det.box.width)),
               static_cast<int>(std::ceil(det.box.height)));
    r &= cv::Rect(0, 0, grey.cols, grey.rows);
    if (r.empty()) return 0.f;
    cv::Mat lap;
    cv::Laplacian(grey(r), lap, CV_64F);
    cv::Scalar mu, sigma;
    cv::meanStdDev(lap, mu, sigma);
    const float sharp = std::clamp(static_cast<float>(sigma[0] * sigma[0]) / 100.f, 0.f, 1.f);

    const float conf = std::clamp(det.score, 0.f, 1.f);
    return conf * h_frac * sharp;
}

// === cosine ===
double cosine_norm(const float* a, const float* b, size_t n) {
    // a and b assumed already L2-normalized; cosine is just the dot product.
    double s = 0;
    for (size_t i = 0; i < n; ++i) s += static_cast<double>(a[i]) * b[i];
    return s;
}

void l2_normalize(std::vector<float>& v) {
    double s = 0;
    for (float x : v) s += static_cast<double>(x) * x;
    const float inv = (s > 1e-12) ? static_cast<float>(1.0 / std::sqrt(s)) : 0.f;
    for (float& x : v) x *= inv;
}

// === enrollment file (FA01 format from DESIGN §8, simplified) ===
struct EnrollmentFile {
    uint32_t embedder_id = 0;
    float    threshold   = 0.f;
    std::vector<std::vector<float>> embeddings;  // each L2-normalized
};

constexpr size_t kEmbDim = 512;

uint32_t simple_embedder_id(const std::string& path) {
    // Cheap identity tag — fold file size and first 16 bytes into a u32.
    // Not crypto, just to detect "you regenerated models" mismatch.
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    f.seekg(0, std::ios::end);
    auto sz = static_cast<uint32_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::array<unsigned char, 16> head{};
    f.read(reinterpret_cast<char*>(head.data()), 16);
    uint32_t h = sz;
    for (auto b : head) h = h * 31u + b;
    return h;
}

void save_enrollment(const std::string& path, const EnrollmentFile& e) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot open " + path + " for write");
    uint32_t magic = kFileMagic;
    uint16_t ver   = kFileVersion;
    uint64_t now   = static_cast<uint64_t>(std::time(nullptr));
    uint32_t n     = static_cast<uint32_t>(e.embeddings.size());
    f.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    f.write(reinterpret_cast<const char*>(&ver),   sizeof(ver));
    f.write(reinterpret_cast<const char*>(&now),   sizeof(now));
    f.write(reinterpret_cast<const char*>(&e.embedder_id), sizeof(e.embedder_id));
    f.write(reinterpret_cast<const char*>(&e.threshold),   sizeof(e.threshold));
    f.write(reinterpret_cast<const char*>(&n),             sizeof(n));
    for (const auto& v : e.embeddings) {
        f.write(reinterpret_cast<const char*>(v.data()),
                static_cast<std::streamsize>(v.size() * sizeof(float)));
    }
}

std::optional<EnrollmentFile> load_enrollment(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    uint32_t magic = 0; uint16_t ver = 0; uint64_t created = 0;
    EnrollmentFile e;
    uint32_t n = 0;
    f.read(reinterpret_cast<char*>(&magic),         sizeof(magic));
    f.read(reinterpret_cast<char*>(&ver),           sizeof(ver));
    f.read(reinterpret_cast<char*>(&created),       sizeof(created));
    f.read(reinterpret_cast<char*>(&e.embedder_id), sizeof(e.embedder_id));
    f.read(reinterpret_cast<char*>(&e.threshold),   sizeof(e.threshold));
    f.read(reinterpret_cast<char*>(&n),             sizeof(n));
    if (!f || magic != kFileMagic || ver != kFileVersion) return std::nullopt;
    e.embeddings.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        e.embeddings[i].resize(kEmbDim);
        f.read(reinterpret_cast<char*>(e.embeddings[i].data()),
               static_cast<std::streamsize>(kEmbDim * sizeof(float)));
    }
    if (!f) return std::nullopt;
    return e;
}

float pick_threshold(const std::vector<std::vector<float>>& embs) {
    if (embs.size() < 2) return kSimFloor;
    double mn = 1.0;
    for (size_t i = 0; i < embs.size(); ++i) {
        for (size_t j = i + 1; j < embs.size(); ++j) {
            double s = cosine_norm(embs[i].data(), embs[j].data(), kEmbDim);
            if (s < mn) mn = s;
        }
    }
    const float candidate = static_cast<float>(mn) - kSimMargin;
    return std::max(kSimFloor, candidate);
}

// === pipeline (detect + embed) — wraps everything from m2 into one call ===
struct PipelineCtx {
    Ort::Session* det = nullptr;
    Ort::Session* emb = nullptr;
    std::vector<Ort::AllocatedStringPtr> det_in_hs, det_out_hs, emb_in_hs, emb_out_hs;
    std::vector<const char*> det_in, det_out, emb_in, emb_out;
};

void prepare_ctx(PipelineCtx& ctx) {
    Ort::AllocatorWithDefaultOptions alloc;
    ctx.det_in_hs.push_back(ctx.det->GetInputNameAllocated(0, alloc));
    ctx.det_in.push_back(ctx.det_in_hs.back().get());
    for (size_t i = 0; i < ctx.det->GetOutputCount(); ++i) {
        ctx.det_out_hs.push_back(ctx.det->GetOutputNameAllocated(i, alloc));
        ctx.det_out.push_back(ctx.det_out_hs.back().get());
    }
    ctx.emb_in_hs.push_back(ctx.emb->GetInputNameAllocated(0, alloc));
    ctx.emb_in.push_back(ctx.emb_in_hs.back().get());
    for (size_t i = 0; i < ctx.emb->GetOutputCount(); ++i) {
        ctx.emb_out_hs.push_back(ctx.emb->GetOutputNameAllocated(i, alloc));
        ctx.emb_out.push_back(ctx.emb_out_hs.back().get());
    }
}

struct FrameResult {
    bool                 has_face = false;
    Detection            det;
    std::vector<float>   embedding; // L2-normalized
    double               brightness = 0.0;
    double               quality = 0.0;
};

FrameResult recognize_frame(PipelineCtx& ctx, const cv::Mat& grey) {
    FrameResult res;
    cv::Scalar mu = cv::mean(grey);
    res.brightness = mu[0];

    auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    float scale = 1.f; int pad_x = 0; int pad_y = 0;
    auto det_in = preprocess_scrfd(grey, scale, pad_x, pad_y);
    const std::array<int64_t, 4> det_shape{1, 3, kInputSize, kInputSize};
    Ort::Value det_t = Ort::Value::CreateTensor<float>(mem, det_in.data(), det_in.size(),
                                                       det_shape.data(), det_shape.size());
    auto outs = ctx.det->Run(Ort::RunOptions{}, ctx.det_in.data(), &det_t, 1,
                             ctx.det_out.data(), ctx.det_out.size());
    auto sc_outs = categorize_outputs(outs);
    auto raw = decode_scrfd(sc_outs, scale, pad_x, pad_y);
    auto dets = nms(std::move(raw), kNmsIou);
    if (dets.empty()) return res;

    res.has_face = true;
    res.det      = dets.front();
    res.quality  = frame_quality(grey, res.det);

    auto emb_in = preprocess_embed_aligned(grey, res.det.kps);
    const std::array<int64_t, 4> emb_shape{1, 3, kEmbedSize, kEmbedSize};
    Ort::Value emb_t = Ort::Value::CreateTensor<float>(mem, emb_in.data(), emb_in.size(),
                                                       emb_shape.data(), emb_shape.size());
    auto eouts = ctx.emb->Run(Ort::RunOptions{}, ctx.emb_in.data(), &emb_t, 1,
                              ctx.emb_out.data(), ctx.emb_out.size());
    auto info = eouts[0].GetTensorTypeAndShapeInfo();
    auto shape = info.GetShape();
    const size_t dim = static_cast<size_t>(
        std::accumulate(shape.begin(), shape.end(), int64_t{1}, std::multiplies<>()));
    const float* p = eouts[0].GetTensorData<float>();
    res.embedding.assign(p, p + dim);
    l2_normalize(res.embedding);
    return res;
}

std::string default_emb_path() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    std::string base;
    if (xdg && *xdg) base = xdg;
    else {
        const char* home = std::getenv("HOME");
        if (!home || !*home) {
            passwd* pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : "/tmp";
        }
        base = std::string(home) + "/.cache";
    }
    return base + "/chowdy/test_embedding.bin";
}

struct Args {
    enum class Mode { Auth, Enroll } mode = Mode::Auth;
    std::string device     = "/dev/video2";
    std::string models_dir = "models";
    std::string emb_path   = default_emb_path();
    int         timeout_ms = 2000;
    int         enroll_n   = kEnrollDefault;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string_view s = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::cerr << "missing value for " << what << "\n"; std::exit(2); }
            return std::string(argv[++i]);
        };
        if      (s == "--enroll")     a.mode = Args::Mode::Enroll;
        else if (s == "--auth")       a.mode = Args::Mode::Auth;
        else if (s == "--device")     a.device     = next("--device");
        else if (s == "--models")     a.models_dir = next("--models");
        else if (s == "--emb-file")   a.emb_path   = next("--emb-file");
        else if (s == "--timeout-ms") a.timeout_ms = std::atoi(next("--timeout-ms").c_str());
        else if (s == "-n")           a.enroll_n   = std::atoi(next("-n").c_str());
        else if (s == "-h" || s == "--help") {
            std::cout << "usage: " << argv[0]
                      << " [--auth|--enroll [-n N]] [--device path] [--models dir]"
                      << " [--emb-file path] [--timeout-ms N]\n";
            std::exit(0);
        }
        else { std::cerr << "unknown arg: " << s << "\n"; std::exit(2); }
    }
    return a;
}

} // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
    const std::string det_path = args.models_dir + "/scrfd_500m_bnkps.onnx";
    const std::string emb_path = args.models_dir + "/w600k_mbf.onnx";

    std::cout << "chowdy-pipeline-test  mode="
              << (args.mode == Args::Mode::Enroll ? "enroll" : "auth")
              << "  device=" << args.device
              << "  emb_file=" << args.emb_path << "\n";

    // === one-time setup ===
    auto t_setup = clock_type::now();
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "chowdy-m3");
    Ort::SessionOptions sopts;
    sopts.SetIntraOpNumThreads(2);
    sopts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    Ort::Session det_session(env, det_path.c_str(), sopts);
    Ort::Session emb_session(env, emb_path.c_str(), sopts);
    PipelineCtx ctx{&det_session, &emb_session, {}, {}, {}, {}, {}, {}, {}, {}};
    prepare_ctx(ctx);
    const double models_ms = ms_since(t_setup);

    auto t_cam = clock_type::now();
    Camera cam;
    cam.open(args.device);
    const double cam_open_ms = ms_since(t_cam);

    std::cout << "[setup] models loaded in " << models_ms << " ms,"
              << " camera opened in " << cam_open_ms << " ms\n\n";

    if (args.mode == Args::Mode::Enroll) {
        std::cout << "ENROLL: looking for " << args.enroll_n
                  << " quality-passing frames. Smile straight at the camera.\n";
        std::vector<std::vector<float>> kept;
        kept.reserve(static_cast<size_t>(args.enroll_n));
        int attempts = 0;
        const auto t_loop = clock_type::now();
        while (static_cast<int>(kept.size()) < args.enroll_n
               && attempts < kEnrollMaxAttempts) {
            cv::Mat frame = cam.capture(args.timeout_ms);
            if (frame.empty()) { std::cerr << "frame timeout\n"; break; }
            ++attempts;
            if (attempts <= kCamWarmup) continue;

            auto t_inf = clock_type::now();
            auto fr = recognize_frame(ctx, frame);
            const double inf_ms = ms_since(t_inf);

            std::cout << "  attempt " << attempts
                      << "  bright=" << fr.brightness
                      << "  pipeline=" << inf_ms << " ms";
            if (!fr.has_face) { std::cout << "  no face\n"; continue; }
            std::cout << "  score=" << fr.det.score
                      << "  quality=" << fr.quality;
            if (fr.quality < kEnrollQualityMin) {
                std::cout << "  → reject (low quality)\n";
                continue;
            }
            kept.push_back(std::move(fr.embedding));
            std::cout << "  → KEPT (" << kept.size() << "/" << args.enroll_n << ")\n";
        }
        const double loop_ms = ms_since(t_loop);
        if (kept.empty()) { std::cerr << "no usable frames collected.\n"; return 1; }

        EnrollmentFile ef;
        ef.embedder_id = simple_embedder_id(emb_path);
        ef.embeddings  = std::move(kept);
        ef.threshold   = pick_threshold(ef.embeddings);
        save_enrollment(args.emb_path, ef);

        std::cout << "\nenrolled " << ef.embeddings.size() << " embeddings in "
                  << loop_ms << " ms, picked threshold=" << ef.threshold
                  << ", saved to " << args.emb_path << "\n";
        return 0;
    }

    // === AUTH mode ===
    auto enrolled = load_enrollment(args.emb_path);
    if (!enrolled) {
        std::cerr << "no enrollment at " << args.emb_path
                  << " — run with --enroll first.\n";
        return 1;
    }
    const uint32_t current_id = simple_embedder_id(emb_path);
    if (enrolled->embedder_id != current_id) {
        std::cerr << "WARNING: embedder id mismatch (" << enrolled->embedder_id
                  << " vs " << current_id << ") — re-enroll if matches look bad.\n";
    }
    std::cout << "AUTH: loaded " << enrolled->embeddings.size()
              << " embeddings, threshold=" << enrolled->threshold << "\n";

    const auto t_loop = clock_type::now();
    int attempts = 0;
    int faces_seen = 0;
    bool success = false;
    double best_sim_overall = -2.0;

    while (ms_since(t_loop) < args.timeout_ms) {
        cv::Mat frame = cam.capture(args.timeout_ms);
        if (frame.empty()) break;
        ++attempts;
        if (attempts <= kCamWarmup) continue;

        auto t_inf = clock_type::now();
        auto fr = recognize_frame(ctx, frame);
        const double inf_ms = ms_since(t_inf);

        std::cout << "  attempt " << attempts
                  << "  bright=" << fr.brightness
                  << "  pipeline=" << inf_ms << " ms";
        if (!fr.has_face) { std::cout << "  no face\n"; continue; }
        ++faces_seen;

        double best_sim = -2.0;
        for (const auto& e : enrolled->embeddings) {
            double s = cosine_norm(fr.embedding.data(), e.data(), kEmbDim);
            if (s > best_sim) best_sim = s;
        }
        if (best_sim > best_sim_overall) best_sim_overall = best_sim;

        std::cout << "  score=" << fr.det.score
                  << "  sim=" << best_sim;
        if (best_sim >= enrolled->threshold) {
            std::cout << "  → MATCH\n";
            success = true;
            break;
        }
        std::cout << "  → below threshold\n";
    }

    const double total_ms = ms_since(t_loop);
    std::cout << "\n[result] " << (success ? "SUCCESS" : "FAIL")
              << "  total=" << total_ms << " ms"
              << "  attempts=" << attempts
              << "  faces=" << faces_seen
              << "  best_sim=" << best_sim_overall << "\n";
    return success ? 0 : 1;
}
