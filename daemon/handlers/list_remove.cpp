#include "daemon/handlers/list_remove.hpp"

#include "common/logging.hpp"

namespace fastauth::daemon::handlers {

proto::AnyResponse handle_list(const Context& ctx, const proto::ListRequest& req) {
    common::log::info("list request (stub)",
        {{"peer_uid", std::to_string(ctx.peer.uid)}});
    proto::ListResponse r;
    r.request_id = req.request_id;
    return r; // empty list — actual scan in M7
}

proto::AnyResponse handle_remove(const Context& ctx, const proto::RemoveRequest& req) {
    common::log::info("remove request (stub)",
        {{"peer_uid", std::to_string(ctx.peer.uid)},
         {"label",    req.label}});
    proto::RemoveResponse r;
    r.request_id = req.request_id;
    r.ok         = false;
    return r;
}

} // namespace fastauth::daemon::handlers
