// M1 capture test for chowdy.
//
// Opens an IR V4L2 device, requests GREY 640x360, streams frames via mmap,
// drops the first few warmup frames, then writes the rest as PGM files
// to /tmp/. Prints per-step latency so we can see where the cold-open cost
// actually lives on the target hardware (ASUS ZenBook 13 Flip, 13d3:56eb).
//
// This is throwaway scaffolding — it intentionally has no project deps
// (no OpenCV, no third-party libs). The production daemon will replace
// all of this with a proper Camera class in daemon/camera.cpp.
//
// Usage:
//   chowdy-capture-test [device] [num-frames]
//   device:     default /dev/video2
//   num-frames: default 10 (3 of which are silently dropped as warmup)

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/videodev2.h>

namespace {

using clock_type = std::chrono::steady_clock;

constexpr int    kWidth         = 640;
constexpr int    kHeight        = 360;
constexpr int    kFpsNumerator  = 1;
constexpr int    kFpsDenom      = 30;
constexpr size_t kNumBuffers    = 4;
constexpr int    kWarmupFrames  = 3;

struct MmapBuffer {
    void*  start  = nullptr;
    size_t length = 0;
};

double ms_since(clock_type::time_point t0) {
    auto dt = clock_type::now() - t0;
    return std::chrono::duration<double, std::milli>(dt).count();
}

// xioctl: retry on EINTR, like every V4L2 example since 2003.
int xioctl(int fd, unsigned long req, void* arg) {
    int r = 0;
    do {
        r = ::ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

[[noreturn]] void die(std::string_view what) {
    std::cerr << "fatal: " << what << ": " << std::strerror(errno) << "\n";
    std::exit(1);
}

void write_pgm(const std::string& path, const uint8_t* data, int w, int h) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "warn: cannot open " << path << " for write\n";
        return;
    }
    out << "P5\n" << w << " " << h << "\n255\n";
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(w) * h);
}

} // namespace

int main(int argc, char** argv) {
    const std::string device =
        (argc > 1) ? argv[1] : "/dev/video2";
    const int num_frames =
        (argc > 2) ? std::atoi(argv[2]) : 10;

    std::cout << "chowdy-capture-test\n"
              << "  device:      " << device << "\n"
              << "  frames:      " << num_frames << " (drop first " << kWarmupFrames << ")\n"
              << "  format:      GREY " << kWidth << "x" << kHeight
              << " @ " << kFpsDenom << " fps\n\n";

    auto t_open = clock_type::now();
    int fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) die("open(" + device + ")");
    std::cout << "[+" << ms_since(t_open) << " ms] open\n";

    // QUERYCAP — sanity check this is a streaming capture device.
    {
        v4l2_capability cap{};
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) die("VIDIOC_QUERYCAP");
        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
            !(cap.capabilities & V4L2_CAP_STREAMING)) {
            std::cerr << "device does not support video capture + streaming\n";
            return 1;
        }
        std::cout << "[+" << ms_since(t_open) << " ms] QUERYCAP ok: "
                  << reinterpret_cast<const char*>(cap.card) << "\n";
    }

    // S_FMT — request GREY 640x360.
    {
        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width       = kWidth;
        fmt.fmt.pix.height      = kHeight;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_GREY;
        fmt.fmt.pix.field       = V4L2_FIELD_NONE;
        if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) die("VIDIOC_S_FMT");

        if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_GREY) {
            std::cerr << "driver refused GREY pixel format\n";
            return 1;
        }
        std::cout << "[+" << ms_since(t_open) << " ms] S_FMT ok: "
                  << fmt.fmt.pix.width << "x" << fmt.fmt.pix.height
                  << " bytesperline=" << fmt.fmt.pix.bytesperline
                  << " sizeimage=" << fmt.fmt.pix.sizeimage << "\n";
    }

    // S_PARM — request 30 fps.
    {
        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator   = kFpsNumerator;
        parm.parm.capture.timeperframe.denominator = kFpsDenom;
        if (xioctl(fd, VIDIOC_S_PARM, &parm) < 0) {
            // Not fatal — some drivers ignore S_PARM. Log and continue.
            std::cerr << "warn: VIDIOC_S_PARM failed: " << std::strerror(errno) << "\n";
        } else {
            const auto& tpf = parm.parm.capture.timeperframe;
            std::cout << "[+" << ms_since(t_open) << " ms] S_PARM ok: "
                      << tpf.numerator << "/" << tpf.denominator
                      << " s/frame\n";
        }
    }

    // REQBUFS + mmap.
    std::vector<MmapBuffer> buffers(kNumBuffers);
    {
        v4l2_requestbuffers req{};
        req.count  = kNumBuffers;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) die("VIDIOC_REQBUFS");
        if (req.count < 2) {
            std::cerr << "not enough mmap buffers granted: " << req.count << "\n";
            return 1;
        }
        buffers.resize(req.count);

        for (unsigned i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index  = i;
            if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) die("VIDIOC_QUERYBUF");

            buffers[i].length = buf.length;
            buffers[i].start  = ::mmap(nullptr, buf.length,
                                       PROT_READ | PROT_WRITE, MAP_SHARED,
                                       fd, static_cast<off_t>(buf.m.offset));
            if (buffers[i].start == MAP_FAILED) die("mmap");
        }
        std::cout << "[+" << ms_since(t_open) << " ms] REQBUFS+mmap ok: "
                  << buffers.size() << " buffers\n";
    }

    // Enqueue all buffers, start streaming.
    for (size_t i = 0; i < buffers.size(); ++i) {
        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = static_cast<__u32>(i);
        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) die("VIDIOC_QBUF (init)");
    }
    {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) die("VIDIOC_STREAMON");
        std::cout << "[+" << ms_since(t_open) << " ms] STREAMON\n\n";
    }

    // Capture loop.
    int saved = 0;
    auto t_first_frame = clock_type::time_point{};
    for (int i = 0; i < num_frames; ++i) {
        // Wait for a frame to become available.
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        timeval tv{};
        tv.tv_sec  = 2;
        tv.tv_usec = 0;

        auto t_wait = clock_type::now();
        int r = ::select(fd + 1, &fds, nullptr, nullptr, &tv);
        if (r < 0 && errno != EINTR) die("select");
        if (r == 0) {
            std::cerr << "frame " << i << ": select timeout\n";
            break;
        }

        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) die("VIDIOC_DQBUF");

        const double t_total = ms_since(t_open);
        const double t_wait_ms = ms_since(t_wait);
        if (i == 0) t_first_frame = clock_type::now();

        // Quick brightness check — sum a sparse sample to keep it cheap.
        // Useful to see warmup frames brightening up.
        const auto* px = static_cast<const uint8_t*>(buffers[buf.index].start);
        uint64_t sum = 0;
        const size_t total_px = static_cast<size_t>(kWidth) * kHeight;
        constexpr size_t stride = 64; // ~3600 sample points
        size_t n = 0;
        for (size_t p = 0; p < total_px; p += stride) {
            sum += px[p];
            ++n;
        }
        const double mean = static_cast<double>(sum) / static_cast<double>(n);

        const bool is_warmup = (i < kWarmupFrames);
        std::cout << "frame " << i
                  << (is_warmup ? " [warmup]" : "        ")
                  << "  wait=" << t_wait_ms << " ms"
                  << "  total=" << t_total << " ms"
                  << "  bytesused=" << buf.bytesused
                  << "  mean_brightness=" << mean
                  << "\n";

        if (!is_warmup) {
            char path[128];
            std::snprintf(path, sizeof(path), "/tmp/chowdy-frame-%02d.pgm", saved);
            write_pgm(path, px, kWidth, kHeight);
            ++saved;
        }

        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) die("VIDIOC_QBUF (recycle)");
    }

    std::cout << "\n[+" << ms_since(t_open) << " ms] capture loop done, saved "
              << saved << " frame(s) to /tmp/chowdy-frame-*.pgm\n";
    if (t_first_frame != clock_type::time_point{}) {
        std::cout << "time from open to first frame: "
                  << std::chrono::duration<double, std::milli>(
                         t_first_frame - t_open).count()
                  << " ms\n";
    }

    // Cleanup.
    {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0) die("VIDIOC_STREAMOFF");
    }
    for (auto& b : buffers) {
        if (b.start && b.start != MAP_FAILED) ::munmap(b.start, b.length);
    }
    ::close(fd);
    return 0;
}
