// Unix-socket IPC primitives for fastauthd.
//
// All messages are framed as:
//   [uint32_t big-endian payload length] [JSON UTF-8 bytes]
//
// Both sides use blocking I/O with optional socket timeouts. The daemon's
// accept loop spawns one thread per accepted connection — connection lifetime
// is short (a single auth round-trip or enrollment session), so the simplicity
// outweighs an event loop here.

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace fastauth::common {

class IpcError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Owning RAII wrapper around a file descriptor.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) { close(); fd_ = o.fd_; o.fd_ = -1; }
        return *this;
    }
    ~Fd() { close(); }

    int  get() const noexcept   { return fd_; }
    int  release() noexcept     { int f = fd_; fd_ = -1; return f; }
    bool valid()  const noexcept{ return fd_ >= 0; }
    void close()  noexcept;

private:
    int fd_ = -1;
};

// Connect a client AF_UNIX SOCK_STREAM socket to `path`.
Fd connect_unix(const std::string& path);

// Bind a server socket at `path` (unlinks an existing inode), set socket
// permissions to `mode`, and start listening. Returns the listening fd.
Fd listen_unix(const std::string& path, mode_t mode, int backlog = 16);

// Apply receive/send timeouts to an existing fd.
void set_timeout(int fd, std::chrono::milliseconds rcv,
                 std::chrono::milliseconds snd);

// Read one length-prefixed frame from `fd`. Throws IpcError on EOF before a
// complete frame or on too-large payload.
nlohmann::json read_message(int fd, size_t max_payload = 1 << 20);

// Write a length-prefixed JSON message to `fd`.
void write_message(int fd, const nlohmann::json& j);

} // namespace fastauth::common
