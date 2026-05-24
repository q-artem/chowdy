#include "daemon/camera.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include "common/logging.hpp"

namespace chowdy::daemon {

namespace {

int xioctl(int fd, unsigned long req, void* arg) {
    int r = 0;
    do { r = ::ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

[[noreturn]] void die(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

} // namespace

Camera::~Camera() { close(); }

void Camera::open(const CameraConfig& cfg) {
    if (fd_ >= 0) return;
    cfg_ = cfg;

    fd_ = ::open(cfg.device.c_str(), O_RDWR | O_NONBLOCK, 0);
    if (fd_ < 0) die("open(" + cfg.device + ")");

    v4l2_capability cap{};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) die("VIDIOC_QUERYCAP");
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING)) {
        ::close(fd_); fd_ = -1;
        throw std::runtime_error("device does not support capture + streaming");
    }

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = static_cast<uint32_t>(cfg.width);
    fmt.fmt.pix.height      = static_cast<uint32_t>(cfg.height);
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_GREY;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) die("VIDIOC_S_FMT");
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_GREY) {
        ::close(fd_); fd_ = -1;
        throw std::runtime_error("driver refused V4L2_PIX_FMT_GREY");
    }

    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = static_cast<uint32_t>(cfg.fps);
    if (xioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
        common::log::warn("VIDIOC_S_PARM failed; effective fps depends on driver",
            {{"err", std::strerror(errno)}});
    }

    v4l2_requestbuffers req{};
    req.count  = static_cast<uint32_t>(cfg.buffer_count);
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) die("VIDIOC_REQBUFS");
    if (req.count < 2) {
        ::close(fd_); fd_ = -1;
        throw std::runtime_error("driver gave too few mmap buffers");
    }
    buffers_.resize(req.count);
    for (unsigned i = 0; i < req.count; ++i) {
        v4l2_buffer b{};
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index  = i;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) die("VIDIOC_QUERYBUF");
        buffers_[i].length = b.length;
        buffers_[i].start  = ::mmap(nullptr, b.length, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd_, static_cast<off_t>(b.m.offset));
        if (buffers_[i].start == MAP_FAILED) die("mmap");
    }
    start_stream();
}

void Camera::start_stream() {
    if (streaming_) return;
    if (fd_ < 0) throw std::runtime_error("start_stream on unopened camera");

    auto try_once = [this]() -> int {
        for (size_t i = 0; i < buffers_.size(); ++i) {
            v4l2_buffer b{};
            b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            b.index  = static_cast<__u32>(i);
            // After a STREAMOFF buffers are in dequeued state — QBUF before STREAMON.
            if (xioctl(fd_, VIDIOC_QBUF, &b) < 0) return errno;
        }
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) return errno;
        return 0;
    };

    // Retry on EBUSY / EINVAL — these usually mean "another process is currently
    // streaming on this device" (classic on a system where Howdy or some other
    // V4L2 consumer is also poking /dev/video2 from PAM). Back off and try a
    // few times; if it still won't go after ~600ms, give up.
    constexpr int kMaxAttempts = 6;
    constexpr int kBackoffMs   = 100;
    int err = 0;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        err = try_once();
        if (err == 0) { streaming_ = true; return; }
        if (err != EBUSY && err != EINVAL) break;
        // Stop the stream (no-op if QBUF failed before STREAMON) and reset queue
        // state before the next try. STREAMOFF on a non-streaming device returns
        // success on uvcvideo.
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
        ::usleep(static_cast<useconds_t>(kBackoffMs * 1000));
    }
    errno = err;
    die("VIDIOC_STREAMON");
}

void Camera::stop_stream() noexcept {
    if (!streaming_ || fd_ < 0) return;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(fd_, VIDIOC_STREAMOFF, &type);
    streaming_ = false;
}

void Camera::close() noexcept {
    if (fd_ < 0) return;
    stop_stream();
    for (auto& b : buffers_) {
        if (b.start && b.start != MAP_FAILED) ::munmap(b.start, b.length);
    }
    buffers_.clear();
    ::close(fd_);
    fd_ = -1;
}

cv::Mat Camera::capture(std::chrono::milliseconds timeout) {
    if (fd_ < 0 || !streaming_)
        throw std::runtime_error("Camera::capture without active stream");

    fd_set fds; FD_ZERO(&fds); FD_SET(fd_, &fds);
    timeval tv{};
    tv.tv_sec  = timeout.count() / 1000;
    tv.tv_usec = (timeout.count() % 1000) * 1000;
    int r = ::select(fd_ + 1, &fds, nullptr, nullptr, &tv);
    if (r <= 0) return {};   // timeout / error — caller treats as no-frame

    v4l2_buffer b{};
    b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_DQBUF, &b) < 0) die("VIDIOC_DQBUF");

    cv::Mat view(cfg_.height, cfg_.width, CV_8UC1, buffers_[b.index].start);
    cv::Mat copy = view.clone();   // own the memory; mmap buffer is about to be recycled

    if (xioctl(fd_, VIDIOC_QBUF, &b) < 0) die("VIDIOC_QBUF (recycle)");
    return copy;
}

} // namespace chowdy::daemon
