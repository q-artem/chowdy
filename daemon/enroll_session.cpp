#include "daemon/enroll_session.hpp"

#include <cstdio>

namespace chowdy::daemon {

namespace {
constexpr auto kSessionTtl = std::chrono::minutes(5);
}

std::string EnrollSessionManager::create(uid_t uid, std::string label,
                                         int min_frames, int max_frames) {
    std::lock_guard<std::mutex> lock(mu_);
    reap_expired_locked();

    ++counter_;
    char id[40];
    std::snprintf(id, sizeof(id), "%u-%llu", uid,
                  static_cast<unsigned long long>(counter_));

    EnrollSession s;
    s.uid         = uid;
    s.label       = std::move(label);
    s.min_frames  = min_frames;
    s.max_frames  = max_frames;
    s.embeddings.reserve(static_cast<size_t>(max_frames));
    sessions_.emplace(id, std::move(s));
    return id;
}

EnrollSession* EnrollSessionManager::get(const std::string& id, uid_t uid) {
    std::lock_guard<std::mutex> lock(mu_);
    reap_expired_locked();
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return nullptr;
    if (it->second.uid != uid) return nullptr;
    it->second.last_touch = std::chrono::steady_clock::now();
    return &it->second;
}

void EnrollSessionManager::drop(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    sessions_.erase(id);
}

void EnrollSessionManager::reap_expired_locked() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (now - it->second.last_touch > kSessionTtl) it = sessions_.erase(it);
        else ++it;
    }
}

} // namespace chowdy::daemon
