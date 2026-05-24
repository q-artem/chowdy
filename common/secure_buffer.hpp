// Small RAII helper: an std::vector-like buffer that is mlock()'d for the
// duration of its lifetime and zero-wiped (`explicit_bzero`) on destruction.
//
// Intended for face embeddings in memory — see DESIGN.md §3 ("Утечка
// эмбеддинга из памяти после auth") and §16 checklist.
//
// Header-only so any TU using embeddings can include it without dragging
// extra libs.

#pragma once

#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <sys/mman.h>

namespace chowdy::common {

template <typename T>
class SecureBuffer {
public:
    SecureBuffer() = default;

    explicit SecureBuffer(size_t n) { resize(n); }

    SecureBuffer(const SecureBuffer&)            = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;

    SecureBuffer(SecureBuffer&& o) noexcept
        : data_(o.data_), size_(o.size_), locked_(o.locked_) {
        o.data_ = nullptr; o.size_ = 0; o.locked_ = false;
    }
    SecureBuffer& operator=(SecureBuffer&& o) noexcept {
        if (this != &o) { wipe_and_free(); data_ = o.data_; size_ = o.size_;
                          locked_ = o.locked_;
                          o.data_ = nullptr; o.size_ = 0; o.locked_ = false; }
        return *this;
    }

    ~SecureBuffer() { wipe_and_free(); }

    // Allocate `n` elements (default-constructed). Best-effort mlock; if it
    // fails (RLIMIT_MEMLOCK) we still allocate but `locked()` is false.
    void resize(size_t n) {
        wipe_and_free();
        if (n == 0) return;
        data_ = new T[n]();
        size_ = n;
        locked_ = (::mlock(data_, n * sizeof(T)) == 0);
    }

    T*       data()       noexcept { return data_; }
    const T* data() const noexcept { return data_; }
    size_t   size() const noexcept { return size_; }
    bool     locked() const noexcept { return locked_; }

private:
    void wipe_and_free() noexcept {
        if (data_) {
            // explicit_bzero is the canonical "guaranteed not optimised away"
            // wipe on glibc and musl.
            ::explicit_bzero(data_, size_ * sizeof(T));
            if (locked_) ::munlock(data_, size_ * sizeof(T));
            delete[] data_;
        }
        data_ = nullptr; size_ = 0; locked_ = false;
    }

    T*     data_   = nullptr;
    size_t size_   = 0;
    bool   locked_ = false;
};

} // namespace chowdy::common
