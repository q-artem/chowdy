#include "daemon/handlers/auth.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>

#include "common/encoding.hpp"
#include "common/logging.hpp"
#include "daemon/enrollment_store.hpp"
#include "daemon/pipeline.hpp"

namespace chowdy::daemon::handlers {

namespace {

using clock_type = std::chrono::steady_clock;
double ms_since(clock_type::time_point t0) {
    return std::chrono::duration<double, std::milli>(clock_type::now() - t0).count();
}

// SO_PEERCRED gate from DESIGN.md §5: PAM-facing socket only accepts a uid==0
// caller authenticating any user, OR a self-authenticating caller (uid in the
// request matches the connecting uid). Connections via mgmt socket can do
// either form too — useful for chowdy-cli auth-test under the current user.
bool authorise(const Context& ctx, uint32_t req_uid) {
    if (ctx.peer.uid == 0) return true;             // PAM proxying for the user
    return ctx.peer.uid == req_uid;                 // self-auth
}

proto::AuthResponse make_resp(const proto::AuthRequest& req, std::string_view reason,
                              double elapsed_ms,
                              const std::string& matched_label = "",
                              double confidence = 0.0,
                              bool success = false) {
    proto::AuthResponse r;
    r.request_id    = req.request_id;
    r.success       = success;
    r.reason        = std::string(reason);
    r.matched_label = matched_label;
    r.confidence    = confidence;
    r.elapsed_ms    = elapsed_ms;
    return r;
}

} // namespace

proto::AnyResponse handle_auth(const Context& ctx, const proto::AuthRequest& req) {
    const auto t0 = clock_type::now();

    if (!ctx.pipeline || !ctx.store) {
        return make_resp(req, proto::reason::internal_error, ms_since(t0));
    }

    // Create the camera-closing scope FIRST, before any early-return check.
    // The speculative prewarm (Server, on connect) may already have opened
    // the camera; if we bail out below (peer_denied / not_enrolled /
    // embedder_mismatch) we must still close it, otherwise the LED stays lit
    // with nothing to shut it. On lazy the scope's dtor spawns the async
    // close on every return path. No-op if nothing opened the camera.
    auto cam_scope = ctx.pipeline->request_scope();

    if (!authorise(ctx, req.uid)) {
        common::log::warn("auth peer not authorised",
            {{"peer_uid", std::to_string(ctx.peer.uid)},
             {"req_uid",  std::to_string(req.uid)}});
        return make_resp(req, proto::reason::peer_denied, ms_since(t0));
    }

    auto enrollments = ctx.store->for_uid(req.uid);
    if (enrollments.empty()) {
        return make_resp(req, proto::reason::not_enrolled, ms_since(t0));
    }

    // All enrollments for this uid must have been produced by the embedder we
    // currently load; otherwise cosine values are meaningless.
    const uint32_t my_id = ctx.pipeline->embedder_model_id();
    enrollments.erase(std::remove_if(enrollments.begin(), enrollments.end(),
        [my_id](const UserEnrollment& u) { return u.file.embedder_id != my_id; }),
        enrollments.end());
    if (enrollments.empty()) {
        return make_resp(req, proto::reason::embedder_mismatch, ms_since(t0));
    }

    // Serialise camera access across connections.
    std::lock_guard<std::mutex> lock(ctx.pipeline->mutex());

    const auto budget = std::chrono::milliseconds(req.timeout_ms > 0 ? req.timeout_ms : 2000);
    const auto deadline = t0 + budget;

    int attempts = 0;
    int faces_seen = 0;
    int captures_seen = 0;
    double best_sim_overall = -1.0;
    std::string best_label;

    while (clock_type::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - clock_type::now());
        if (remaining.count() <= 0) break;
        auto step_to = std::min(remaining, std::chrono::milliseconds{300});

        FrameOutcome out;
        try {
            out = ctx.pipeline->process_one_frame(step_to, /*run_embedder=*/true);
        } catch (const std::exception& e) {
            common::log::error("pipeline frame failed", {{"err", e.what()}});
            return make_resp(req, proto::reason::camera_error, ms_since(t0));
        }
        ++attempts;

        if (!out.captured) continue;
        ++captures_seen;
        if (out.brightness < ctx.pipeline->config().dark_threshold) continue;   // emitter off-frame
        if (!out.has_face)                 continue;
        ++faces_seen;
        if (!out.embedding) continue;

        for (const auto& u : enrollments) {
            double best_for_label = -1.0;
            for (const auto& e : u.file.embeddings) {
                const double s = common::encoding::cosine_sim_normed(
                    out.embedding->data(), e.data(), common::encoding::kEmbDim);
                if (s > best_for_label) best_for_label = s;
            }
            if (best_for_label > best_sim_overall) {
                best_sim_overall = best_for_label;
                best_label       = u.label;
            }
            if (best_for_label >= u.file.threshold) {
                common::log::notice("auth matched",
                    {{"uid", std::to_string(req.uid)},
                     {"label", u.label},
                     {"sim",   std::to_string(best_for_label)},
                     {"attempts", std::to_string(attempts)}});
                return make_resp(req, proto::reason::matched, ms_since(t0),
                                 u.label, best_for_label, /*success=*/true);
            }
        }
    }

    // Choose the most informative final reason based on what actually happened:
    //   faces_seen > 0    -> we saw the face, just under threshold
    //   captures_seen > 0 -> camera worked but never had a face (or all dark)
    //   else              -> couldn't get any frame in time
    std::string final_reason = proto::reason::timeout;
    if (faces_seen > 0)        final_reason = proto::reason::low_confidence;
    else if (captures_seen > 0) final_reason = proto::reason::no_face;

    common::log::info("auth not matched",
        {{"uid", std::to_string(req.uid)},
         {"reason", final_reason},
         {"attempts", std::to_string(attempts)},
         {"faces", std::to_string(faces_seen)},
         {"best_sim", std::to_string(best_sim_overall)}});

    return make_resp(req, final_reason, ms_since(t0),
                     best_label, best_sim_overall > 0 ? best_sim_overall : 0.0,
                     /*success=*/false);
}

} // namespace chowdy::daemon::handlers
