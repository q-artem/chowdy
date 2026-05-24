#include "daemon/handlers/auth.hpp"

#include "common/logging.hpp"

namespace fastauth::daemon::handlers {

proto::AnyResponse handle_auth(const Context& ctx, const proto::AuthRequest& req) {
    // M4 stub. The real pipeline lands on M5: open camera, capture loop,
    // detect/embed/match against enrollments for ctx.peer.uid (after
    // verifying ctx.peer.uid==0 for PAM, or ctx.peer.uid==req.uid for
    // self-test), apply timeout, return matched/no_face/etc.
    common::log::info("auth request (stub)",
        {{"peer_uid", std::to_string(ctx.peer.uid)},
         {"req_uid",  std::to_string(req.uid)}});

    proto::AuthResponse r;
    r.request_id = req.request_id;
    r.success    = false;
    r.reason     = proto::reason::not_enrolled;
    r.elapsed_ms = 0.0;
    return r;
}

} // namespace fastauth::daemon::handlers
