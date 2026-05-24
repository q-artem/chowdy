#include "daemon/handlers/test.hpp"

#include <chrono>
#include <mutex>

#include "common/encoding.hpp"
#include "common/logging.hpp"
#include "daemon/enrollment_store.hpp"
#include "daemon/pipeline.hpp"

namespace chowdy::daemon::handlers {

proto::AnyResponse handle_test(const Context& ctx, const proto::TestRequest& req) {
    using clock_type = std::chrono::steady_clock;
    const auto t0 = clock_type::now();

    proto::TestResponse r;
    r.request_id = req.request_id;
    r.reason     = proto::reason::ok;

    if (!ctx.pipeline) {
        r.reason = proto::reason::internal_error;
        return r;
    }

    std::lock_guard<std::mutex> lock(ctx.pipeline->mutex());
    auto cam_scope = ctx.pipeline->request_scope();   // closes camera on lazy

    const auto deadline = t0 + std::chrono::milliseconds(req.timeout_ms > 0
                                                         ? req.timeout_ms : 3000);
    FrameOutcome out;
    try {
        while (clock_type::now() < deadline) {
            out = ctx.pipeline->process_one_frame(std::chrono::milliseconds{300}, true);
            if (out.has_face) break;
        }
    } catch (const std::exception& e) {
        common::log::warn("test pipeline failed", {{"err", e.what()}});
        r.reason = proto::reason::camera_error;
        r.elapsed_ms = std::chrono::duration<double, std::milli>(clock_type::now() - t0).count();
        return r;
    }

    r.face_detected = out.has_face;
    r.elapsed_ms = std::chrono::duration<double, std::milli>(clock_type::now() - t0).count();
    if (!out.has_face) {
        r.reason = proto::reason::no_face;
        return r;
    }
    r.confidence = out.detection ? out.detection->score : 0.0;

    if (ctx.store && out.embedding) {
        // Compare against enrollments for the caller's uid — convenient for
        // CLI smoke tests ("does what's in the camera look like me?").
        auto enrollments = ctx.store->for_uid(ctx.peer.uid);
        double best = -1.0;
        std::string best_label;
        for (const auto& u : enrollments) {
            if (u.file.embedder_id != ctx.pipeline->embedder_model_id()) continue;
            for (const auto& e : u.file.embeddings) {
                double s = common::encoding::cosine_sim_normed(
                    out.embedding->data(), e.data(), common::encoding::kEmbDim);
                if (s > best) { best = s; best_label = u.label; }
            }
        }
        if (best > 0) {
            r.best_match = best_label;
            r.confidence = best;
            // Find the matching enrollment's threshold to compute would_auth.
            for (const auto& u : enrollments) {
                if (u.label == best_label && best >= u.file.threshold) {
                    r.would_auth = true;
                    break;
                }
            }
        }
    }
    return r;
}

} // namespace chowdy::daemon::handlers
