#include "common/encoding.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <stdexcept>

#include <sys/stat.h>
#include <unistd.h>

namespace fastauth::common::encoding {

uint32_t simple_model_id(const std::filesystem::path& model_path) {
    std::ifstream f(model_path, std::ios::binary);
    if (!f) return 0;
    f.seekg(0, std::ios::end);
    auto sz = static_cast<uint32_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::array<unsigned char, 16> head{};
    f.read(reinterpret_cast<char*>(head.data()), 16);
    uint32_t h = sz;
    for (auto b : head) h = h * 31u + b;
    return h;
}

void l2_normalize(Embedding& v) {
    double s = 0;
    for (float x : v) s += static_cast<double>(x) * x;
    if (s < 1e-12) {
        for (float& x : v) x = 0.f;
        return;
    }
    const float inv = static_cast<float>(1.0 / std::sqrt(s));
    for (float& x : v) x *= inv;
}

double cosine_sim_normed(const float* a, const float* b, size_t n) {
    double s = 0;
    for (size_t i = 0; i < n; ++i) s += static_cast<double>(a[i]) * b[i];
    return s;
}

float pick_threshold(const std::vector<Embedding>& embs, float global_floor, float margin) {
    if (embs.size() < 2) return global_floor;
    double mn = 1.0;
    for (size_t i = 0; i < embs.size(); ++i) {
        for (size_t j = i + 1; j < embs.size(); ++j) {
            double s = cosine_sim_normed(embs[i].data(), embs[j].data(), kEmbDim);
            if (s < mn) mn = s;
        }
    }
    const float candidate = static_cast<float>(mn) - margin;
    return std::max(global_floor, candidate);
}

namespace {

void write_all(std::ofstream& f, const void* p, size_t n) {
    f.write(static_cast<const char*>(p), static_cast<std::streamsize>(n));
    if (!f) throw std::runtime_error("write failed");
}

bool read_all(std::ifstream& f, void* p, size_t n) {
    f.read(static_cast<char*>(p), static_cast<std::streamsize>(n));
    return static_cast<size_t>(f.gcount()) == n;
}

} // namespace

void save_enrollment(const std::filesystem::path& path, const EnrollmentFile& e) {
    std::error_code ec;
    auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        // Best-effort: tighten the directory mode if we just created it.
        ::chmod(parent.c_str(), 0700);
    }

    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("cannot open " + tmp.string() + " for write");

        const uint32_t magic = kFileMagic;
        const uint16_t ver   = kFileVersion;
        const uint64_t created = e.created != 0
            ? e.created
            : static_cast<uint64_t>(std::time(nullptr));
        const uint32_t n = static_cast<uint32_t>(e.embeddings.size());

        write_all(f, &magic,         sizeof(magic));
        write_all(f, &ver,           sizeof(ver));
        write_all(f, &created,       sizeof(created));
        write_all(f, &e.embedder_id, sizeof(e.embedder_id));
        write_all(f, &e.threshold,   sizeof(e.threshold));
        write_all(f, &n,             sizeof(n));
        for (const auto& v : e.embeddings) {
            if (v.size() != kEmbDim)
                throw std::runtime_error("embedding wrong dim");
            write_all(f, v.data(), v.size() * sizeof(float));
        }
    }
    ::chmod(tmp.c_str(), 0600);

    std::filesystem::rename(tmp, path, ec);
    if (ec) throw std::runtime_error("rename: " + ec.message());
}

std::optional<EnrollmentFile> load_enrollment(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;

    uint32_t magic = 0;
    uint16_t ver   = 0;
    EnrollmentFile e;
    uint32_t n = 0;

    if (!read_all(f, &magic,         sizeof(magic)))         return std::nullopt;
    if (!read_all(f, &ver,           sizeof(ver)))           return std::nullopt;
    if (!read_all(f, &e.created,     sizeof(e.created)))     return std::nullopt;
    if (!read_all(f, &e.embedder_id, sizeof(e.embedder_id))) return std::nullopt;
    if (!read_all(f, &e.threshold,   sizeof(e.threshold)))   return std::nullopt;
    if (!read_all(f, &n,             sizeof(n)))             return std::nullopt;
    if (magic != kFileMagic || ver != kFileVersion)          return std::nullopt;
    if (n == 0 || n > 1000)                                  return std::nullopt;

    e.embeddings.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        e.embeddings[i].resize(kEmbDim);
        if (!read_all(f, e.embeddings[i].data(), kEmbDim * sizeof(float)))
            return std::nullopt;
    }
    return e;
}

} // namespace fastauth::common::encoding
