#include "daemon/handlers/test.hpp"

#include "common/logging.hpp"

namespace fastauth::daemon::handlers {

proto::AnyResponse handle_test(const Context& ctx, const proto::TestRequest& req) {
    // M4 placeholder: the M5 implementation will run a real
    // capture+detect+embed cycle. For now we just return a structured "alive"
    // response so the CLI can confirm the round-trip works.
    common::log::info("test request", {{"peer_uid", std::to_string(ctx.peer.uid)}});

    proto::TestResponse r;
    r.request_id    = req.request_id;
    r.face_detected = false;
    r.would_auth    = false;
    r.confidence    = 0.0;
    r.elapsed_ms    = 0.0;
    r.reason        = proto::reason::ok;
    return r;
}

} // namespace fastauth::daemon::handlers
