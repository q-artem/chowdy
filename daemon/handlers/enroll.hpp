#pragma once

#include "daemon/handlers/test.hpp"
#include "proto/messages.hpp"

#include <variant>

namespace chowdy::daemon::handlers {

// Single entry point handling all three enrollment messages.
proto::AnyResponse handle_enroll(
    const Context& ctx,
    const std::variant<proto::EnrollStartRequest,
                       proto::EnrollFrameRequest,
                       proto::EnrollFinishRequest>& req);

// Overloads so std::visit in server.cpp can call us directly with each
// concrete request type.
inline proto::AnyResponse handle_enroll(const Context& ctx,
                                        const proto::EnrollStartRequest& r) {
    return handle_enroll(ctx, std::variant<proto::EnrollStartRequest,
                                           proto::EnrollFrameRequest,
                                           proto::EnrollFinishRequest>{r});
}
inline proto::AnyResponse handle_enroll(const Context& ctx,
                                        const proto::EnrollFrameRequest& r) {
    return handle_enroll(ctx, std::variant<proto::EnrollStartRequest,
                                           proto::EnrollFrameRequest,
                                           proto::EnrollFinishRequest>{r});
}
inline proto::AnyResponse handle_enroll(const Context& ctx,
                                        const proto::EnrollFinishRequest& r) {
    return handle_enroll(ctx, std::variant<proto::EnrollStartRequest,
                                           proto::EnrollFrameRequest,
                                           proto::EnrollFinishRequest>{r});
}

} // namespace chowdy::daemon::handlers
