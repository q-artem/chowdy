#include "daemon/handlers/enroll.hpp"

#include <chrono>
#include <mutex>
#include <variant>

#include "common/encoding.hpp"
#include "common/logging.hpp"
#include "daemon/enroll_session.hpp"
#include "daemon/enrollment_store.hpp"
#include "daemon/pipeline.hpp"

namespace chowdy::daemon::handlers {

namespace {

proto::EnrollProgressResponse make_progress(const std::string& id,
                                            const std::string& req_id,
                                            const EnrollSession& s,
                                            const std::string& hint,
                                            double quality,
                                            bool done) {
    proto::EnrollProgressResponse r;
    r.session         = id;
    r.request_id      = req_id;
    r.frames_collected = static_cast<int>(s.embeddings.size());
    r.frames_needed   = s.max_frames;
    r.quality         = quality;
    r.done            = done;
    r.hint            = hint;
    return r;
}

proto::ErrorResponse make_err(const std::string& req_id, std::string_view reason,
                              std::string detail = "") {
    proto::ErrorResponse r;
    r.request_id = req_id;
    r.reason     = std::string(reason);
    r.detail     = std::move(detail);
    return r;
}

// Enrollment adds an authentication credential, so it must itself be
// authenticated. We require the CLI to come through sudo: peer uid 0.
// Without this gate anyone with access to an unlocked session could
// enroll THEIR face for the logged-in user and then pass face-auth as
// them — see DESIGN.md §3.
bool enroll_authorised(const Context& ctx) {
    return ctx.peer.uid == 0;
}

constexpr const char* kNeedRoot =
    "enrollment changes credentials and must run as root (use sudo)";

proto::AnyResponse handle_start(const Context& ctx, const proto::EnrollStartRequest& r) {
    if (!enroll_authorised(ctx)) {
        common::log::warn("enroll_start denied: peer is not root",
            {{"peer_uid", std::to_string(ctx.peer.uid)},
             {"target_uid", std::to_string(r.uid)}});
        return make_err(r.request_id, proto::reason::peer_denied, kNeedRoot);
    }
    if (!ctx.store || !ctx.pipeline) return make_err(r.request_id, proto::reason::internal_error);
    if (r.label.empty() || r.label.find('/') != std::string::npos)
        return make_err(r.request_id, proto::reason::internal_error, "invalid label");
    if (!ctx.enroll_sessions) return make_err(r.request_id, proto::reason::internal_error);

    const int min_f = r.min_frames > 0 ? r.min_frames : 5;
    const int max_f = r.max_frames > min_f ? r.max_frames : (min_f + 7);
    auto id = ctx.enroll_sessions->create(static_cast<uid_t>(r.uid), r.label, min_f, max_f);

    common::log::notice("enroll start",
        {{"target_uid", std::to_string(r.uid)},
         {"peer_pid",   std::to_string(ctx.peer.pid)},
         {"label", r.label},
         {"session", id}});

    EnrollSession* s = ctx.enroll_sessions->get(id);
    if (!s) return make_err(r.request_id, proto::reason::internal_error);
    return make_progress(id, r.request_id, *s, "ok", 0.0, false);
}

proto::AnyResponse handle_frame(const Context& ctx, const proto::EnrollFrameRequest& r) {
    if (!enroll_authorised(ctx))
        return make_err(r.request_id, proto::reason::peer_denied, kNeedRoot);
    if (!ctx.enroll_sessions || !ctx.pipeline)
        return make_err(r.request_id, proto::reason::internal_error);

    EnrollSession* s = ctx.enroll_sessions->get(r.session);
    if (!s) return make_err(r.request_id, proto::reason::peer_denied, "unknown session");

    if (static_cast<int>(s->embeddings.size()) >= s->max_frames) {
        return make_progress(r.session, r.request_id, *s, "ok", 0.0, true);
    }

    std::lock_guard<std::mutex> lock(ctx.pipeline->mutex());

    FrameOutcome out;
    try {
        out = ctx.pipeline->process_one_frame(std::chrono::milliseconds{800}, true);
    } catch (const std::exception& e) {
        common::log::warn("enroll pipeline failed", {{"err", e.what()}});
        return make_progress(r.session, r.request_id, *s, "no_face", 0.0, false);
    }
    ++s->attempts;

    if (!out.captured)   return make_progress(r.session, r.request_id, *s, "no_face",     0.0, false);
    if (out.brightness < ctx.pipeline->config().dark_threshold)
                         return make_progress(r.session, r.request_id, *s, "too_dark",    0.0, false);
    if (!out.has_face)   return make_progress(r.session, r.request_id, *s, "no_face",     0.0, false);

    // Face-size hints (DESIGN §7).
    const float h_frac = out.detection
        ? out.detection->box.height / 360.f
        : 0.f;
    if (h_frac < 0.15f) return make_progress(r.session, r.request_id, *s, "too_far",  out.quality, false);
    if (h_frac > 0.80f) return make_progress(r.session, r.request_id, *s, "too_close", out.quality, false);

    if (out.quality < ctx.pipeline->config().enroll_quality_min || !out.embedding) {
        return make_progress(r.session, r.request_id, *s, "blurry_hold_still", out.quality, false);
    }

    s->embeddings.push_back(std::move(*out.embedding));
    const bool done = static_cast<int>(s->embeddings.size()) >= s->max_frames;
    return make_progress(r.session, r.request_id, *s, "ok", out.quality, done);
}

proto::AnyResponse handle_finish(const Context& ctx, const proto::EnrollFinishRequest& r) {
    if (!enroll_authorised(ctx))
        return make_err(r.request_id, proto::reason::peer_denied, kNeedRoot);
    if (!ctx.enroll_sessions || !ctx.store || !ctx.pipeline)
        return make_err(r.request_id, proto::reason::internal_error);
    // Enroll is a multi-request flow — we deliberately keep the camera open
    // across enroll_start / enroll_frame to avoid paying cold-open per frame.
    // Closing happens here, at finish.
    auto cam_scope = ctx.pipeline->request_scope();   // closes camera on lazy

    EnrollSession* s = ctx.enroll_sessions->get(r.session);
    if (!s) return make_err(r.request_id, proto::reason::peer_denied, "unknown session");
    if (static_cast<int>(s->embeddings.size()) < s->min_frames)
        return make_err(r.request_id, proto::reason::low_confidence, "not enough good frames");

    common::encoding::EnrollmentFile ef;
    ef.embedder_id = ctx.pipeline->embedder_model_id();
    ef.embeddings  = s->embeddings;
    ef.threshold   = common::encoding::pick_threshold(ef.embeddings);

    try {
        ctx.store->save(s->uid, s->label, ef);   // s->uid = target user from enroll_start
    } catch (const std::exception& e) {
        return make_err(r.request_id, proto::reason::internal_error, e.what());
    }

    proto::EnrollDoneResponse done;
    done.request_id       = r.request_id;
    done.label            = s->label;
    done.embeddings_saved = static_cast<int>(ef.embeddings.size());
    done.threshold        = ef.threshold;

    common::log::notice("enroll saved",
        {{"target_uid", std::to_string(s->uid)},
         {"label", s->label},
         {"n", std::to_string(done.embeddings_saved)},
         {"threshold", std::to_string(done.threshold)}});

    ctx.enroll_sessions->drop(r.session);
    return done;
}

} // namespace

proto::AnyResponse handle_enroll(
    const Context& ctx,
    const std::variant<proto::EnrollStartRequest,
                       proto::EnrollFrameRequest,
                       proto::EnrollFinishRequest>& req) {
    return std::visit([&](auto&& concrete) -> proto::AnyResponse {
        using T = std::decay_t<decltype(concrete)>;
        if constexpr (std::is_same_v<T, proto::EnrollStartRequest>)
            return handle_start(ctx, concrete);
        else if constexpr (std::is_same_v<T, proto::EnrollFrameRequest>)
            return handle_frame(ctx, concrete);
        else
            return handle_finish(ctx, concrete);
    }, req);
}

} // namespace chowdy::daemon::handlers
