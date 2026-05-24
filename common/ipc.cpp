#include "common/ipc.hpp"

#include <array>
#include <cerrno>
#include <cstring>

#include <netinet/in.h> // htonl / ntohl
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace fastauth::common {

void Fd::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

namespace {

void check_path_fits(const std::string& path) {
    sockaddr_un u{};
    if (path.size() + 1 > sizeof(u.sun_path))
        throw IpcError("socket path too long: " + path);
}

void fill_addr(sockaddr_un& addr, const std::string& path) {
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.data(), path.size());
    addr.sun_path[path.size()] = '\0';
}

} // namespace

Fd connect_unix(const std::string& path) {
    check_path_fits(path);
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw IpcError(std::string("socket: ") + std::strerror(errno));
    Fd guard(fd);

    sockaddr_un addr{};
    fill_addr(addr, path);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw IpcError("connect " + path + ": " + std::strerror(errno));

    return guard;
}

Fd listen_unix(const std::string& path, mode_t mode, int backlog) {
    check_path_fits(path);
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw IpcError(std::string("socket: ") + std::strerror(errno));
    Fd guard(fd);

    // Remove a stale socket inode if present — best effort.
    ::unlink(path.c_str());

    sockaddr_un addr{};
    fill_addr(addr, path);

    // umask manipulation so the file mode actually lands as requested,
    // independent of the daemon's process umask.
    mode_t old_umask = ::umask(0);
    int bind_rc = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    int bind_errno = errno;
    ::umask(old_umask);
    if (bind_rc < 0)
        throw IpcError("bind " + path + ": " + std::strerror(bind_errno));

    if (::chmod(path.c_str(), mode) < 0)
        throw IpcError("chmod " + path + ": " + std::strerror(errno));

    if (::listen(fd, backlog) < 0)
        throw IpcError("listen " + path + ": " + std::strerror(errno));

    return guard;
}

void set_timeout(int fd, std::chrono::milliseconds rcv,
                 std::chrono::milliseconds snd) {
    auto fill = [](std::chrono::milliseconds ms) {
        timeval tv{};
        tv.tv_sec  = static_cast<time_t>(ms.count() / 1000);
        tv.tv_usec = static_cast<suseconds_t>((ms.count() % 1000) * 1000);
        return tv;
    };
    timeval tr = fill(rcv);
    timeval ts = fill(snd);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tr, sizeof(tr));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &ts, sizeof(ts));
}

namespace {

void read_exact(int fd, void* buf, size_t n) {
    auto* p = static_cast<unsigned char*>(buf);
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, p + got, n - got);
        if (r > 0) { got += static_cast<size_t>(r); continue; }
        if (r == 0) throw IpcError("peer closed mid-frame");
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) throw IpcError("read timeout");
        throw IpcError(std::string("read: ") + std::strerror(errno));
    }
}

void write_exact(int fd, const void* buf, size_t n) {
    const auto* p = static_cast<const unsigned char*>(buf);
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = ::write(fd, p + sent, n - sent);
        if (w > 0) { sent += static_cast<size_t>(w); continue; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) throw IpcError("write timeout");
        throw IpcError(std::string("write: ") + std::strerror(errno));
    }
}

} // namespace

nlohmann::json read_message(int fd, size_t max_payload) {
    uint32_t be_len = 0;
    read_exact(fd, &be_len, sizeof(be_len));
    const uint32_t len = ::ntohl(be_len);
    if (len == 0)            throw IpcError("empty frame");
    if (len > max_payload)   throw IpcError("frame too large: " + std::to_string(len));

    std::vector<char> payload(len);
    read_exact(fd, payload.data(), len);
    return nlohmann::json::parse(std::string_view(payload.data(), payload.size()));
}

void write_message(int fd, const nlohmann::json& j) {
    const std::string s = j.dump();
    if (s.size() > (1u << 30))
        throw IpcError("payload too big: " + std::to_string(s.size()));
    const uint32_t be_len = ::htonl(static_cast<uint32_t>(s.size()));
    write_exact(fd, &be_len, sizeof(be_len));
    write_exact(fd, s.data(), s.size());
}

} // namespace fastauth::common
