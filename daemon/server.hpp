// fastauthd's accept loop. Owns the two listening sockets (auth + mgmt),
// spawns one short-lived thread per connection, and dispatches requests to
// handlers/ by message type.

#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "common/ipc.hpp"

namespace fastauth::daemon {

struct ServerConfig {
    std::string auth_socket_path;
    std::string mgmt_socket_path;
    mode_t      auth_socket_mode = 0660;
    mode_t      mgmt_socket_mode = 0666;
    // When >= 0 these override the listening sockets — for systemd socket
    // activation via sd_listen_fds.
    int         auth_socket_fd = -1;
    int         mgmt_socket_fd = -1;
};

class Server {
public:
    explicit Server(ServerConfig cfg);
    ~Server();

    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    void run();      // blocks; returns when stop() is called
    void stop();     // can be called from a signal handler thread

private:
    enum class SockKind { Auth, Mgmt };

    void accept_loop(int listen_fd, SockKind kind);
    void handle_connection(common::Fd conn, SockKind kind);

    ServerConfig            cfg_;
    common::Fd              auth_listen_;
    common::Fd              mgmt_listen_;
    std::atomic<bool>       stopping_{false};
    std::vector<std::thread> threads_;
    common::Fd              wake_r_;     // self-pipe to interrupt accept()
    common::Fd              wake_w_;
};

} // namespace fastauth::daemon
