#include "daemon/enrollment_store.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include <sys/stat.h>
#include <unistd.h>

#include "common/logging.hpp"

namespace fastauth::daemon {

namespace {

std::filesystem::path uid_dir(const std::filesystem::path& base, uid_t uid) {
    return base / std::to_string(uid);
}

// Return the most-recent mtime under `dir`, or file_time_type::min() if
// the directory does not exist. The cache uses this as a freshness key:
// if any .enc file is added/removed/rewritten the value changes.
std::filesystem::file_time_type latest_mtime(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::file_time_type best = std::filesystem::file_time_type::min();
    if (!std::filesystem::exists(dir, ec)) return best;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".enc") continue;
        auto t = std::filesystem::last_write_time(e.path(), ec);
        if (!ec && t > best) best = t;
    }
    return best;
}

} // namespace

EnrollmentStore::EnrollmentStore(std::filesystem::path users_dir)
    : users_dir_(std::move(users_dir)) {}

std::vector<UserEnrollment> EnrollmentStore::for_uid(uid_t uid) {
    std::lock_guard<std::mutex> lock(mu_);

    const auto dir = uid_dir(users_dir_, uid);
    const auto mt  = latest_mtime(dir);

    auto it = cache_.find(uid);
    if (it != cache_.end() && it->second.mtime == mt) {
        return it->second.items;
    }

    CacheEntry e;
    e.mtime = mt;

    std::error_code ec;
    if (std::filesystem::exists(dir, ec)) {
        for (auto& de : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!de.is_regular_file()) continue;
            if (de.path().extension() != ".enc") continue;
            auto loaded = common::encoding::load_enrollment(de.path());
            if (!loaded) {
                common::log::warn("enrollment file unreadable",
                    {{"path", de.path().string()}});
                continue;
            }
            UserEnrollment u;
            u.label = de.path().stem().string();
            u.file  = std::move(*loaded);
            e.items.push_back(std::move(u));
        }
    }

    cache_[uid] = e;
    return cache_[uid].items;
}

void EnrollmentStore::save(uid_t uid, const std::string& label,
                           const common::encoding::EnrollmentFile& file) {
    if (label.empty() || label.find('/') != std::string::npos
        || label.find('\0') != std::string::npos) {
        throw std::runtime_error("invalid enrollment label");
    }
    const auto dir = uid_dir(users_dir_, uid);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ::chmod(dir.c_str(), 0700);

    const auto path = dir / (label + ".enc");
    common::encoding::save_enrollment(path, file);

    std::lock_guard<std::mutex> lock(mu_);
    cache_.erase(uid);   // force reload on next for_uid
}

int EnrollmentStore::remove(uid_t uid, const std::string& label) {
    const auto path = uid_dir(users_dir_, uid) / (label + ".enc");
    std::error_code ec;
    bool ok = std::filesystem::remove(path, ec);
    std::lock_guard<std::mutex> lock(mu_);
    cache_.erase(uid);
    return ok ? 1 : 0;
}

int EnrollmentStore::remove_all(uid_t uid) {
    const auto dir = uid_dir(users_dir_, uid);
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return 0;
    int n = 0;
    for (auto& de : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!de.is_regular_file()) continue;
        if (de.path().extension() != ".enc") continue;
        if (std::filesystem::remove(de.path(), ec)) ++n;
    }
    std::lock_guard<std::mutex> lock(mu_);
    cache_.erase(uid);
    return n;
}

} // namespace fastauth::daemon
