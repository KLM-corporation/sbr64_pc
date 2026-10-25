// Bionic libc, libm, libstdc++, libdl, libz, and liblog High-Level Emulation for Windows
#include "bionic_hle.h"
#include "core/thunk.h"
#include "core/sync_win32.h"
#include "core/mem_win32.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <deque>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#endif

namespace zb::bionic {

// Stdio emulation constants (Bionic LP32 struct __sFILE is 84 bytes)
constexpr u32 kGuestFileSize = 84;
constexpr u16 kSEOF = 0x0020;
constexpr u16 kSERR = 0x0040;

// Mutex / Futex bit layout on Bionic LP32
constexpr u32 kTypeMask       = 0xC000;
constexpr u32 kShared         = 0x2000;
constexpr u32 kState          = 3;
constexpr u32 kCountInc       = 4;
constexpr u32 kCountMask      = 0x1FFC;
constexpr u32 kTypeErrorCheck = 0x8000;
constexpr u32 kGuestDefaultStack = 1024 * 1024;

namespace {

// ==============================================================================
// 1. Static State & Deduplication Tables
// ==============================================================================
std::mutex g_static_lock;
std::unordered_map<std::string, gaddr> g_static_strings;

std::mutex g_env_lock;
std::unordered_map<std::string, gaddr> g_env_cache;

std::string g_save_dir = ".\\save";
std::string g_obb_dir  = "data\\obb";

std::mutex g_files_lock;
std::unordered_map<gaddr, FILE*> g_files;
gaddr g_sF = 0; // Base address of guest __sF array (stdin, stdout, stderr)

inline u32* word_ptr(gaddr a) {
    return hp<u32>(a);
}

} // namespace

// ==============================================================================
// 2. Memory Emulation Functions
// ==============================================================================
gaddr hle_malloc(gsize n) {
    // Boundary check: SIZE_MAX or requests >= 0x80000000 must fail cleanly with NULL
    if (n >= 0x80000000u || n == 0) return 0;
    return mem::gmalloc(n);
}

gaddr hle_calloc(gsize n, gsize s) {
    if (n != 0 && s > 0xFFFFFFFFu / n) return 0;
    if (static_cast<u64>(n) * s >= 0x80000000u) return 0;
    return mem::gcalloc(n, s);
}

gaddr hle_realloc(gaddr p, gsize n) {
    if (n >= 0x80000000u) return 0;
    return mem::grealloc(p, n);
}

void hle_free(gaddr p) {
    if (p) mem::gfree(p);
}

gaddr hle_memalign(gsize a, gsize n) {
    if (n >= 0x80000000u) return 0;
    return mem::gmemalign(a, n);
}

gsize hle_usable_size(gaddr p) {
    return p ? static_cast<gsize>(mem::gusable_size(p)) : 0;
}

gaddr hle_memcpy(gaddr d, gaddr s, gsize n) {
    if (d && s && n) std::memcpy(hp(d), hp(s), n);
    return d;
}

gaddr hle_memmove(gaddr d, gaddr s, gsize n) {
    if (d && s && n) std::memmove(hp(d), hp(s), n);
    return d;
}

gaddr hle_memset(gaddr d, i32 c, gsize n) {
    if (d && n) std::memset(hp(d), c, n);
    return d;
}

i32 hle_memcmp(gaddr a, gaddr b, gsize n) {
    if (!a || !b || !n) return 0;
    return std::memcmp(hp(a), hp(b), n);
}

gaddr hle_memchr(gaddr s, i32 c, gsize n) {
    if (!s || !n) return 0;
    void* r = std::memchr(hp(s), c, n);
    return r ? ga(r) : 0;
}

// ==============================================================================
// 3. String & Character Emulation
// ==============================================================================
gsize hle_strlen(gcstr s) {
    if (!s.a) return 0;
    return static_cast<gsize>(std::strlen(s.get()));
}

i32 hle_strcmp(gcstr a, gcstr b) {
    if (!a.a || !b.a) return (a.a == b.a) ? 0 : (a.a ? 1 : -1);
    return std::strcmp(a.get(), b.get());
}

i32 hle_strncmp(gcstr a, gcstr b, gsize n) {
    if (n == 0) return 0;
    if (!a.a || !b.a) return (a.a == b.a) ? 0 : (a.a ? 1 : -1);
    return std::strncmp(a.get(), b.get(), n);
}

gaddr hle_strcpy(gstr d, gcstr s) {
    if (!d.a || !s.a) return d.a;
    std::strcpy(d.get(), s.get());
    return d.a;
}

gaddr hle_strncpy(gstr d, gcstr s, gsize n) {
    if (!d.a || n == 0) return d.a;
    if (!s.a) {
        std::memset(d.get(), 0, n);
        return d.a;
    }
    std::strncpy(d.get(), s.get(), n);
    return d.a;
}

gaddr hle_strcat(gstr d, gcstr s) {
    if (!d.a || !s.a) return d.a;
    std::strcat(d.get(), s.get());
    return d.a;
}

gaddr hle_strchr(gcstr s, i32 c) {
    if (!s.a) return 0;
    const char* r = std::strchr(s.get(), c);
    return r ? ga(r) : 0;
}

gaddr hle_strrchr(gcstr s, i32 c) {
    if (!s.a) return 0;
    const char* r = std::strrchr(s.get(), c);
    return r ? ga(r) : 0;
}

gaddr hle_strstr(gcstr h, gcstr n) {
    if (!h.a || !n.a) return 0;
    const char* r = std::strstr(h.get(), n.get());
    return r ? ga(r) : 0;
}

i32 hle_atoi(gcstr s) {
    return s.a ? std::atoi(s.get()) : 0;
}

glong hle_atol(gcstr s) {
    return s.a ? static_cast<glong>(std::strtol(s.get(), nullptr, 10)) : 0;
}

double hle_strtod(JitCtx& j, gcstr s, gaddr endp) {
    if (!s.a) return 0.0;
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(s.get(), &end);
    write_ptr(endp, end);
    if (errno) sync_errno(j);
    return v;
}

i32 hle_tolower(i32 c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

i32 hle_isspace(i32 c) {
    return (c == ' ' || (c >= '\t' && c <= '\r')) ? 1 : 0;
}

i32 hle_isupper(i32 c) {
    return (c >= 'A' && c <= 'Z') ? 1 : 0;
}

gaddr hle_strerror(JitCtx& j, i32 e) {
    gaddr buf = tls_buffer(j, tls::kStrerror);
    const char* msg = std::strerror(e);
    snprintf(hp<char>(buf), 256, "%s", msg ? msg : "Unknown error");
    return buf;
}

gaddr hle_getenv(gcstr name) {
    if (!name.a) return 0;
    const char* v = std::getenv(name.get());
    if (!v) return 0;
    std::lock_guard lk(g_env_lock);
    std::string key = std::string(name.get()) + "=" + v;
    auto it = g_env_cache.find(key);
    if (it != g_env_cache.end()) return it->second;
    gaddr g = mem::gstrdup(v);
    g_env_cache.emplace(key, g);
    return g;
}

glong hle_lrand48() {
    return static_cast<glong>(std::rand() & 0x7FFFFFFF);
}

void hle_qsort(gaddr base, gsize n, gsize size, gaddr compar) {
    if (n < 2 || size == 0 || !base) return;
    std::vector<u32> idx(n), tmp(n);
    for (u32 i = 0; i < n; ++i) idx[i] = i;
    auto less_eq = [&](u32 a, u32 b) {
        u64 r = cpu::call(compar, cpu::Args().u32v(base + a * size).u32v(base + b * size));
        return static_cast<i32>(r) <= 0;
    };
    for (u32 width = 1; width < n; width *= 2) {
        for (u32 lo = 0; lo < n; lo += 2 * width) {
            u32 mid = std::min(lo + width, static_cast<u32>(n));
            u32 hi = std::min(lo + 2 * width, static_cast<u32>(n));
            u32 i = lo, k = mid, o = lo;
            while (i < mid && k < hi) tmp[o++] = less_eq(idx[i], idx[k]) ? idx[i++] : idx[k++];
            while (i < mid) tmp[o++] = idx[i++];
            while (k < hi) tmp[o++] = idx[k++];
        }
        idx.swap(tmp);
    }
    std::vector<u8> sorted(static_cast<size_t>(n) * size);
    for (u32 i = 0; i < n; ++i) {
        std::memcpy(&sorted[static_cast<size_t>(i) * size], hp(base + idx[i] * size), size);
    }
    std::memcpy(hp(base), sorted.data(), sorted.size());
}

// ==============================================================================
// 4. Math Emulation (libm)
// ==============================================================================
float m_sinf(float x)   { return std::sin(x); }
float m_cosf(float x)   { return std::cos(x); }
float m_tanf(float x)   { return std::tan(x); }
float m_atan2f(float y, float x) { return std::atan2(y, x); }
float m_sqrtf(float x)  { return std::sqrt(x); }
float m_powf(float x, float y) { return std::pow(x, y); }
float m_floorf(float x) { return std::floor(x); }
float m_ceilf(float x)  { return std::ceil(x); }
float m_log10f(float x) { return std::log10(x); }
double m_sin(double x)  { return std::sin(x); }

// ==============================================================================
// 5. C++ Runtime / ABI Operators & Helpers
// ==============================================================================
gaddr cxx_new(gsize n) {
    gaddr p = mem::gmalloc(n ? n : 1);
    if (!p) fatal("guest operator new(%u): out of memory", n);
    return p;
}

void cxx_delete(gaddr p) {
    if (p) mem::gfree(p);
}

namespace {
std::mutex g_guard_lock;
std::condition_variable g_guard_cv;
}

i32 cxa_guard_acquire(gp<u32> g) {
    if (!g.a) return 0;
    std::unique_lock lk(g_guard_lock);
    for (;;) {
        u32 v = *g.get();
        if (v & 1) return 0; // Already initialized
        if (!(v & 0x100)) {
            *g.get() = v | 0x100; // In-progress
            return 1;
        }
        g_guard_cv.wait(lk);
    }
}

void cxa_guard_release(gp<u32> g) {
    if (!g.a) return;
    {
        std::lock_guard lk(g_guard_lock);
        *g.get() = (*g.get() & ~0x100u) | 1;
    }
    g_guard_cv.notify_all();
}

[[noreturn]] void cxa_pure_virtual(JitCtx& j) {
    cpu::dump_state(j, "__cxa_pure_virtual");
    fatal("guest called a pure virtual function");
}

i32 cxa_atexit(gaddr, gaddr, gaddr) { return 0; }
void cxa_finalize(gaddr) {}
i32 aeabi_atexit(gaddr, gaddr, gaddr) { return 0; }

void aeabi_memcpy(gaddr d, gaddr s, gsize n) {
    if (d && s && n) std::memcpy(hp(d), hp(s), n);
}

void aeabi_memmove(gaddr d, gaddr s, gsize n) {
    if (d && s && n) std::memmove(hp(d), hp(s), n);
}

void aeabi_memset(gaddr d, gsize n, i32 c) {
    // Note ARM EABI parameter ordering: (dest, size, value)
    if (d && n) std::memset(hp(d), c, n);
}

// ==============================================================================
// 6. Stdio & File Handle Emulation
// ==============================================================================
void update_file_flags(gaddr gf, FILE* f) {
    if (gf < 0x1000) return;
    u16 flags = rd<u16>(gf + 12) & ~(kSEOF | kSERR);
    if (feof(f)) flags |= kSEOF;
    if (ferror(f)) flags |= kSERR;
    wr<u16>(gf + 12, flags);
}

FILE* need_file(JitCtx& j, gaddr gf) {
    FILE* f = host_file(gf);
    if (!f) {
        ZLOGW("unknown guest FILE* 0x%08x", gf);
        j.set_errno(9 /* EBADF */);
    }
    return f;
}

gaddr s_fopen(JitCtx& j, gcstr path, gcstr mode) {
    if (!path.a || !mode.a) {
        j.set_errno(22 /* EINVAL */);
        return 0;
    }
    std::string host_path = translate_path(path.get());
    std::string host_mode = mode.get();
    // Enforce binary mode on Windows if not already specified
    if (host_mode.find('b') == std::string::npos) {
        host_mode += 'b';
    }
    std::error_code ec;
    std::filesystem::path fp(host_path);
    if (fp.has_parent_path() && host_mode.find_first_of("wa+") != std::string::npos) {
        std::filesystem::create_directories(fp.parent_path(), ec);
    }
    FILE* f = std::fopen(host_path.c_str(), host_mode.c_str());
    if (g_debug) {
        ZLOGD("fopen(%s -> %s, %s) -> %p [from %s]", path.get(), host_path.c_str(), host_mode.c_str(), f,
              cpu::symbolize(j.r()[14]).c_str());
    }
    if (!f) {
        sync_errno(j);
        return 0;
    }
    return guest_file(f);
}

i32 s_fclose(JitCtx& j, gaddr gf) {
    FILE* f = need_file(j, gf);
    if (!f) return EOF;
    if (f == stdin || f == stdout || f == stderr) return 0;
    forget_file(gf);
    int r = std::fclose(f);
    if (r) sync_errno(j);
    return r;
}

i32 s_fflush(JitCtx& j, gaddr gf) {
    if (!gf) return std::fflush(nullptr);
    FILE* f = need_file(j, gf);
    return f ? std::fflush(f) : EOF;
}

gsize s_fread(JitCtx& j, gaddr buf, gsize size, gsize n, gaddr gf) {
    FILE* f = need_file(j, gf);
    if (!f || size == 0 || n == 0 || !buf) return 0;
    size_t r = std::fread(hp(buf), size, n, f);
    update_file_flags(gf, f);
    if (r < n && std::ferror(f)) sync_errno(j);
    return static_cast<gsize>(r);
}

gsize s_fwrite(JitCtx& j, gaddr buf, gsize size, gsize n, gaddr gf) {
    FILE* f = need_file(j, gf);
    if (!f || size == 0 || n == 0 || !buf) return 0;
    size_t w = std::fwrite(hp(buf), size, n, f);
    update_file_flags(gf, f);
    if (w < n && std::ferror(f)) sync_errno(j);
    return static_cast<gsize>(w);
}

i32 s_fgetc(JitCtx& j, gaddr gf) {
    FILE* f = need_file(j, gf);
    if (!f) return EOF;
    int c = std::fgetc(f);
    update_file_flags(gf, f);
    return c;
}

i32 s_fputc(JitCtx& j, i32 c, gaddr gf) {
    FILE* f = need_file(j, gf);
    if (!f) return EOF;
    int r = std::fputc(c, f);
    update_file_flags(gf, f);
    return r;
}

i32 s_fputs(JitCtx& j, gcstr s, gaddr gf) {
    FILE* f = need_file(j, gf);
    if (!f || !s.a) return EOF;
    int r = std::fputs(s.get(), f);
    update_file_flags(gf, f);
    return (r >= 0) ? 0 : EOF;
}

i32 s_ungetc(JitCtx& j, i32 c, gaddr gf) {
    FILE* f = need_file(j, gf);
    if (!f) return EOF;
    int r = std::ungetc(c, f);
    update_file_flags(gf, f);
    return r;
}

i32 s_fseek(JitCtx& j, gaddr gf, glong off, i32 whence) {
    FILE* f = need_file(j, gf);
    if (!f) return -1;
    int r = std::fseek(f, off, whence);
    if (r) sync_errno(j);
    update_file_flags(gf, f);
    return r;
}

glong s_ftell(JitCtx& j, gaddr gf) {
    FILE* f = need_file(j, gf);
    if (!f) return -1;
    long r = std::ftell(f);
    if (r < 0) sync_errno(j);
    return static_cast<glong>(r);
}

// -----------------------------------------------------------------------------
// Variadic Formatting & Scanning Engines
// -----------------------------------------------------------------------------
namespace {
struct GVa {
    u32 ap;
    u32 next32() {
        u32 v = rd<u32>(ap);
        ap += 4;
        return v;
    }
    u64 next64() {
        ap = (ap + 7) & ~7u;
        u64 v = rd<u64>(ap);
        ap += 8;
        return v;
    }
    double nextd() {
        u64 b = next64();
        double d;
        std::memcpy(&d, &b, 8);
        return d;
    }
};
} // namespace

i32 copy_out(gaddr buf, u32 cap, const std::string& s) {
    if (cap > 0 && buf != 0) {
        u32 n = std::min<u32>(cap - 1, static_cast<u32>(s.size()));
        std::memcpy(hp(buf), s.data(), n);
        wr<u8>(buf + n, 0);
    }
    return static_cast<i32>(s.size());
}

void s_snprintf(JitCtx& j) {
    ArgReader r(j);
    u32 buf = r.w32(), n = r.w32(), fmt = r.w32();
    u32 ap = r.va_list_here();
    std::string str = vformat(hp<char>(fmt), ap);
    j.set_ret(static_cast<u32>(copy_out(buf, n, str)));
}

void s_sprintf(JitCtx& j) {
    ArgReader r(j);
    u32 buf = r.w32(), fmt = r.w32();
    u32 ap = r.va_list_here();
    std::string str = vformat(hp<char>(fmt), ap);
    j.set_ret(static_cast<u32>(copy_out(buf, static_cast<u32>(str.size()) + 1, str)));
}

void s_vsprintf(JitCtx& j) {
    ArgReader r(j);
    u32 buf = r.w32(), fmt = r.w32(), ap = r.w32();
    std::string str = vformat(hp<char>(fmt), ap);
    j.set_ret(static_cast<u32>(copy_out(buf, static_cast<u32>(str.size()) + 1, str)));
}

void s_fprintf(JitCtx& j) {
    ArgReader r(j);
    u32 gf = r.w32(), fmt = r.w32();
    u32 ap = r.va_list_here();
    FILE* f = need_file(j, gf);
    std::string s = vformat(hp<char>(fmt), ap);
    if (f) std::fwrite(s.data(), 1, s.size(), f);
    j.set_ret(static_cast<u32>(s.size()));
}

void s_sscanf(JitCtx& j) {
    ArgReader r(j);
    u32 str = r.w32(), fmt = r.w32();
    u32 ap = r.va_list_here();
    j.set_ret(static_cast<u32>(vscan_string(hp<char>(str), hp<char>(fmt), ap)));
}

// ==============================================================================
// 7. Threading & Synchronization (Futex on Win32)
// ==============================================================================
int mutex_lock(GuestThread* t, gaddr m) {
    u32* w = word_ptr(m);
    u32 tid = t->id;
    u32 v = __atomic_load_n(w, __ATOMIC_ACQUIRE);
    u32 type = v & kTypeMask;
    u32 base = v & (kTypeMask | kShared);

    if (type == 0) { // Normal mutex
        u32 expected = base;
        if (__atomic_compare_exchange_n(w, &expected, base | 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return 0;
        }
        while ((__atomic_exchange_n(w, base | 2, __ATOMIC_ACQUIRE) & kState) != 0) {
            sync::futex_wait(m, base | 2, nullptr);
        }
        return 0;
    }

    // Recursive / Errorcheck mutex
    if ((v & kState) != 0 && (v >> 16) == tid) {
        if (type == kTypeErrorCheck) return 35 /* EDEADLK */;
        if ((v & kCountMask) == kCountMask) return 11 /* EAGAIN */;
        __atomic_fetch_add(w, kCountInc, __ATOMIC_RELAXED);
        return 0;
    }

    u32 expected = base;
    if (__atomic_compare_exchange_n(w, &expected, base | 1 | (tid << 16), false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return 0;
    }
    for (;;) {
        u32 cur = __atomic_load_n(w, __ATOMIC_RELAXED);
        if ((cur & kState) == 0) {
            u32 e = cur;
            if (__atomic_compare_exchange_n(w, &e, base | 2 | (tid << 16), false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                return 0;
            }
            continue;
        }
        if ((cur & kState) == 1) {
            u32 e = cur;
            u32 want = (cur & ~kState) | 2;
            if (!__atomic_compare_exchange_n(w, &e, want, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) continue;
            cur = want;
        }
        sync::futex_wait(m, cur, nullptr);
    }
}

int mutex_trylock(GuestThread* t, gaddr m) {
    u32* w = word_ptr(m);
    u32 v = __atomic_load_n(w, __ATOMIC_ACQUIRE);
    u32 type = v & kTypeMask;
    u32 base = v & (kTypeMask | kShared);

    if (type != 0 && (v & kState) != 0 && (v >> 16) == t->id) {
        if (type == kTypeErrorCheck) return 16 /* EBUSY */;
        if ((v & kCountMask) == kCountMask) return 11 /* EAGAIN */;
        __atomic_fetch_add(w, kCountInc, __ATOMIC_RELAXED);
        return 0;
    }
    u32 expected = base;
    u32 locked = base | 1 | (type ? (t->id << 16) : 0);
    return __atomic_compare_exchange_n(w, &expected, locked, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 0 : 16 /* EBUSY */;
}

int mutex_unlock(GuestThread* t, gaddr m) {
    u32* w = word_ptr(m);
    u32 v = __atomic_load_n(w, __ATOMIC_RELAXED);
    u32 type = v & kTypeMask;
    u32 base = v & (kTypeMask | kShared);

    if (type != 0) {
        if ((v & kState) == 0 || (v >> 16) != t->id) return 1 /* EPERM */;
        if (v & kCountMask) {
            __atomic_fetch_sub(w, kCountInc, __ATOMIC_RELEASE);
            return 0;
        }
    }
    u32 old = __atomic_exchange_n(w, base, __ATOMIC_RELEASE);
    if ((old & kState) == 2) sync::futex_wake(m, 1);
    return 0;
}

i32 p_mutex_init(gaddr m, gaddr attr) {
    u32 type = attr ? (rd<u32>(attr) & 0xF) : 0;
    if (type > 2) return 22 /* EINVAL */;
    wr<u32>(m, type << 14);
    return 0;
}
i32 p_mutex_destroy(gaddr m) { return (rd<u32>(m) & kState) ? 16 /* EBUSY */ : 0; }
i32 p_mutex_lock(JitCtx& j, gaddr m) { return mutex_lock(j.thread, m); }
i32 p_mutex_trylock(JitCtx& j, gaddr m) { return mutex_trylock(j.thread, m); }
i32 p_mutex_unlock(JitCtx& j, gaddr m) { return mutex_unlock(j.thread, m); }

int cond_wait(GuestThread* t, gaddr c, gaddr m, const struct timespec* rel) {
    u32 seq = __atomic_load_n(word_ptr(c), __ATOMIC_RELAXED);
    mutex_unlock(t, m);
    int r = sync::futex_wait(c, seq, rel);
    int err = (r != 0) ? 110 /* ETIMEDOUT */ : 0;
    mutex_lock(t, m);
    return err;
}

i32 p_cond_init(gaddr c, gaddr) { wr<u32>(c, 0); return 0; }
i32 p_cond_destroy(gaddr) { return 0; }
i32 p_cond_wait(JitCtx& j, gaddr c, gaddr m) { return cond_wait(j.thread, c, m, nullptr); }
i32 p_cond_signal(gaddr c) {
    __atomic_fetch_add(word_ptr(c), 4u, __ATOMIC_RELEASE);
    sync::futex_wake(c, 1);
    return 0;
}
i32 p_cond_broadcast(gaddr c) {
    __atomic_fetch_add(word_ptr(c), 4u, __ATOMIC_RELEASE);
    sync::futex_wake(c, INT_MAX);
    return 0;
}

// Semaphore implementation
i32 sem_init_(gaddr s, i32, u32 value) { wr<u32>(s, value); return 0; }
i32 sem_post_(gaddr s) {
    __atomic_fetch_add(word_ptr(s), 1u, __ATOMIC_RELEASE);
    sync::futex_wake(s, 1);
    return 0;
}
i32 sem_wait_(gaddr s) {
    u32* w = word_ptr(s);
    for (;;) {
        u32 v = __atomic_load_n(w, __ATOMIC_ACQUIRE);
        if (v > 0) {
            if (__atomic_compare_exchange_n(w, &v, v - 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
            continue;
        }
        sync::futex_wait(s, 0, nullptr);
    }
}
i32 sem_trywait_(JitCtx& j, gaddr s) {
    u32* w = word_ptr(s);
    u32 v = __atomic_load_n(w, __ATOMIC_ACQUIRE);
    while (v > 0) {
        if (__atomic_compare_exchange_n(w, &v, v - 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
    }
    j.set_errno(11 /* EAGAIN */);
    return -1;
}

// Pthread keys (TLS slots)
namespace {
struct KeyRec {
    bool used = false;
    gaddr dtor = 0;
};
std::mutex g_key_lock;
std::array<KeyRec, 128> g_keys;
}

i32 p_key_create(gp<u32> key, gaddr dtor) {
    std::lock_guard lk(g_key_lock);
    for (u32 i = 0; i < g_keys.size(); ++i) {
        if (!g_keys[i].used) {
            g_keys[i] = {true, dtor};
            *key.get() = i;
            return 0;
        }
    }
    return 11 /* EAGAIN */;
}
i32 p_key_delete(u32 key) {
    std::lock_guard lk(g_key_lock);
    if (key >= g_keys.size() || !g_keys[key].used) return 22 /* EINVAL */;
    g_keys[key] = {};
    return 0;
}
gaddr p_getspecific(JitCtx& j, u32 key) { return key < 128 ? j.thread->keys[key] : 0; }
i32 p_setspecific(JitCtx& j, u32 key, gaddr v) {
    if (key >= 128) return 22 /* EINVAL */;
    j.thread->keys[key] = v;
    return 0;
}

// Thread creation
namespace {
struct ThreadRunner {
    GuestThread* t;
    gaddr fn;
    gaddr arg;
};

void guest_thread_worker(ThreadRunner* r) {
    GuestThread* t = r->t;
    gaddr fn = r->fn;
    gaddr arg = r->arg;
    delete r;

    cpu::adopt_thread(t);
    cpu::call(fn, cpu::Args().u32v(arg));
    cpu::run_thread_exit_hooks(t);
}
}

i32 p_create(gp<u32> out, gaddr attr, gaddr fn, gaddr arg) {
    u32 stack = kGuestDefaultStack;
    if (attr) {
        u32 s = rd<u32>(attr + 8);
        if (s >= 16384) stack = s;
    }
    GuestThread* t = cpu::new_thread_state(stack);
    *out.get() = t->tls;

    auto* runner = new ThreadRunner{t, fn, arg};
    std::thread th(guest_thread_worker, runner);
    th.detach();
    return 0;
}

i32 p_attr_init(gaddr a) {
    wr<u32>(a + 0, 0);
    wr<u32>(a + 4, 0);
    wr<u32>(a + 8, kGuestDefaultStack);
    return 0;
}
i32 p_attr_setdetachstate(gaddr a, i32 state) {
    u32 f = rd<u32>(a);
    wr<u32>(a, state == 1 ? (f | 1) : (f & ~1u));
    return 0;
}
i32 p_setschedparam(JitCtx&, gaddr, i32, gp<i32>) { return 0; }
i32 s_yield() { std::this_thread::yield(); return 0; }

// ==============================================================================
// 8. Time & Clocks
// ==============================================================================
gtime hle_time(gp<i32> out) {
    gtime now = static_cast<gtime>(std::time(nullptr));
    if (out.a) *out.get() = now;
    return now;
}

i32 hle_clock_gettime(JitCtx& j, i32 clk, gp<GTimespec> ts) {
    if (!ts.a) return -1;
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() % 1000000000;
    ts->tv_sec = static_cast<i32>(sec);
    ts->tv_nsec = static_cast<i32>(nsec);
    (void)clk; (void)j;
    return 0;
}

gaddr hle_localtime(JitCtx& j, gp<i32> t) {
    if (!t.a) return 0;
    std::time_t v = *t.get();
    struct tm r{};
#if defined(_WIN32)
    localtime_s(&r, &v);
#else
    localtime_r(&v, &r);
#endif
    gaddr out = tls_buffer(j, tls::kTm);
    i32 f[9] = {r.tm_sec, r.tm_min, r.tm_hour, r.tm_mday, r.tm_mon, r.tm_year, r.tm_wday, r.tm_yday, r.tm_isdst};
    std::memcpy(hp(out), f, sizeof(f));
    return out;
}

gtime hle_mktime(gaddr g) {
    if (!g) return -1;
    i32 f[9];
    std::memcpy(f, hp(g), sizeof(f));
    struct tm t{};
    t.tm_sec = f[0]; t.tm_min = f[1]; t.tm_hour = f[2];
    t.tm_mday = f[3]; t.tm_mon = f[4]; t.tm_year = f[5];
    t.tm_wday = f[6]; t.tm_yday = f[7]; t.tm_isdst = f[8];
    return static_cast<gtime>(std::mktime(&t));
}

double hle_difftime(gtime a, gtime b) {
    return static_cast<double>(a) - static_cast<double>(b);
}

i32 hle_usleep(u32 us) {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
    return 0;
}

// ==============================================================================
// 9. Process, OS & Stubs
// ==============================================================================
[[noreturn]] void hle_abort(JitCtx& j) {
    cpu::dump_state(j, "abort()");
    fatal("guest called abort()");
}

[[noreturn]] void hle_assert2(JitCtx& j, gcstr file, i32 line, gcstr func, gcstr msg) {
    cpu::dump_state(j, "__assert2");
    fatal("guest assertion failed: %s:%d (%s): %s", file.get(), line, func.get(), msg.get());
}

gaddr hle_errno_location(JitCtx& j) {
    return j.thread->tls + tls::kErrno;
}

i32 hle_chdir(gcstr p) {
    ZLOGD("chdir(%s) (stub)", p.a ? p.get() : "(null)");
    return 0;
}

i32 hle_close(i32 fd) {
    ZLOGD("close(%d) (stub)", fd);
    return 0;
}

i32 hle_pipe(gp<i32> fds) {
    if (fds.a) {
        fds.get()[0] = 3;
        fds.get()[1] = 4;
    }
    return 0;
}

i32 hle_raise(i32 sig) {
    ZLOGW("guest raised signal %d (ignored)", sig);
    return 0;
}

gssize hle_read(i32 fd, gaddr buf, gsize n) {
    (void)fd; (void)buf; (void)n;
    return 0;
}

gssize hle_write(i32 fd, gaddr buf, gsize n) {
    if (fd == 1 || fd == 2) {
        FILE* out = (fd == 2) ? stderr : stdout;
        std::fwrite(hp(buf), 1, n, out);
        std::fflush(out);
        return static_cast<gssize>(n);
    }
    return static_cast<gssize>(n);
}

i32 hle_remove(JitCtx& j, gcstr p) {
    if (!p.a) {
        j.set_errno(22 /* EINVAL */);
        return -1;
    }
    std::string path = translate_path(p.get());
    int r = std::remove(path.c_str());
    if (r != 0) sync_errno(j);
    return r;
}

i32 hle_unlink(JitCtx& j, gcstr p) {
    return hle_remove(j, p);
}

i32 hle_rename(JitCtx& j, gcstr oldp, gcstr newp) {
    if (!oldp.a || !newp.a) {
        j.set_errno(22 /* EINVAL */);
        return -1;
    }
    std::string o = translate_path(oldp.get());
    std::string n = translate_path(newp.get());
    std::error_code ec;
    std::filesystem::remove(n, ec);
    int r = std::rename(o.c_str(), n.c_str());
    if (r != 0) sync_errno(j);
    return r;
}

i32 hle_access(JitCtx& j, gcstr p, i32 mode) {
    if (!p.a) {
        j.set_errno(22 /* EINVAL */);
        return -1;
    }
    std::string host = translate_path(p.get());
#if defined(_WIN32)
    int r = _access(host.c_str(), mode & 6);
#else
    int r = access(host.c_str(), mode);
#endif
    if (r != 0) sync_errno(j);
    return r;
}

i32 hle_mkdir(JitCtx& j, gcstr p, u32 mode) {
    (void)mode;
    if (!p.a) {
        j.set_errno(22 /* EINVAL */);
        return -1;
    }
    std::string host = translate_path(p.get());
    std::error_code ec;
    std::filesystem::create_directories(host, ec);
    if (ec) {
        j.set_errno(13 /* EACCES */);
        return -1;
    }
    return 0;
}

i32 hle_rmdir(JitCtx& j, gcstr p) {
    if (!p.a) {
        j.set_errno(22 /* EINVAL */);
        return -1;
    }
    std::string host = translate_path(p.get());
#if defined(_WIN32)
    int r = _rmdir(host.c_str());
#else
    int r = rmdir(host.c_str());
#endif
    if (r != 0) sync_errno(j);
    return r;
}

i32 hle_open(JitCtx& j, gcstr p, i32 flags, u32 mode) {
    (void)mode;
    if (!p.a) {
        j.set_errno(22 /* EINVAL */);
        return -1;
    }
    std::string host = translate_path(p.get());
#if defined(_WIN32)
    int hflags = _O_BINARY;
    switch (flags & 3) {
        case 0: hflags |= _O_RDONLY; break;
        case 1: hflags |= _O_WRONLY; break;
        case 2: hflags |= _O_RDWR;   break;
    }
    if (flags & 0100)  hflags |= _O_CREAT;
    if (flags & 0200)  hflags |= _O_EXCL;
    if (flags & 01000) hflags |= _O_TRUNC;
    if (flags & 02000) hflags |= _O_APPEND;
    if (hflags & _O_CREAT) {
        std::error_code ec;
        std::filesystem::path fp(host);
        if (fp.has_parent_path()) std::filesystem::create_directories(fp.parent_path(), ec);
    }
    int fd = _open(host.c_str(), hflags, _S_IREAD | _S_IWRITE);
#else
    int fd = open(host.c_str(), flags, mode);
#endif
    ZLOGD("open(%s -> %s, 0x%x) -> %d", p.get(), host.c_str(), flags, fd);
    if (fd < 0) sync_errno(j);
    return fd;
}

namespace {
void fill_bionic_stat(gaddr st, u32 mode, u64 size, i64 atime_s, i64 mtime_s, i64 ctime_s) {
    if (!st) return;
    std::memset(hp(st), 0, 104);
    wr<u32>(st + 16, mode);
    wr<u32>(st + 20, 1); // st_nlink
    wr<i64>(st + 48, static_cast<i64>(size));
    wr<u32>(st + 56, 4096); // st_blksize
    wr<u64>(st + 64, (size + 511) / 512); // st_blocks
    wr<u32>(st + 72, static_cast<u32>(atime_s));
    wr<u32>(st + 80, static_cast<u32>(mtime_s));
    wr<u32>(st + 88, static_cast<u32>(ctime_s));
}
} // namespace

i32 hle_stat(JitCtx& j, gcstr p, gaddr st) {
    if (!p.a || !st) {
        j.set_errno(22 /* EINVAL */);
        return -1;
    }
    std::string host = translate_path(p.get());
#if defined(_WIN32)
    struct _stat64 s{};
    if (_stat64(host.c_str(), &s) != 0) {
        sync_errno(j);
        return -1;
    }
#else
    struct stat s{};
    if (stat(host.c_str(), &s) != 0) {
        sync_errno(j);
        return -1;
    }
#endif
    fill_bionic_stat(st, static_cast<u32>(s.st_mode), static_cast<u64>(s.st_size), s.st_atime, s.st_mtime, s.st_ctime);
    return 0;
}

i32 hle_lstat(JitCtx& j, gcstr p, gaddr st) {
    return hle_stat(j, p, st);
}

i32 hle_fstat(JitCtx& j, i32 fd, gaddr st) {
    if (!st) {
        j.set_errno(22 /* EINVAL */);
        return -1;
    }
#if defined(_WIN32)
    struct _stat64 s{};
    if (_fstat64(fd, &s) != 0) {
        sync_errno(j);
        return -1;
    }
#else
    struct stat s{};
    if (fstat(fd, &s) != 0) {
        sync_errno(j);
        return -1;
    }
#endif
    fill_bionic_stat(st, static_cast<u32>(s.st_mode), static_cast<u64>(s.st_size), s.st_atime, s.st_mtime, s.st_ctime);
    return 0;
}

namespace {
struct GuestDir {
    std::vector<std::filesystem::directory_entry> entries;
    size_t idx = 0;
    gaddr dirent_buf = 0;
};
std::mutex g_dirs_lock;
std::unordered_map<gaddr, GuestDir> g_dirs;
u32 g_next_dir = 0x16000000;
} // namespace

gaddr hle_opendir(JitCtx& j, gcstr p) {
    if (!p.a) {
        j.set_errno(22 /* EINVAL */);
        return 0;
    }
    std::string host = translate_path(p.get());
    std::error_code ec;
    if (!std::filesystem::is_directory(host, ec)) {
        j.set_errno(2 /* ENOENT */);
        return 0;
    }
    GuestDir gd;
    for (const auto& entry : std::filesystem::directory_iterator(host, ec)) {
        gd.entries.push_back(entry);
    }
    gd.dirent_buf = mem::gcalloc(1, 280);
    std::lock_guard lk(g_dirs_lock);
    gaddr h = g_next_dir += 4;
    g_dirs[h] = std::move(gd);
    return h;
}

gaddr hle_readdir(JitCtx& j, gaddr dirp) {
    (void)j;
    std::lock_guard lk(g_dirs_lock);
    auto it = g_dirs.find(dirp);
    if (it == g_dirs.end() || it->second.idx >= it->second.entries.size()) return 0;
    const auto& entry = it->second.entries[it->second.idx++];
    gaddr buf = it->second.dirent_buf;
    std::memset(hp(buf), 0, 280);
    wr<u64>(buf + 0, it->second.idx); // d_ino
    wr<i64>(buf + 8, static_cast<i64>(it->second.idx)); // d_off
    wr<u16>(buf + 16, 280); // d_reclen
    std::error_code ec;
    wr<u8>(buf + 18, entry.is_directory(ec) ? 4 /* DT_DIR */ : 8 /* DT_REG */); // d_type
    std::string name = entry.path().filename().string();
    std::snprintf(hp<char>(buf + 19), 256, "%s", name.c_str());
    return buf;
}

i32 hle_closedir(JitCtx& j, gaddr dirp) {
    std::lock_guard lk(g_dirs_lock);
    auto it = g_dirs.find(dirp);
    if (it == g_dirs.end()) {
        j.set_errno(9 /* EBADF */);
        return -1;
    }
    if (it->second.dirent_buf) mem::gfree(it->second.dirent_buf);
    g_dirs.erase(it);
    return 0;
}

// setjmp / longjmp
namespace {
constexpr u32 kJmpMagic = 0x5A424A42;
}

void hle_setjmp(JitCtx& j) {
    gaddr buf = j.r()[0];
    auto& r = j.r();
    for (int i = 0; i < 8; ++i) wr<u32>(buf + 4 * i, r[4 + i]); // r4-r11
    wr<u32>(buf + 32, r[13]); // sp
    wr<u32>(buf + 36, r[14]); // lr
    wr<u32>(buf + 108, kJmpMagic);
    j.set_ret(0);
}

void hle_longjmp(JitCtx& j) {
    gaddr buf = j.r()[0];
    u32 val = j.r()[1];
    if (rd<u32>(buf + 108) != kJmpMagic) {
        cpu::dump_state(j, "longjmp");
        fatal("longjmp corrupted jmp_buf");
    }
    auto& r = j.r();
    for (int i = 0; i < 8; ++i) r[4 + i] = rd<u32>(buf + 4 * i);
    r[13] = rd<u32>(buf + 32);
    u32 target = rd<u32>(buf + 36);
    r[0] = val ? val : 1;
    r[15] = target & ~1u;
}

// liblog print forwarding
void hle_android_log_print(JitCtx& j) {
    ArgReader r(j);
    i32 prio = static_cast<i32>(r.w32());
    u32 tag = r.w32();
    u32 fmt = r.w32();
    u32 ap = r.va_list_here();
    std::string msg = vformat(hp<char>(fmt), ap);
    const char* tag_str = tag ? hp<const char>(tag) : "RSBR";
    FILE* out = (prio >= 5 /* ANDROID_LOG_WARN */) ? stderr : stdout;
    std::fprintf(out, "[%s] %s\n", tag_str, msg.c_str());
    std::fflush(out);
    j.set_ret(static_cast<u32>(msg.size()));
}

// libdl
gaddr hle_dlopen(gcstr name, i32 flags) {
    (void)flags;
    ZLOGD("dlopen(%s) -> pseudo-handle", name.a ? name.get() : "self");
    return 0x20; // pseudo handle
}
gaddr hle_dlsym(gaddr, gcstr sym) {
    if (!sym.a) return 0;
    std::string s = sym.get();
    if (gaddr a = cpu::find_thunk(s)) return a;
    return cpu::find_data(s);
}
i32 hle_dlclose(gaddr) { return 0; }
gaddr hle_dlerror() { return 0; }

// ==============================================================================
// 10. Public API & Helper Implementations
// ==============================================================================
std::string vformat(const char* fmt, u32 ap, int* consumed_ap) {
    if (!fmt) return "";
    std::string out;
    GVa va{ap};
    for (const char* p = fmt; *p;) {
        if (*p != '%') { out += *p++; continue; }
        p++;
        if (*p == '%') { out += '%'; p++; continue; }
        std::string spec = "%";
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') spec += *p++;
        while (*p >= '0' && *p <= '9') spec += *p++;
        if (*p == '.') {
            spec += *p++;
            while (*p >= '0' && *p <= '9') spec += *p++;
        }
        if (*p == 'l') { spec += *p++; if (*p == 'l') spec += *p++; }
        char conv = *p ? *p++ : 0;
        spec += conv;
        char buf[512];
        switch (conv) {
        case 'd': case 'i':
            std::snprintf(buf, sizeof(buf), spec.c_str(), static_cast<int>(va.next32()));
            out += buf;
            break;
        case 'u': case 'x': case 'X': case 'p':
            std::snprintf(buf, sizeof(buf), spec.c_str(), va.next32());
            out += buf;
            break;
        case 's': {
            u32 s_addr = va.next32();
            out += s_addr ? hp<const char>(s_addr) : "(null)";
            break;
        }
        case 'f': case 'g': case 'e':
            std::snprintf(buf, sizeof(buf), spec.c_str(), va.nextd());
            out += buf;
            break;
        case 'c':
            out += static_cast<char>(va.next32());
            break;
        default:
            out += spec;
            break;
        }
    }
    if (consumed_ap) *consumed_ap = static_cast<int>(va.ap - ap);
    return out;
}

int vscan_string(const char* input, const char* fmt, u32 ap) {
    if (!input || !fmt) return EOF;
    GVa va{ap};
    int assigned = 0;
    const char* in = input;
    const char* f = fmt;
    while (*f && *in) {
        if (std::isspace(static_cast<unsigned char>(*f))) {
            while (std::isspace(static_cast<unsigned char>(*f))) f++;
            while (std::isspace(static_cast<unsigned char>(*in))) in++;
            continue;
        }
        if (*f != '%') {
            if (*f != *in) break;
            f++; in++;
            continue;
        }
        f++;
        if (*f == '%') { if (*in != '%') break; f++; in++; continue; }
        char conv = *f++;
        if (conv == 'd') {
            char* end = nullptr;
            long val = std::strtol(in, &end, 10);
            if (end == in) break;
            in = end;
            wr<i32>(va.next32(), static_cast<i32>(val));
            assigned++;
        } else if (conv == 's') {
            while (std::isspace(static_cast<unsigned char>(*in))) in++;
            std::string tok;
            while (*in && !std::isspace(static_cast<unsigned char>(*in))) tok += *in++;
            if (tok.empty()) break;
            gaddr dst = va.next32();
            std::memcpy(hp(dst), tok.c_str(), tok.size() + 1);
            assigned++;
        }
    }
    return assigned;
}

int vscan_file(FILE* f, const char* fmt, u32 ap) {
    (void)f; (void)fmt; (void)ap;
    return 0;
}

FILE* host_file(gaddr gf) {
    if (gf >= g_sF && gf < g_sF + 3 * kGuestFileSize) {
        u32 idx = (gf - g_sF) / kGuestFileSize;
        return (idx == 0) ? stdin : ((idx == 1) ? stdout : stderr);
    }
    std::lock_guard lk(g_files_lock);
    auto it = g_files.find(gf);
    return (it == g_files.end()) ? nullptr : it->second;
}

gaddr guest_file(FILE* f) {
    if (f == stdin) return g_sF;
    if (f == stdout) return g_sF + kGuestFileSize;
    if (f == stderr) return g_sF + 2 * kGuestFileSize;
    gaddr gf = mem::gcalloc(1, kGuestFileSize + 12);
    std::lock_guard lk(g_files_lock);
    g_files[gf] = f;
    return gf;
}

void forget_file(gaddr gf) {
    std::lock_guard lk(g_files_lock);
    g_files.erase(gf);
    mem::gfree(gf);
}

gaddr get_sF() {
    return g_sF;
}

gaddr static_string(const std::string& s) {
    std::lock_guard lk(g_static_lock);
    auto it = g_static_strings.find(s);
    if (it != g_static_strings.end()) return it->second;
    gaddr g = mem::gstrdup(s.c_str());
    g_static_strings.emplace(s, g);
    return g;
}

gaddr tls_buffer(JitCtx& j, u32 off) {
    return j.thread->tls + off;
}

void set_save_dir(const std::string& path) {
    g_save_dir = path;
    std::error_code ec;
    std::filesystem::create_directories(g_save_dir, ec);
}

const std::string& get_save_dir() {
    return g_save_dir;
}

void set_obb_dir(const std::string& path) {
    if (!path.empty()) g_obb_dir = path;
}

const std::string& get_obb_dir() {
    return g_obb_dir;
}

std::string translate_path(const char* guest_path) {
    if (!guest_path || !*guest_path) return "";
    std::string s = guest_path;
    for (char& c : s) if (c == '\\') c = '/';

    auto to_win = [](std::string p) {
        for (char& c : p) if (c == '/') c = '\\';
        return p;
    };

    // 1. OBB files (main.25.com.playstation.runsackboyrun.obb, patch.42.com.playstation.runsackboyrun.obb, /Android/obb/...)
    if (s.find("main.25.com.playstation.runsackboyrun.obb") != std::string::npos) {
        return to_win((std::filesystem::path(g_obb_dir) / "main.25.com.playstation.runsackboyrun.obb").string());
    }
    if (s.find("patch.42.com.playstation.runsackboyrun.obb") != std::string::npos) {
        return to_win((std::filesystem::path(g_obb_dir) / "patch.42.com.playstation.runsackboyrun.obb").string());
    }
    const std::string obb_pkg = "/Android/obb/com.playstation.runsackboyrun/";
    if (size_t pos = s.find(obb_pkg); pos != std::string::npos) {
        std::string rel = s.substr(pos + obb_pkg.size());
        return to_win((std::filesystem::path(g_obb_dir) / rel).string());
    }
    if (s == "/Android/obb/com.playstation.runsackboyrun" ||
        s == "/storage/emulated/0/Android/obb/com.playstation.runsackboyrun" ||
        s == "/obb") {
        return to_win(g_obb_dir);
    }
    if (s.rfind("/obb/", 0) == 0) {
        return to_win((std::filesystem::path(g_obb_dir) / s.substr(5)).string());
    }

    // 2. Save / Document / Cache files
    // When Resource::g_saveDataPath is empty, DocumentFileLoad/Save formats "/%s/%s" -> "//<filename>"
    if (s.rfind("//", 0) == 0) {
        size_t first_non_slash = s.find_first_not_of('/');
        std::string rel = (first_non_slash == std::string::npos) ? "" : s.substr(first_non_slash);
        return to_win((std::filesystem::path(g_save_dir) / rel).string());
    }

    for (const char* prefix : {
             "/data/data/com.playstation.runsackboyrun/files/",
             "/data/user/0/com.playstation.runsackboyrun/files/",
             "/files/" }) {
        size_t plen = std::strlen(prefix);
        if (s.rfind(prefix, 0) == 0) {
            return to_win((std::filesystem::path(g_save_dir) / s.substr(plen)).string());
        }
    }
    for (const char* prefix : {
             "/data/data/com.playstation.runsackboyrun/cache/",
             "/data/user/0/com.playstation.runsackboyrun/cache/",
             "/cache/" }) {
        size_t plen = std::strlen(prefix);
        if (s.rfind(prefix, 0) == 0) {
            return to_win((std::filesystem::path(g_save_dir) / "cache" / s.substr(plen)).string());
        }
    }

    bool is_win_abs = (s.size() >= 2 && s[1] == ':');
    bool is_rel_dot = (s.rfind("./", 0) == 0 || s.rfind("../", 0) == 0);
    if (!is_win_abs && !is_rel_dot) {
        if (s.find("/data/data/") != std::string::npos ||
            s.find("rsbra.sav") != std::string::npos ||
            s.find("rsbrb.sav") != std::string::npos ||
            s.find("rsbr.rcx") != std::string::npos ||
            (s.size() > 4 && (s.substr(s.size() - 4) == ".sav" ||
                              s.substr(s.size() - 4) == ".rcx" ||
                              s.substr(s.size() - 4) == ".pad"))) {
            size_t slash = s.rfind('/');
            std::string fname = (slash == std::string::npos) ? s : s.substr(slash + 1);
            return to_win((std::filesystem::path(g_save_dir) / fname).string());
        }
        if (s.rfind("/storage/emulated/0/", 0) == 0) {
            return to_win((std::filesystem::path(g_save_dir) / s.substr(20)).string());
        }
        if (s.rfind("/sdcard/", 0) == 0) {
            return to_win((std::filesystem::path(g_save_dir) / s.substr(8)).string());
        }
    }

    return to_win(s);
}

// ==============================================================================
// 11. Registration Entries
// ==============================================================================
void register_libc() {
    REG(malloc, hle_malloc);
    REG(free, hle_free);
    REG(realloc, hle_realloc);
    REG(memchr, hle_memchr);
    REG(memcmp, hle_memcmp);
    REG(memcpy, hle_memcpy);
    REG(memmove, hle_memmove);
    REG(memset, hle_memset);

    REG(strlen, hle_strlen);
    REG(strcmp, hle_strcmp);
    REG(strncmp, hle_strncmp);
    REG(strcpy, hle_strcpy);
    REG(strncpy, hle_strncpy);
    REG(strcat, hle_strcat);
    REG(strchr, hle_strchr);
    REG(strrchr, hle_strrchr);
    REG(strstr, hle_strstr);
    REG(strerror, hle_strerror);
    REG(atoi, hle_atoi);
    REG(atol, hle_atol);
    REG(strtod, hle_strtod);
    REG(tolower, hle_tolower);
    REG(isspace, hle_isspace);
    REG(isupper, hle_isupper);
    REG(lrand48, hle_lrand48);
    REG(getenv, hle_getenv);
    REG(qsort, hle_qsort);

    REG(abort, hle_abort);
    REG(__assert2, hle_assert2);
    REG(__errno, hle_errno_location);
    REG_RAW(setjmp, hle_setjmp);
    REG_RAW(longjmp, hle_longjmp);

    REG(chdir, hle_chdir);
    REG(close, hle_close);
    REG(pipe, hle_pipe);
    REG(raise, hle_raise);
    REG(read, hle_read);
    REG(write, hle_write);
    REG(remove, hle_remove);
    REG(unlink, hle_unlink);
    REG(rename, hle_rename);
    REG(access, hle_access);
    REG(mkdir, hle_mkdir);
    REG(rmdir, hle_rmdir);
    REG(open, hle_open);
    REG(stat, hle_stat);
    REG(lstat, hle_lstat);
    REG(fstat, hle_fstat);
    REG(opendir, hle_opendir);
    REG(readdir, hle_readdir);
    REG(closedir, hle_closedir);
}

void register_math() {
    REG(sin, m_sin);
    REG(sinf, m_sinf);
    REG(cosf, m_cosf);
    REG(tanf, m_tanf);
    REG(atan2f, m_atan2f);
    REG(sqrtf, m_sqrtf);
    REG(powf, m_powf);
    REG(floorf, m_floorf);
    REG(ceilf, m_ceilf);
    REG(log10f, m_log10f);
}

void register_cxx() {
    REG(_Znwj, cxx_new);
    REG(_Znaj, cxx_new);
    REG(_ZdlPv, cxx_delete);
    REG(_ZdaPv, cxx_delete);
    REG(__cxa_guard_acquire, cxa_guard_acquire);
    REG(__cxa_guard_release, cxa_guard_release);
    REG(__cxa_pure_virtual, cxa_pure_virtual);
    REG(__cxa_atexit, cxa_atexit);
    REG(__cxa_finalize, cxa_finalize);
    REG(__aeabi_atexit, aeabi_atexit);
    REG(__aeabi_memcpy, aeabi_memcpy);
    REG(__aeabi_memmove, aeabi_memmove);
    REG(__aeabi_memset, aeabi_memset);
}

void register_stdio() {
    g_sF = mem::gcalloc(3, kGuestFileSize);
    for (u32 i = 0; i < 3; ++i) wr<i16>(g_sF + i * kGuestFileSize + 14, static_cast<i16>(i));
    cpu::register_data("__sF", g_sF);

    REG(fopen, s_fopen);
    REG(fclose, s_fclose);
    REG(fflush, s_fflush);
    REG(fread, s_fread);
    REG(fwrite, s_fwrite);
    REG(fgetc, s_fgetc);
    REG(getc, s_fgetc);
    REG(fputc, s_fputc);
    REG(putc, s_fputc);
    REG(fputs, s_fputs);
    REG(ungetc, s_ungetc);
    REG(fseek, s_fseek);
    REG(ftell, s_ftell);

    REG_RAW(snprintf, s_snprintf);
    REG_RAW(sprintf, s_sprintf);
    REG_RAW(vsprintf, s_vsprintf);
    REG_RAW(fprintf, s_fprintf);
    REG_RAW(sscanf, s_sscanf);
}

void register_pthread() {
    REG(pthread_mutex_init, p_mutex_init);
    REG(pthread_mutex_destroy, p_mutex_destroy);
    REG(pthread_mutex_lock, p_mutex_lock);
    REG(pthread_mutex_trylock, p_mutex_trylock);
    REG(pthread_mutex_unlock, p_mutex_unlock);
    REG(pthread_cond_init, p_cond_init);
    REG(pthread_cond_destroy, p_cond_destroy);
    REG(pthread_cond_wait, p_cond_wait);
    REG(pthread_cond_signal, p_cond_signal);
    REG(pthread_cond_broadcast, p_cond_broadcast);
    REG(sem_init, sem_init_);
    REG(sem_wait, sem_wait_);
    REG(sem_trywait, sem_trywait_);
    REG(sem_post, sem_post_);
    REG(pthread_key_create, p_key_create);
    REG(pthread_key_delete, p_key_delete);
    REG(pthread_getspecific, p_getspecific);
    REG(pthread_setspecific, p_setspecific);
    REG(pthread_create, p_create);
    REG(pthread_attr_init, p_attr_init);
    REG(pthread_attr_setdetachstate, p_attr_setdetachstate);
    REG(pthread_setschedparam, p_setschedparam);
    REG(sched_yield, s_yield);
}

void register_time() {
    REG(time, hle_time);
    REG(clock_gettime, hle_clock_gettime);
    REG(localtime, hle_localtime);
    REG(mktime, hle_mktime);
    REG(difftime, hle_difftime);
    REG(usleep, hle_usleep);
}

void register_log() {
    REG_RAW(__android_log_print, hle_android_log_print);
}

void register_dl() {
    REG(dlopen, hle_dlopen);
    REG(dlsym, hle_dlsym);
    REG(dlclose, hle_dlclose);
    REG(dlerror, hle_dlerror);
}

void register_all_symbols_stubs() {
    // OpenSL interface GUID data stubs (same distinct bytes as opensl_win32.cpp)
    const char* sl_iids[] = {
        "", "SL_IID_ENGINE", "SL_IID_PLAY", "SL_IID_VOLUME", "SL_IID_BUFFERQUEUE",
        "SL_IID_EFFECTSEND", "SL_IID_PLAYBACKRATE", "SL_IID_ENGINECAPABILITIES",
        "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"
    };
    for (int i = 1; i <= 8; ++i) {
        if (!cpu::find_data(sl_iids[i])) {
            gaddr g = mem::gcalloc(1, 16);
            u8* b = hp<u8>(g);
            for (size_t k = 0; k < 16; ++k) b[k] = static_cast<u8>(0xA0 + i * 11 + k * 3);
            gaddr var = mem::gmalloc(4);
            wr<u32>(var, g);
            cpu::register_data(sl_iids[i], var);
        }
    }
}

void register_all() {
    register_libc();
    register_math();
    register_cxx();
    register_stdio();
    register_pthread();
    register_time();
    register_log();
    register_dl();
    register_all_symbols_stubs();
}

} // namespace zb::bionic
