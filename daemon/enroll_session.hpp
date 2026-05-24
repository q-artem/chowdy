// Short-lived per-uid enrollment sessions.
//
// CLI flow (DESIGN.md §7):
//   1. enroll_start  → daemon allocates a session id, returns it.
//   2. enroll_frame  → daemon captures one frame, runs the pipeline, if
//                      quality passes appends the embedding, returns
//                      progress + hint. CLI calls in a loop until done.
//   3. enroll_finish → daemon picks a threshold from pairwise sims, saves
//                      to <users_dir>/<uid>/<label>.enc, drops session.
//
// Sessions live in memory only — daemon restart drops them. They're
// uid-bound: the SO_PEERCRED check on the handler ensures only the owner
// can drive their own session.

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

#include "common/encoding.hpp"

namespace fastauth::daemon {

struct EnrollSession {
    uid_t                                       uid = 0;
    std::string                                 label;
    int                                         min_frames = 5;
    int                                         max_frames = 15;
    std::vector<common::encoding::Embedding>    embeddings;     // L2-normalised
    int                                         attempts   = 0;
    std::chrono::steady_clock::time_point       last_touch = std::chrono::steady_clock::now();
};

class EnrollSessionManager {
public:
    // Allocate a fresh session id. Caller fills in label and bounds via the
    // returned pointer. Sessions older than 5 minutes are reaped on access.
    std::string create(uid_t uid, std::string label, int min_frames, int max_frames);

    // Returns nullptr if id is unknown, expired, or doesn't belong to uid.
    EnrollSession* get(const std::string& id, uid_t uid);

    void drop(const std::string& id);

private:
    void reap_expired_locked();

    std::mutex                                       mu_;
    std::unordered_map<std::string, EnrollSession>   sessions_;
    uint64_t                                         counter_ = 0;
};

} // namespace fastauth::daemon
