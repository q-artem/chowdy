#include "daemon/server.hpp"

#include <cerrno>
#include <cstring>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/logging.hpp"
#include "common/peer_cred.hpp"
#include "proto/messages.hpp"

#include "daemon/handlers/test.hpp"
#include "daemon/handlers/auth.hpp"
#include "daemon/handlers/enroll.hpp"
#include "daemon/handlers/list_remove.hpp"
#include "daemon/pipeline.hpp"

namespace chowdy::daemon {

namespace {

common::Fd adopt_listen_fd(int fd) {
    // systemd-activated fds: socket already bound/listening. Just take it.
    return common::Fd(fd);
}

void mark_close_on_exec(int fd) {
    int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

} // namespace

Server::Server(ServerConfig cfg, Pipeline* pipeline, EnrollmentStore* store,
               EnrollSessionManager* enroll_sessions)
    : cfg_(std::move(cfg)), pipeline_(pipeline), store_(store),
      enroll_sessions_(enroll_sessions) {
    if (cfg_.auth_socket_fd >= 0) auth_listen_ = adopt_listen_fd(cfg_.auth_socket_fd);
    else                          auth_listen_ = common::listen_unix(cfg_.auth_socket_path, cfg_.auth_socket_mode);

    if (cfg_.mgmt_socket_fd >= 0) mgmt_listen_ = adopt_listen_fd(cfg_.mgmt_socket_fd);
    else                          mgmt_listen_ = common::listen_unix(cfg_.mgmt_socket_path, cfg_.mgmt_socket_mode);

    mark_close_on_exec(auth_listen_.get());
    mark_close_on_exec(mgmt_listen_.get());

    int pipefd[2];
    if (::pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) < 0)
        throw std::runtime_error(std::string("pipe2: ") + std::strerror(errno));
    wake_r_ = common::Fd(pipefd[0]);
    wake_w_ = common::Fd(pipefd[1]);
}

Server::~Server() { stop(); for (auto& t : threads_) if (t.joinable()) t.join(); }

void Server::stop() {
    stopping_.store(true);
    char b = 1;
    // Best-effort wake-up; if the pipe is gone we already torn down,
    // so ignoring the result is the right thing here.
    if (wake_w_.valid()) (void)!::write(wake_w_.get(), &b, 1);
}

void Server::run() {
    std::thread auth_th(&Server::accept_loop, this, auth_listen_.get(), SockKind::Auth);
    std::thread mgmt_th(&Server::accept_loop, this, mgmt_listen_.get(), SockKind::Mgmt);
    threads_.push_back(std::move(auth_th));
    threads_.push_back(std::move(mgmt_th));
    for (auto& t : threads_) if (t.joinable()) t.join();
    threads_.clear();
}

void Server::accept_loop(int listen_fd, SockKind kind) {
    while (!stopping_.load()) {
        pollfd pfds[2];
        pfds[0].fd = listen_fd;       pfds[0].events = POLLIN;
        pfds[1].fd = wake_r_.get();   pfds[1].events = POLLIN;
        int r = ::poll(pfds, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            common::log::error("poll failed",
                {{"err", std::strerror(errno)}});
            break;
        }
        if (stopping_.load()) break;
        if (!(pfds[0].revents & POLLIN)) continue;

        int cfd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            common::log::warn("accept failed",
                {{"err", std::strerror(errno)}});
            continue;
        }
        common::Fd conn(cfd);
        // One thread per connection — connections are short-lived (single auth
        // round-trip or short enrollment session), no need for a pool yet.
        //
        // Pre-warm the camera ONLY on the auth socket: there the sole request
        // type is `auth`, whose handler always closes the camera via its
        // request_scope. The mgmt socket carries list/remove (which never
        // touch the camera) and test/enroll (which open+close it themselves) —
        // speculatively opening here would leave the stream on after a `list`
        // with nothing to close it. start_stream + the driver's first-frame
        // wait overlap with read_message + parse. No-op when policy != lazy.
        if (pipeline_ && kind == SockKind::Auth) pipeline_->prewarm_async();

        std::thread([this, c = std::move(conn), kind]() mutable {
            try { handle_connection(std::move(c), kind); }
            catch (const std::exception& e) {
                common::log::warn("connection handler crashed",
                    {{"err", e.what()}});
            }
        }).detach();
    }
}

void Server::handle_connection(common::Fd conn, SockKind kind) {
    using namespace chowdy::proto;
    common::set_timeout(conn.get(),
                        std::chrono::milliseconds(5000),
                        std::chrono::milliseconds(5000));

    common::PeerCred peer{};
    try { peer = common::get_peer_cred(conn.get()); }
    catch (const std::exception& e) {
        ErrorResponse er; er.reason = reason::internal_error;
        er.detail = std::string("peer_cred: ") + e.what();
        common::write_message(conn.get(), json(er));
        return;
    }

    common::log::debug("connection accepted",
        {{"sock", kind == SockKind::Auth ? "auth" : "mgmt"},
         {"peer_uid", std::to_string(peer.uid)},
         {"peer_pid", std::to_string(peer.pid)}});

    json req_json;
    try { req_json = common::read_message(conn.get()); }
    catch (const std::exception& e) {
        // Bumped to warn — silent drop here is exactly what showed up as
        // "peer closed mid-frame" on the CLI side and was invisible in the
        // journal.
        common::log::warn("read_message failed",
            {{"err",      e.what()},
             {"sock",     kind == SockKind::Auth ? "auth" : "mgmt"},
             {"peer_uid", std::to_string(peer.uid)},
             {"peer_pid", std::to_string(peer.pid)}});
        return;
    }

    handlers::Context ctx{
        .peer            = peer,
        .conn_fd         = conn.get(),
        .is_auth_sock    = (kind == SockKind::Auth),
        .pipeline        = pipeline_,
        .store           = store_,
        .enroll_sessions = enroll_sessions_,
    };

    AnyRequest req;
    try { req = parse_request(req_json); }
    catch (const std::exception& e) {
        ErrorResponse er;
        if (req_json.contains("request_id"))
            er.request_id = req_json.value("request_id", "");
        er.reason = reason::internal_error;
        er.detail = e.what();
        common::write_message(conn.get(), json(er));
        return;
    }

    AnyResponse resp = std::visit([&](auto&& concrete) -> AnyResponse {
        using T = std::decay_t<decltype(concrete)>;
        if constexpr (std::is_same_v<T, AuthRequest>) {
            return handlers::handle_auth(ctx, concrete);
        } else if constexpr (std::is_same_v<T, TestRequest>) {
            return handlers::handle_test(ctx, concrete);
        } else if constexpr (std::is_same_v<T, EnrollStartRequest>
                          || std::is_same_v<T, EnrollFrameRequest>
                          || std::is_same_v<T, EnrollFinishRequest>) {
            return handlers::handle_enroll(ctx, concrete);
        } else if constexpr (std::is_same_v<T, ListRequest>) {
            return handlers::handle_list(ctx, concrete);
        } else if constexpr (std::is_same_v<T, RemoveRequest>) {
            return handlers::handle_remove(ctx, concrete);
        } else {
            ErrorResponse er; er.reason = reason::internal_error;
            er.detail = "unhandled request variant";
            return er;
        }
    }, req);

    try { common::write_message(conn.get(), serialize_response(resp)); }
    catch (const std::exception& e) {
        common::log::warn("write_message failed",
            {{"err",      e.what()},
             {"sock",     kind == SockKind::Auth ? "auth" : "mgmt"},
             {"peer_uid", std::to_string(peer.uid)},
             {"peer_pid", std::to_string(peer.pid)}});
    }
}

} // namespace chowdy::daemon
