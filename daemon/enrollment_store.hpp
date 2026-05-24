// Per-uid enrollment lookup. On-disk layout matches DESIGN.md §8:
//   <users_dir>/<uid>/<label>.enc
// All embeddings for a uid live in one directory, owned 0700 by chowdy.
//
// The store caches loaded files in memory keyed by uid. A stat-based mtime
// check keeps the cache from going stale when chowdy-cli rewrites a file.

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

#include "common/encoding.hpp"

namespace chowdy::daemon {

struct UserEnrollment {
    std::string                       label;
    common::encoding::EnrollmentFile  file;
};

class EnrollmentStore {
public:
    explicit EnrollmentStore(std::filesystem::path users_dir);

    // Return every <label>.enc file for the given uid. Loads or refreshes
    // the in-memory cache as needed. Files that fail to parse are silently
    // skipped (with a warn log).
    std::vector<UserEnrollment> for_uid(uid_t uid);

    // Persist `file` to <users_dir>/<uid>/<label>.enc atomically and refresh
    // the cache for this uid. Creates the per-uid directory 0700.
    void save(uid_t uid, const std::string& label,
              const common::encoding::EnrollmentFile& file);

    // Remove a single label or all labels for a uid. Returns the number of
    // files removed.
    int remove(uid_t uid, const std::string& label);
    int remove_all(uid_t uid);

    const std::filesystem::path& users_dir() const noexcept { return users_dir_; }

private:
    struct CacheEntry {
        std::filesystem::file_time_type mtime;
        std::vector<UserEnrollment>     items;
    };

    std::filesystem::path                       users_dir_;
    std::mutex                                  mu_;
    std::unordered_map<uid_t, CacheEntry>       cache_;
};

} // namespace chowdy::daemon
