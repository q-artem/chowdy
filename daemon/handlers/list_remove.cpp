#include "daemon/handlers/list_remove.hpp"

#include "common/logging.hpp"
#include "daemon/enrollment_store.hpp"

namespace fastauth::daemon::handlers {

proto::AnyResponse handle_list(const Context& ctx, const proto::ListRequest& req) {
    proto::ListResponse r;
    r.request_id = req.request_id;
    if (!ctx.store) return r;
    auto items = ctx.store->for_uid(ctx.peer.uid);
    r.enrollments.reserve(items.size());
    for (auto& u : items) {
        proto::ListResponse::Item it;
        it.label      = u.label;
        it.created    = u.file.created;
        it.embeddings = static_cast<int>(u.file.embeddings.size());
        it.threshold  = u.file.threshold;
        r.enrollments.push_back(std::move(it));
    }
    return r;
}

proto::AnyResponse handle_remove(const Context& ctx, const proto::RemoveRequest& req) {
    proto::RemoveResponse r;
    r.request_id = req.request_id;
    if (!ctx.store) { r.ok = false; return r; }
    int n = req.label.empty()
        ? ctx.store->remove_all(ctx.peer.uid)
        : ctx.store->remove(ctx.peer.uid, req.label);
    r.ok = n > 0;
    common::log::info("enrollment remove",
        {{"uid", std::to_string(ctx.peer.uid)},
         {"label", req.label.empty() ? "(all)" : req.label},
         {"n", std::to_string(n)}});
    return r;
}

} // namespace fastauth::daemon::handlers
