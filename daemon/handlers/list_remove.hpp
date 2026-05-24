#pragma once

#include "daemon/handlers/test.hpp"
#include "proto/messages.hpp"

namespace fastauth::daemon::handlers {

proto::AnyResponse handle_list  (const Context& ctx, const proto::ListRequest&   req);
proto::AnyResponse handle_remove(const Context& ctx, const proto::RemoveRequest& req);

} // namespace fastauth::daemon::handlers
