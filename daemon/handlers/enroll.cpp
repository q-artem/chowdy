#include "daemon/handlers/enroll.hpp"

#include "common/logging.hpp"

namespace fastauth::daemon::handlers {

proto::AnyResponse handle_enroll(
    const Context& ctx,
    const std::variant<proto::EnrollStartRequest,
                       proto::EnrollFrameRequest,
                       proto::EnrollFinishRequest>& req) {
    // M4 stub — full session-based enrollment lives in M7.
    common::log::info("enroll request (stub)",
        {{"peer_uid", std::to_string(ctx.peer.uid)}});

    return std::visit([&](const auto& concrete) -> proto::AnyResponse {
        proto::ErrorResponse er;
        er.request_id = concrete.request_id;
        er.reason     = proto::reason::internal_error;
        er.detail     = "enroll not implemented yet";
        return er;
    }, req);
}

} // namespace fastauth::daemon::handlers
