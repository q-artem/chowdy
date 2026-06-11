#include "daemon/handlers/list_remove.hpp"

#include "common/logging.hpp"
#include "daemon/enrollment_store.hpp"

namespace chowdy::daemon::handlers {

proto::AnyResponse handle_list(const Context& ctx, const proto::ListRequest& req) {
    proto::ListResponse r;
    r.request_id = req.request_id;
    if (!ctx.store) return r;
    // Read-only: a user may always see their own list. Only root may ask
    // about someone else's (CLI under sudo passes the SUDO_UID target).
    const uid_t target = (ctx.peer.uid == 0 && req.uid != 0)
        ? static_cast<uid_t>(req.uid)
        : ctx.peer.uid;
    auto items = ctx.store->for_uid(target);
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
    // Mutating credentials requires sudo — same contract as enroll.
    // Without it, anyone at an unlocked session could silently delete
    // (or worse, replace) the legitimate user's enrollments.
    if (ctx.peer.uid != 0) {
        common::log::warn("remove denied: peer is not root",
            {{"peer_uid", std::to_string(ctx.peer.uid)},
             {"target_uid", std::to_string(req.uid)}});
        proto::ErrorResponse er;
        er.request_id = req.request_id;
        er.reason     = proto::reason::peer_denied;
        er.detail     = "removing enrollments must run as root (use sudo)";
        return er;
    }
    proto::RemoveResponse r;
    r.request_id = req.request_id;
    if (!ctx.store) { r.ok = false; return r; }

    const uid_t target = static_cast<uid_t>(req.uid);
    int n = req.label.empty()
        ? ctx.store->remove_all(target)
        : ctx.store->remove(target, req.label);
    r.ok = n > 0;
    common::log::notice("enrollment remove",
        {{"target_uid", std::to_string(target)},
         {"peer_pid",   std::to_string(ctx.peer.pid)},
         {"label", req.label.empty() ? "(all)" : req.label},
         {"n", std::to_string(n)}});
    return r;
}

} // namespace chowdy::daemon::handlers
