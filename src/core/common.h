#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace zb {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// A guest (32-bit) address.
using gaddr = u32;

inline void* hp(gaddr a) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(a));
}

template <typename T>
inline T* hp(gaddr a) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(a));
}

inline gaddr ga(const void* p) {
    return static_cast<gaddr>(reinterpret_cast<uintptr_t>(p));
}

// Logging priority constants (mirroring Android log priorities for compatibility)
enum {
    ANDROID_LOG_UNKNOWN = 0,
    ANDROID_LOG_DEFAULT = 1,
    ANDROID_LOG_VERBOSE = 2,
    ANDROID_LOG_DEBUG   = 3,
    ANDROID_LOG_INFO    = 4,
    ANDROID_LOG_WARN    = 5,
    ANDROID_LOG_ERROR   = 6,
    ANDROID_LOG_FATAL   = 7,
    ANDROID_LOG_SILENT  = 8,
};

inline bool g_debug = false;
inline bool g_log_to_stderr = true;

inline void log_print(int prio, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    FILE* out = (prio >= ANDROID_LOG_WARN) ? stderr : stdout;
    vfprintf(out, fmt, ap);
    fprintf(out, "\n");
    fflush(out);
    va_end(ap);
}

[[noreturn]] inline void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[FATAL] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(ap);
    std::abort();
}

inline gaddr ga_checked(const void* p, const char* what) {
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    if (v > 0xFFFFFFFFu) {
        fatal("host pointer %p (%s) is outside the guest address space", p, what);
    }
    return static_cast<gaddr>(v);
}

// Typed guest pointer used in thunk signatures.
template <typename T>
struct gp {
    u32 a = 0;
    T* get() const { return hp<T>(a); }
    operator T*() const { return get(); }
    T* operator->() const { return get(); }
    explicit operator bool() const { return a != 0; }
};

template <typename T>
inline T rd(gaddr a) {
    T v;
    std::memcpy(&v, hp(a), sizeof(T));
    return v;
}

template <typename T>
inline void wr(gaddr a, T v) {
    std::memcpy(hp(a), &v, sizeof(T));
}

} // namespace zb

namespace sbr64 = zb;

#define ZLOGI(...) ::zb::log_print(ANDROID_LOG_INFO, __VA_ARGS__)
#define ZLOGW(...) ::zb::log_print(ANDROID_LOG_WARN, __VA_ARGS__)
#define ZLOGE(...) ::zb::log_print(ANDROID_LOG_ERROR, __VA_ARGS__)
#define ZLOGD(...) do { if (::zb::g_debug) ::zb::log_print(ANDROID_LOG_DEBUG, __VA_ARGS__); } while (0)
