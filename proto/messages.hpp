// IPC message types between PAM/CLI and fastauthd.
//
// Wire format: length-prefixed JSON.
//   [u32 big-endian payload length] [UTF-8 JSON bytes]
//
// All requests carry a `type` discriminator; that drives dispatch on the
// daemon side. All responses include the same `request_id` the request had,
// to make logs traceable.

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace fastauth::proto {

using nlohmann::json;

// ===== reason codes (stable strings, logged + returned to PAM) =====
//
// New codes append-only — existing values must never change meaning so logs
// stay greppable across upgrades.
namespace reason {
inline constexpr const char* matched         = "matched";
inline constexpr const char* no_face         = "no_face";
inline constexpr const char* low_confidence  = "low_confidence";
inline constexpr const char* liveness_failed = "liveness_failed";
inline constexpr const char* not_enrolled    = "not_enrolled";
inline constexpr const char* camera_busy     = "camera_busy";
inline constexpr const char* camera_error    = "camera_error";
inline constexpr const char* timeout         = "timeout";
inline constexpr const char* internal_error  = "internal_error";
inline constexpr const char* peer_denied     = "peer_denied";
inline constexpr const char* embedder_mismatch = "embedder_mismatch";
inline constexpr const char* ok              = "ok";
} // namespace reason

// ===== request types =====

struct AuthRequest {
    uint32_t    uid        = 0;
    int         timeout_ms = 2000;
    std::string request_id;       // optional, free-form (PAM provides uuid)
};

struct TestRequest {
    int         timeout_ms = 3000;
    std::string request_id;
};

struct EnrollStartRequest {
    std::string label;
    int         min_frames = 5;
    int         max_frames = 15;
    std::string request_id;
};

struct EnrollFrameRequest {
    std::string session;
    std::string request_id;
};

struct EnrollFinishRequest {
    std::string session;
    std::string request_id;
};

struct ListRequest {
    std::string request_id;
};

struct RemoveRequest {
    std::string label;
    std::string request_id;
};

using AnyRequest = std::variant<
    AuthRequest,
    TestRequest,
    EnrollStartRequest,
    EnrollFrameRequest,
    EnrollFinishRequest,
    ListRequest,
    RemoveRequest
>;

// ===== response types =====

struct AuthResponse {
    std::string request_id;
    bool        success       = false;
    std::string reason        = reason::internal_error;
    std::string matched_label;        // empty if no match
    double      confidence    = 0.0;  // cosine similarity, 0..1
    double      elapsed_ms    = 0.0;
};

struct TestResponse {
    std::string request_id;
    bool        face_detected = false;
    std::string best_match;           // empty if no enrollments match
    double      confidence    = 0.0;
    bool        would_auth    = false;
    double      elapsed_ms    = 0.0;
    std::string reason        = reason::ok;
};

struct EnrollProgressResponse {
    std::string request_id;
    std::string session;
    int         frames_collected = 0;
    int         frames_needed    = 0;
    double      quality          = 0.0;
    bool        done             = false;
    std::string hint;                 // ok / no_face / look_straight / ...
};

struct EnrollDoneResponse {
    std::string request_id;
    std::string label;
    int         embeddings_saved = 0;
    double      threshold        = 0.0;
};

struct ListResponse {
    struct Item {
        std::string label;
        uint64_t    created       = 0;   // unix seconds
        int         embeddings    = 0;
        double      threshold     = 0.0;
    };
    std::string         request_id;
    std::vector<Item>   enrollments;
};

struct RemoveResponse {
    std::string request_id;
    bool        ok = false;
};

struct ErrorResponse {
    std::string request_id;
    std::string reason = reason::internal_error;
    std::string detail;          // human-readable, English
};

using AnyResponse = std::variant<
    AuthResponse,
    TestResponse,
    EnrollProgressResponse,
    EnrollDoneResponse,
    ListResponse,
    RemoveResponse,
    ErrorResponse
>;

// ===== JSON conversion (intrusive, ADL-found by nlohmann::json) =====

inline void to_json(json& j, const AuthRequest& r) {
    j = json{{"type", "auth"}, {"uid", r.uid}, {"timeout_ms", r.timeout_ms},
             {"request_id", r.request_id}};
}
inline void from_json(const json& j, AuthRequest& r) {
    j.at("uid").get_to(r.uid);
    if (j.contains("timeout_ms")) r.timeout_ms = j.at("timeout_ms");
    if (j.contains("request_id")) r.request_id = j.at("request_id");
}

inline void to_json(json& j, const TestRequest& r) {
    j = json{{"type", "test"}, {"timeout_ms", r.timeout_ms},
             {"request_id", r.request_id}};
}
inline void from_json(const json& j, TestRequest& r) {
    if (j.contains("timeout_ms")) r.timeout_ms = j.at("timeout_ms");
    if (j.contains("request_id")) r.request_id = j.at("request_id");
}

inline void to_json(json& j, const EnrollStartRequest& r) {
    j = json{{"type", "enroll_start"}, {"label", r.label},
             {"min_frames", r.min_frames}, {"max_frames", r.max_frames},
             {"request_id", r.request_id}};
}
inline void from_json(const json& j, EnrollStartRequest& r) {
    j.at("label").get_to(r.label);
    if (j.contains("min_frames")) r.min_frames = j.at("min_frames");
    if (j.contains("max_frames")) r.max_frames = j.at("max_frames");
    if (j.contains("request_id")) r.request_id = j.at("request_id");
}

inline void to_json(json& j, const EnrollFrameRequest& r) {
    j = json{{"type", "enroll_frame"}, {"session", r.session},
             {"request_id", r.request_id}};
}
inline void from_json(const json& j, EnrollFrameRequest& r) {
    j.at("session").get_to(r.session);
    if (j.contains("request_id")) r.request_id = j.at("request_id");
}

inline void to_json(json& j, const EnrollFinishRequest& r) {
    j = json{{"type", "enroll_finish"}, {"session", r.session},
             {"request_id", r.request_id}};
}
inline void from_json(const json& j, EnrollFinishRequest& r) {
    j.at("session").get_to(r.session);
    if (j.contains("request_id")) r.request_id = j.at("request_id");
}

inline void to_json(json& j, const ListRequest& r) {
    j = json{{"type", "list"}, {"request_id", r.request_id}};
}
inline void from_json(const json& j, ListRequest& r) {
    if (j.contains("request_id")) r.request_id = j.at("request_id");
}

inline void to_json(json& j, const RemoveRequest& r) {
    j = json{{"type", "remove"}, {"label", r.label},
             {"request_id", r.request_id}};
}
inline void from_json(const json& j, RemoveRequest& r) {
    j.at("label").get_to(r.label);
    if (j.contains("request_id")) r.request_id = j.at("request_id");
}

inline void to_json(json& j, const AuthResponse& r) {
    j = json{{"type", "auth_result"}, {"request_id", r.request_id},
             {"success", r.success}, {"reason", r.reason},
             {"matched_label", r.matched_label}, {"confidence", r.confidence},
             {"elapsed_ms", r.elapsed_ms}};
}
inline void from_json(const json& j, AuthResponse& r) {
    j.at("request_id").get_to(r.request_id);
    j.at("success").get_to(r.success);
    j.at("reason").get_to(r.reason);
    if (j.contains("matched_label")) r.matched_label = j.at("matched_label");
    if (j.contains("confidence"))    r.confidence    = j.at("confidence");
    if (j.contains("elapsed_ms"))    r.elapsed_ms    = j.at("elapsed_ms");
}

inline void to_json(json& j, const TestResponse& r) {
    j = json{{"type", "test_result"}, {"request_id", r.request_id},
             {"face_detected", r.face_detected}, {"best_match", r.best_match},
             {"confidence", r.confidence}, {"would_auth", r.would_auth},
             {"elapsed_ms", r.elapsed_ms}, {"reason", r.reason}};
}
inline void from_json(const json& j, TestResponse& r) {
    j.at("request_id").get_to(r.request_id);
    if (j.contains("face_detected")) r.face_detected = j.at("face_detected");
    if (j.contains("best_match"))    r.best_match    = j.at("best_match");
    if (j.contains("confidence"))    r.confidence    = j.at("confidence");
    if (j.contains("would_auth"))    r.would_auth    = j.at("would_auth");
    if (j.contains("elapsed_ms"))    r.elapsed_ms    = j.at("elapsed_ms");
    if (j.contains("reason"))        r.reason        = j.at("reason");
}

inline void to_json(json& j, const EnrollProgressResponse& r) {
    j = json{{"type", "enroll_progress"}, {"request_id", r.request_id},
             {"session", r.session}, {"frames_collected", r.frames_collected},
             {"frames_needed", r.frames_needed}, {"quality", r.quality},
             {"done", r.done}, {"hint", r.hint}};
}
inline void from_json(const json& j, EnrollProgressResponse& r) {
    j.at("session").get_to(r.session);
    j.at("request_id").get_to(r.request_id);
    if (j.contains("frames_collected")) r.frames_collected = j.at("frames_collected");
    if (j.contains("frames_needed"))    r.frames_needed    = j.at("frames_needed");
    if (j.contains("quality"))          r.quality          = j.at("quality");
    if (j.contains("done"))             r.done             = j.at("done");
    if (j.contains("hint"))             r.hint             = j.at("hint");
}

inline void to_json(json& j, const EnrollDoneResponse& r) {
    j = json{{"type", "enroll_done"}, {"request_id", r.request_id},
             {"label", r.label}, {"embeddings_saved", r.embeddings_saved},
             {"threshold", r.threshold}};
}
inline void from_json(const json& j, EnrollDoneResponse& r) {
    j.at("label").get_to(r.label);
    j.at("request_id").get_to(r.request_id);
    if (j.contains("embeddings_saved")) r.embeddings_saved = j.at("embeddings_saved");
    if (j.contains("threshold"))        r.threshold        = j.at("threshold");
}

inline void to_json(json& j, const ListResponse::Item& it) {
    j = json{{"label", it.label}, {"created", it.created},
             {"embeddings", it.embeddings}, {"threshold", it.threshold}};
}
inline void from_json(const json& j, ListResponse::Item& it) {
    j.at("label").get_to(it.label);
    if (j.contains("created"))    it.created    = j.at("created");
    if (j.contains("embeddings")) it.embeddings = j.at("embeddings");
    if (j.contains("threshold"))  it.threshold  = j.at("threshold");
}
inline void to_json(json& j, const ListResponse& r) {
    j = json{{"type", "list_result"}, {"request_id", r.request_id},
             {"enrollments", r.enrollments}};
}
inline void from_json(const json& j, ListResponse& r) {
    j.at("request_id").get_to(r.request_id);
    if (j.contains("enrollments")) r.enrollments = j.at("enrollments");
}

inline void to_json(json& j, const RemoveResponse& r) {
    j = json{{"type", "remove_result"}, {"request_id", r.request_id},
             {"ok", r.ok}};
}
inline void from_json(const json& j, RemoveResponse& r) {
    j.at("request_id").get_to(r.request_id);
    if (j.contains("ok")) r.ok = j.at("ok");
}

inline void to_json(json& j, const ErrorResponse& r) {
    j = json{{"type", "error"}, {"request_id", r.request_id},
             {"reason", r.reason}, {"detail", r.detail}};
}
inline void from_json(const json& j, ErrorResponse& r) {
    j.at("request_id").get_to(r.request_id);
    if (j.contains("reason")) r.reason = j.at("reason");
    if (j.contains("detail")) r.detail = j.at("detail");
}

// Convenience: parse an incoming request JSON object into the right variant.
inline AnyRequest parse_request(const json& j) {
    if (!j.contains("type") || !j.at("type").is_string())
        throw std::runtime_error("missing 'type'");
    const std::string t = j.at("type");
    if (t == "auth")           return j.get<AuthRequest>();
    if (t == "test")           return j.get<TestRequest>();
    if (t == "enroll_start")   return j.get<EnrollStartRequest>();
    if (t == "enroll_frame")   return j.get<EnrollFrameRequest>();
    if (t == "enroll_finish")  return j.get<EnrollFinishRequest>();
    if (t == "list")           return j.get<ListRequest>();
    if (t == "remove")         return j.get<RemoveRequest>();
    throw std::runtime_error("unknown request type: " + t);
}

inline json serialize_response(const AnyResponse& r) {
    return std::visit([](const auto& v) { return json(v); }, r);
}

} // namespace fastauth::proto
