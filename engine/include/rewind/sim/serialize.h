#pragma once

#include "rewind/core/types.h"

#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace rw {

/// Append-only byte buffer. Games write their whole state into one of these
/// every tick, so keep it cheap: plain-old-data goes in with a memcpy.
class Writer {
public:
    template <class T> void pod(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "pod() needs a trivially copyable type");
        append(&value, sizeof value);
    }

    template <class T> void vec(const std::vector<T>& values) {
        static_assert(std::is_trivially_copyable_v<T>, "vec() needs trivially copyable elements");
        pod<u64>(values.size());
        if (!values.empty()) append(values.data(), values.size() * sizeof(T));
    }

    void str(const std::string& s) {
        pod<u64>(s.size());
        append(s.data(), s.size());
    }

    void append(const void* data, usize size) {
        const u8* bytes = static_cast<const u8*>(data);
        bytes_.insert(bytes_.end(), bytes, bytes + size);
    }

    void clear() { bytes_.clear(); }
    std::vector<u8>& bytes() { return bytes_; }
    const std::vector<u8>& bytes() const { return bytes_; }

private:
    std::vector<u8> bytes_;
};

/// Reads back what a Writer produced, in the same order. Never throws: a
/// truncated buffer zero-fills and flips ok() to false.
class Reader {
public:
    Reader(const u8* data, usize size) : cursor_(data), end_(data + size) {}
    explicit Reader(const std::vector<u8>& bytes) : Reader(bytes.data(), bytes.size()) {}

    template <class T> T pod() {
        T value{};
        take(&value, sizeof value);
        return value;
    }

    template <class T> void pod(T& value) { take(&value, sizeof value); }

    template <class T> void vec(std::vector<T>& values) {
        const u64 count = pod<u64>();
        if (count * sizeof(T) > remaining()) {
            failed_ = true;
            values.clear();
            return;
        }
        values.resize(static_cast<usize>(count));
        if (count) take(values.data(), static_cast<usize>(count) * sizeof(T));
    }

    void str(std::string& s) {
        const u64 count = pod<u64>();
        if (count > remaining()) {
            failed_ = true;
            s.clear();
            return;
        }
        s.assign(reinterpret_cast<const char*>(cursor_), static_cast<usize>(count));
        cursor_ += count;
    }

    /// Raw bytes, zero-filled (and ok() cleared) if the stream is too short.
    void bytes(void* dst, usize size) { take(dst, size); }

    bool ok() const { return !failed_; }
    usize remaining() const { return static_cast<usize>(end_ - cursor_); }

private:
    void take(void* dst, usize size) {
        if (size > remaining()) {
            failed_ = true;
            std::memset(dst, 0, size);
            return;
        }
        std::memcpy(dst, cursor_, size);
        cursor_ += size;
    }

    const u8* cursor_;
    const u8* end_;
    bool failed_ = false;
};

/// FNV-1a, 64-bit. Used to fingerprint serialized state so the engine can
/// prove that re-simulating a tick reproduced it exactly.
inline u64 hash_bytes(const void* data, usize size, u64 seed = 14695981039346656037ull) {
    const u8* bytes = static_cast<const u8*>(data);
    u64 h = seed;
    for (usize i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ull;
    }
    return h;
}

inline u64 hash_bytes(const std::vector<u8>& bytes) { return hash_bytes(bytes.data(), bytes.size()); }

} // namespace rw
