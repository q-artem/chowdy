// Embedding type + FA01 enrollment file format (see DESIGN.md §8).
//
// One file per enrollment label, layout (little-endian):
//   magic       u32   "FA01"  (0x31304146)
//   version     u16
//   created     u64   unix seconds
//   embedder_id u32   identity tag of the model file the embeddings came from
//   threshold   f32   per-enrollment cosine threshold (see DESIGN §6)
//   n           u32   number of embeddings stored
//   data        f32[n][512]   L2-normalised
//
// No checksum yet (data is short and on a 0700 dir); will revisit if we add
// at-rest encryption in v2.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace chowdy::common::encoding {

inline constexpr size_t   kEmbDim       = 512;
inline constexpr uint32_t kFileMagic    = 0x31304146u;  // "FA01"
inline constexpr uint16_t kFileVersion  = 1;

using Embedding = std::vector<float>;  // size() == kEmbDim, L2-normalised

struct EnrollmentFile {
    uint32_t                 embedder_id = 0;
    uint64_t                 created     = 0;       // unix seconds
    float                    threshold   = 0.0f;
    std::vector<Embedding>   embeddings;            // each L2-normalised, length kEmbDim
};

// Cheap, deterministic identity tag for a model file. Combines size and the
// first 16 bytes — not crypto, just enough to detect "you regenerated the
// embedder" mismatches at load time. Returns 0 if path is unreadable.
uint32_t simple_model_id(const std::filesystem::path& model_path);

// L2-normalise in place. If the vector is ~zero (norm < epsilon) all entries
// are set to zero so downstream cosine returns 0 rather than NaN.
void l2_normalize(Embedding& v);

// Cosine similarity for two equally-sized vectors, both assumed L2-normalised
// — i.e. this just computes the dot product. Caller is responsible for the
// pre-normalisation.
double cosine_sim_normed(const float* a, const float* b, size_t n);

// Pick the per-enrollment threshold as
//   max(global_floor, min_pairwise_cosine_sim - margin).
// Single-embedding enrollments fall back to global_floor.
float pick_threshold(const std::vector<Embedding>& embs,
                     float global_floor = 0.40f,
                     float margin       = 0.05f);

// Write the FA01 file atomically (write to .tmp + rename). Creates parent
// dirs with mode 0700. Throws std::runtime_error on I/O failure.
void save_enrollment(const std::filesystem::path& path, const EnrollmentFile& e);

// Returns nullopt if the file is missing, truncated, magic/version mismatched,
// or n is unreasonable (> 1000). Does NOT throw on simple I/O errors — the
// caller already knows the file might not exist (no enrollment yet).
std::optional<EnrollmentFile> load_enrollment(const std::filesystem::path& path);

} // namespace chowdy::common::encoding
