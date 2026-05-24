#pragma once

#include "daemon/handlers/test.hpp"
#include "proto/messages.hpp"

namespace chowdy::daemon::handlers {

proto::AnyResponse handle_auth(const Context& ctx, const proto::AuthRequest& req);

} // namespace chowdy::daemon::handlers
