#include "mem_win32.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

extern "C" {
struct mallinfo {
    size_t arena;
    size_t ordblks;
    size_t smblks;
    size_t hblks;
    size_t hblkhd;
    size_t usmblks;
    size_t fsmblks;
    size_t uordblks;
    size_t fordblks;
    size_t keepcost;
};
void* create_mspace(size_t capacity, int locked);
void* mspace_malloc(void* msp, size_t bytes);
void mspace_free(void* msp, void* mem);
void* mspace_realloc(void* msp, void* mem, size_t newsize);
void* mspace_calloc(void* msp, size_t n_elements, size_t elem_size);
void* mspace_memalign(void* msp, size_t alignment, size_t bytes);
size_t mspace_usable_size(const void* mem);
struct mallinfo mspace_mallinfo(void* msp);
void* zb_dl_mmap(size_t n);
int zb_dl_munmap(void* p, size_t n);
}

namespace zb::mem {
namespace {

constexpr u64 kLow = 0x00100000;    // 1 MiB (NULL trap)
constexpr u64 kHigh = 0xFFFF0000;   // 4 GiB - 64 KiB
constexpr u64 kKUserSharedData = 0x7FFE0000;
constexpr u64 kKUserSharedSize = 0x00010000;

PageTable* g_pt = nullptr;
u32 g_host_page = 4096;
uintptr_t g_base = 0;
bool g_is_identity = true;
std::mutex g_lock;
std::map<u64, u64> g_free; // start -> end (exclusive)
std::vector<std::pair<gaddr, gaddr>> g_ro;
void* g_msp = nullptr;
MemoryStats g_stats{};

u64 round_up(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }

DWORD win32_protect(int prot) {
    bool r = (prot & 1) != 0;
    bool w = (prot & 2) != 0;
    bool x = (prot & 4) != 0;
    if (x) {
        if (w) return PAGE_EXECUTE_READWRITE;
        if (r) return PAGE_EXECUTE_READ;
        return PAGE_EXECUTE;
    }
    if (w) return PAGE_READWRITE;
    if (r) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

void set_pages(u64 a, u64 len, bool present) {
    if (!g_pt) return;
    u64 first = a >> kGuestPageBits;
    u64 last = (a + len + kGuestPageSize - 1) >> kGuestPageBits;
    for (u64 p = first; p < last && p < g_pt->size(); ++p) {
        (*g_pt)[p] = present ? reinterpret_cast<u8*>(static_cast<uintptr_t>(g_base + (p << kGuestPageBits))) : nullptr;
    }
}

void rebuild_free_locked() {
    g_free.clear();
    uintptr_t addr = kLow;
    while (addr < kHigh) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(g_base + addr), &mbi, sizeof(mbi))) break;
        uintptr_t host_base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t size = mbi.RegionSize;
        uintptr_t guest_addr = host_base - g_base;
        uintptr_t end = guest_addr + size;
        if (end > kHigh) end = kHigh;

        if ((mbi.State == MEM_RESERVE || mbi.State == MEM_FREE) && end > guest_addr) {
            uintptr_t s = std::max(guest_addr, static_cast<uintptr_t>(kLow));
            uintptr_t e = std::min(end, static_cast<uintptr_t>(kHigh));
            // Ensure we never hand out the Windows KUSER_SHARED_DATA page
            if (s < kKUserSharedData + kKUserSharedSize && e > kKUserSharedData) {
                if (s < kKUserSharedData) g_free[s] = kKUserSharedData;
                if (e > kKUserSharedData + kKUserSharedSize) g_free[kKUserSharedData + kKUserSharedSize] = e;
            } else if (e > s) {
                g_free[s] = e;
            }
        }
        addr = guest_addr + size;
    }
}

void carve_locked(u64 a, u64 len) {
    u64 end = a + len;
    auto it = g_free.upper_bound(a);
    if (it != g_free.begin()) --it;
    while (it != g_free.end() && it->first < end) {
        u64 s = it->first, e = it->second;
        if (e <= a) {
            ++it;
            continue;
        }
        it = g_free.erase(it);
        if (s < a) g_free[s] = a;
        if (e > end) g_free[end] = e;
    }
}

void release_locked(u64 a, u64 len) {
    u64 s = a, e = a + len;
    auto next = g_free.lower_bound(s);
    if (next != g_free.begin()) {
        auto prev = std::prev(next);
        if (prev->second >= s) {
            s = prev->first;
            e = std::max(e, prev->second);
            g_free.erase(prev);
        }
    }
    next = g_free.lower_bound(s);
    while (next != g_free.end() && next->first <= e) {
        e = std::max(e, next->second);
        next = g_free.erase(next);
    }
    g_free[s] = e;
}

bool is_range_free_locked(u64 a, u64 len) {
    if (len == 0) return false;
    if (a < kLow || static_cast<u64>(a) + len > kHigh) return false;
    if (a < kKUserSharedData + kKUserSharedSize && static_cast<u64>(a) + len > kKUserSharedData) {
        return false;
    }
    // Check page table: every page in the range must be unmapped (nullptr)
    if (g_pt) {
        u64 first = a >> kGuestPageBits;
        u64 last = (a + len + kGuestPageSize - 1) >> kGuestPageBits;
        for (u64 p = first; p < last && p < g_pt->size(); ++p) {
            if ((*g_pt)[p] != nullptr) return false;
        }
    }
    // Check g_free interval: [a, a + len) must be completely enclosed within a single free block
    auto it = g_free.upper_bound(a);
    if (it == g_free.begin()) return false;
    --it;
    if (it->first > a || static_cast<u64>(a) + len > it->second) return false;

    return true;
}

gaddr alloc_locked(u64 size, int prot, int* err) {
    DWORD win_prot = win32_protect(prot);
    for (int attempt = 0; attempt < 4; ++attempt) {
        bool stale = false;
        for (auto it = g_free.rbegin(); it != g_free.rend(); ++it) {
            u64 s = it->first, e = it->second;
            if (e - s < size) continue;
            u64 cand = (e - size) & ~static_cast<u64>(g_host_page - 1);
            if (cand < s) continue;

            void* target = reinterpret_cast<void*>(g_base + cand);
            void* r = VirtualAlloc(target, size, MEM_COMMIT, win_prot);
            if (!r) {
                // If address was not yet reserved, try reserving and committing
                r = VirtualAlloc(target, size, MEM_RESERVE | MEM_COMMIT, win_prot);
            }
            if (!r) {
                stale = true;
                break;
            }
            carve_locked(cand, size);
            g_stats.total_committed_bytes += size;
            return static_cast<gaddr>(cand);
        }
        if (!stale && attempt > 0) break;
        rebuild_free_locked();
    }
    if (err) *err = ENOMEM;
    return 0;
}

} // namespace

void init(const MemoryConfig& cfg) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_host_page = si.dwPageSize;
    DWORD alloc_gran = si.dwAllocationGranularity;

    // Allocate 8 MiB PageTable
    void* pt = VirtualAlloc(nullptr, sizeof(PageTable), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!pt) fatal("cannot allocate guest page table");
    g_pt = static_cast<PageTable*>(pt);

    g_base = 0;
    g_is_identity = false;
    uint64_t total_reserved = 0;

    if (cfg.enable_identity_map) {
        // Step 1: Scan MEM_FREE ranges in low 4GB and reserve them
        uintptr_t scan = kLow;
        while (scan < kHigh) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(scan), &mbi, sizeof(mbi))) break;
            uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            uintptr_t size = mbi.RegionSize;
            uintptr_t end = base + size;
            if (end > kHigh) end = kHigh;

            if (mbi.State == MEM_FREE && end > base) {
                uintptr_t s = std::max(base, static_cast<uintptr_t>(kLow));
                uintptr_t e = std::min(end, static_cast<uintptr_t>(kHigh));

                // Align to 64KB granularity
                s = (s + alloc_gran - 1) & ~static_cast<uintptr_t>(alloc_gran - 1);
                e = e & ~static_cast<uintptr_t>(alloc_gran - 1);

                if (s < kKUserSharedData + kKUserSharedSize && e > kKUserSharedData) {
                    if (s < kKUserSharedData) {
                        uintptr_t sub_e = std::min(e, static_cast<uintptr_t>(kKUserSharedData));
                        if (sub_e > s) {
                            void* r = VirtualAlloc(reinterpret_cast<void*>(s), sub_e - s, MEM_RESERVE, PAGE_NOACCESS);
                            if (r) total_reserved += (sub_e - s);
                        }
                    }
                    if (e > kKUserSharedData + kKUserSharedSize) {
                        uintptr_t sub_s = std::max(s, static_cast<uintptr_t>(kKUserSharedData + kKUserSharedSize));
                        if (e > sub_s) {
                            void* r = VirtualAlloc(reinterpret_cast<void*>(sub_s), e - sub_s, MEM_RESERVE, PAGE_NOACCESS);
                            if (r) total_reserved += (e - sub_s);
                        }
                    }
                } else if (e > s) {
                    void* r = VirtualAlloc(reinterpret_cast<void*>(s), e - s, MEM_RESERVE, PAGE_NOACCESS);
                    if (r) total_reserved += (e - s);
                }
            }
            scan = base + size;
        }

        // If at least 3.5 GiB reserved below 4GB, identity mapping is successful
        if (total_reserved >= (3500ULL << 20)) {
            g_base = 0;
            g_is_identity = true;
            ZLOGI("Win32 memory: Identity mapping active (0x00100000..0xFFFF0000, %llu MiB reserved)",
                  static_cast<unsigned long long>(total_reserved >> 20));
        } else {
            ZLOGW("Win32 memory: Only %llu MiB free in low 4GB; falling back to Tier 2 base pointer",
                  static_cast<unsigned long long>(total_reserved >> 20));
        }
    }

    if (!g_is_identity) {
        // Tier 2 Fallback: Reserve 4 GiB buffer anywhere in 64-bit space
        void* fb = VirtualAlloc(reinterpret_cast<void*>(cfg.fallback_base), 0x100000000ULL, MEM_RESERVE, PAGE_NOACCESS);
        if (!fb && cfg.fallback_base != 0) {
            fb = VirtualAlloc(nullptr, 0x100000000ULL, MEM_RESERVE, PAGE_NOACCESS);
        }
        if (!fb) fatal("cannot reserve 4 GiB guest virtual address space");
        g_base = reinterpret_cast<uintptr_t>(fb);
        total_reserved = 0x100000000ULL;
        ZLOGI("Win32 memory: Base offset mapping active at host base 0x%016llX", static_cast<unsigned long long>(g_base));
    }

    g_stats.total_reserved_bytes = total_reserved;
    g_stats.is_identity_mapped = g_is_identity;
    g_stats.guest_base = g_base;

    {
        std::lock_guard lk(g_lock);
        rebuild_free_locked();
        u64 total = 0, largest = 0;
        for (auto [s, e] : g_free) {
            total += e - s;
            largest = std::max(largest, e - s);
        }
        g_stats.largest_free_hole_bytes = largest;
        ZLOGI("Win32 memory: host page %u, %llu MiB free for guest, largest hole %llu MiB",
              g_host_page, static_cast<unsigned long long>(total >> 20), static_cast<unsigned long long>(largest >> 20));
    }

    g_msp = create_mspace(0, 1);
    if (!g_msp) fatal("cannot create guest heap mspace");
}

void shutdown() {
    std::lock_guard lk(g_lock);
    if (g_pt) {
        VirtualFree(g_pt, 0, MEM_RELEASE);
        g_pt = nullptr;
    }
    if (!g_is_identity && g_base) {
        VirtualFree(reinterpret_cast<void*>(g_base), 0, MEM_RELEASE);
        g_base = 0;
    }
    g_free.clear();
    g_ro.clear();
}

PageTable* page_table() { return g_pt; }
uintptr_t guest_base() { return g_base; }
bool is_identity_mapped() { return g_is_identity; }
u32 host_page_size() { return g_host_page; }
MemoryStats get_memory_stats() { return g_stats; }

gaddr map_anon(u32 size, int prot, const char* /*tag*/) {
    u64 len = round_up(size, g_host_page);
    std::lock_guard lk(g_lock);
    int err = 0;
    gaddr a = alloc_locked(len, prot, &err);
    if (!a) return 0;
    set_pages(a, len, prot != 0);
    return a;
}

void unmap_anon(gaddr addr, u32 size) {
    u64 len = round_up(size, g_host_page);
    std::lock_guard lk(g_lock);
    set_pages(addr, len, false);
    VirtualFree(reinterpret_cast<void*>(g_base + addr), len, MEM_DECOMMIT);
    release_locked(addr, len);
    if (g_stats.total_committed_bytes >= len) g_stats.total_committed_bytes -= len;
}

gaddr guest_mmap(gaddr addr, u32 len32, int prot, int flags, int /*fd*/, int64_t /*offset*/, int* err) {
    if (len32 == 0) {
        if (err) *err = EINVAL;
        return 0xFFFFFFFFu;
    }
    u64 len = round_up(len32, g_host_page);
    DWORD win_prot = win32_protect(prot);

    std::lock_guard lk(g_lock);
    if (flags & 0x10 /* MAP_FIXED */) {
        if (addr % g_host_page || static_cast<u64>(addr) + len > kHigh || addr < kLow) {
            if (err) *err = EINVAL;
            return 0xFFFFFFFFu;
        }
        void* target = reinterpret_cast<void*>(g_base + addr);
        void* r = VirtualAlloc(target, len, MEM_COMMIT, win_prot);
        if (!r) r = VirtualAlloc(target, len, MEM_RESERVE | MEM_COMMIT, win_prot);
        if (!r) {
            if (err) *err = ENOMEM;
            return 0xFFFFFFFFu;
        }
        carve_locked(addr, len);
        g_stats.total_committed_bytes += len;
        set_pages(addr, len, prot != 0);
        return addr;
    }

    // Hinted allocation without MAP_FIXED:
    // Only map at hint if the range is completely free, unmapped, and safe.
    // If already mapped or unavailable, POSIX dictates we must NOT steal/alias;
    // instead fall through to alloc_locked to pick an available region.
    if (addr && addr % g_host_page == 0 && addr >= kLow && static_cast<u64>(addr) + len <= kHigh) {
        if (is_range_free_locked(addr, len)) {
            void* target = reinterpret_cast<void*>(g_base + addr);
            void* r = VirtualAlloc(target, len, MEM_COMMIT, win_prot);
            if (!r) r = VirtualAlloc(target, len, MEM_RESERVE | MEM_COMMIT, win_prot);
            if (r) {
                carve_locked(addr, len);
                g_stats.total_committed_bytes += len;
                set_pages(addr, len, prot != 0);
                return addr;
            }
        }
    }

    gaddr a = alloc_locked(len, prot, err);
    if (!a) return 0xFFFFFFFFu;
    set_pages(a, len, prot != 0);
    return a;
}

int guest_munmap(gaddr addr, u32 len32, int* err) {
    if (addr % g_host_page || len32 == 0) {
        if (err) *err = EINVAL;
        return -1;
    }
    u64 len = round_up(len32, g_host_page);
    std::lock_guard lk(g_lock);
    set_pages(addr, len, false);
    if (!VirtualFree(reinterpret_cast<void*>(g_base + addr), len, MEM_DECOMMIT)) {
        if (err) *err = EINVAL;
        return -1;
    }
    release_locked(addr, len);
    if (g_stats.total_committed_bytes >= len) g_stats.total_committed_bytes -= len;
    return 0;
}

int guest_mprotect(gaddr addr, u32 len32, int prot, int* err) {
    u64 len = round_up(len32, g_host_page);
    DWORD win_prot = win32_protect(prot);
    DWORD old_prot;
    std::lock_guard lk(g_lock);
    if (!VirtualProtect(reinterpret_cast<void*>(g_base + addr), len, win_prot, &old_prot)) {
        if (err) *err = EINVAL;
        return -1;
    }
    set_pages(addr, len, prot != 0);
    return 0;
}

gaddr guest_mremap(gaddr old_addr, u32 old_len32, u32 new_len32, int flags, int* err) {
    u64 old_len = round_up(old_len32, g_host_page);
    u64 new_len = round_up(new_len32, g_host_page);
    std::lock_guard lk(g_lock);
    if (new_len <= old_len) {
        if (new_len < old_len) {
            u64 diff = old_len - new_len;
            set_pages(old_addr + new_len, diff, false);
            VirtualFree(reinterpret_cast<void*>(g_base + old_addr + new_len), diff, MEM_DECOMMIT);
            release_locked(old_addr + new_len, diff);
            if (g_stats.total_committed_bytes >= diff) g_stats.total_committed_bytes -= diff;
        }
        return old_addr;
    }

    // Try in-place expansion:
    // Only attempt in-place expansion if the neighbor range [old_addr + old_len, old_addr + new_len)
    // is completely free and unmapped.
    u64 exp_len = new_len - old_len;
    u64 exp_addr = static_cast<u64>(old_addr) + old_len;
    if (static_cast<u64>(old_addr) + new_len <= kHigh && is_range_free_locked(exp_addr, exp_len)) {
        void* exp_target = reinterpret_cast<void*>(g_base + exp_addr);
        void* r = VirtualAlloc(exp_target, exp_len, MEM_COMMIT, PAGE_READWRITE);
        if (!r) r = VirtualAlloc(exp_target, exp_len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (r) {
            carve_locked(exp_addr, exp_len);
            g_stats.total_committed_bytes += exp_len;
            set_pages(old_addr, new_len, true);
            return old_addr;
        }
    }

    if (!(flags & 1 /* MREMAP_MAYMOVE */)) {
        if (err) *err = ENOMEM;
        return 0xFFFFFFFFu;
    }

    gaddr dst = alloc_locked(new_len, 3 /* PROT_READ | PROT_WRITE */, err);
    if (!dst) return 0xFFFFFFFFu;
    std::memcpy(reinterpret_cast<void*>(g_base + dst), reinterpret_cast<void*>(g_base + old_addr), old_len);
    set_pages(old_addr, old_len, false);
    VirtualFree(reinterpret_cast<void*>(g_base + old_addr), old_len, MEM_DECOMMIT);
    release_locked(old_addr, old_len);
    if (g_stats.total_committed_bytes >= old_len) g_stats.total_committed_bytes -= old_len;
    set_pages(dst, new_len, true);
    return dst;
}

bool is_mapped(gaddr a) {
    if (!g_pt) return false;
    return (*g_pt)[a >> kGuestPageBits] != nullptr;
}

bool is_range_mapped(gaddr a, u32 len) {
    if (!g_pt) return false;
    if (len == 0) return true;
    u64 end = static_cast<u64>(a) + len;
    for (u64 p = a & ~(static_cast<u64>(kGuestPageSize) - 1); p < end; p += kGuestPageSize) {
        if (p > 0xFFFFFFFFull || !(*g_pt)[p >> kGuestPageBits]) return false;
    }
    return true;
}

bool is_range_free(gaddr a, u32 len) {
    std::lock_guard lk(g_lock);
    return is_range_free_locked(a, len);
}

void add_readonly_range(gaddr lo, gaddr hi) {
    std::lock_guard lk(g_lock);
    g_ro.emplace_back(lo, hi);
}

bool is_readonly(gaddr a) {
    std::lock_guard lk(g_lock);
    for (auto [lo, hi] : g_ro) {
        if (a >= lo && a < hi) return true;
    }
    return false;
}

gaddr gmalloc(u32 n) { return ga_checked(mspace_calloc(g_msp, 1, n ? n : 1), "malloc"); }
gaddr gcalloc(u32 n, u32 size) { return ga_checked(mspace_calloc(g_msp, n, size), "calloc"); }
gaddr grealloc(gaddr p, u32 n) { return ga_checked(mspace_realloc(g_msp, p ? hp(p) : nullptr, n), "realloc"); }
void gfree(gaddr p) { if (p) mspace_free(g_msp, hp(p)); }
gaddr gmemalign(u32 align, u32 n) { return ga_checked(mspace_memalign(g_msp, align, n), "memalign"); }
u32 gusable_size(gaddr p) { return p ? static_cast<u32>(mspace_usable_size(hp(p))) : 0; }

HeapInfo heap_info() {
    struct mallinfo mi = mspace_mallinfo(g_msp);
    return HeapInfo{static_cast<u32>(mi.arena),    static_cast<u32>(mi.ordblks),  static_cast<u32>(mi.smblks),
                    static_cast<u32>(mi.hblks),    static_cast<u32>(mi.hblkhd),   static_cast<u32>(mi.usmblks),
                    static_cast<u32>(mi.fsmblks),  static_cast<u32>(mi.uordblks), static_cast<u32>(mi.fordblks),
                    static_cast<u32>(mi.keepcost)};
}

gaddr gstrdup(const char* s) {
    if (!s) return 0;
    return gdup(s, static_cast<u32>(std::strlen(s) + 1));
}

gaddr gdup(const void* p, u32 n) {
    gaddr a = gmalloc(n);
    if (a) std::memcpy(hp(a), p, n);
    return a;
}

} // namespace zb::mem

extern "C" void* zb_dl_mmap(size_t n) {
    zb::gaddr a = zb::mem::map_anon(static_cast<zb::u32>(n), 3 /* PROT_READ | PROT_WRITE */, "zb-guest-heap");
    return a ? zb::hp(a) : reinterpret_cast<void*>(~static_cast<uintptr_t>(0));
}

extern "C" int zb_dl_munmap(void* p, size_t n) {
    zb::mem::unmap_anon(zb::ga(p), static_cast<zb::u32>(n));
    return 0;
}
