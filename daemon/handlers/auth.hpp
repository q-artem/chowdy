#pragma once

#include "daemon/handlers/test.hpp"
#include "proto/messages.hpp"

namespace fastauth::daemon::handlers {

proto::AnyResponse handle_auth(const Context& ctx, const proto::AuthRequest& req);

} // namespace fastauth::daemon::handlers
