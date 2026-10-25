#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

#include "common.h"

namespace zb::mem {

constexpr u32 kGuestPageBits = 12;
constexpr u32 kGuestPageSize = 1u << kGuestPageBits;
constexpr size_t kNumPageTableEntries = 1u << (32 - kGuestPageBits); // 1,048,576
using PageTable = std::array<u8*, kNumPageTableEntries>;

struct MemoryConfig {
    bool enable_identity_map = true; // Attempt 0x00100000..0xFFFF0000 identity map
    bool enable_fastmem = true;      // Enable Dynarmic fastmem pointer
    uintptr_t fallback_base = 0;     // Explicit fallback base if identity map fails
};

struct MemoryStats {
    uint64_t total_reserved_bytes;
    uint64_t total_committed_bytes;
    uint64_t largest_free_hole_bytes;
    bool is_identity_mapped;
    uintptr_t guest_base;            // 0 if identity mapped
};

// Subsystem lifecycle
void init(const MemoryConfig& cfg = {});
inline void init(u32 /*size*/) { init(MemoryConfig{}); }
void shutdown();

// Dynarmic integration
PageTable* page_table();
uintptr_t guest_base();              // 0 if identity mapped
bool is_identity_mapped();
u32 host_page_size();
inline void* guest_to_host(gaddr a) { return hp(a); }

// Anonymous guest memory (stacks, images, heap segments)
gaddr map_anon(u32 size, int prot, const char* tag = nullptr);
void unmap_anon(gaddr addr, u32 size);

// Guest-visible mmap family (POSIX conventions: 0xFFFFFFFF on failure, sets *err)
gaddr guest_mmap(gaddr addr, u32 len, int prot, int flags, int fd, int64_t offset, int* err);
int guest_munmap(gaddr addr, u32 len, int* err);
int guest_mprotect(gaddr addr, u32 len, int prot, int* err);
gaddr guest_mremap(gaddr old_addr, u32 old_len, u32 new_len, int flags, int* err);

// Address validation & queries
bool is_mapped(gaddr a);
bool is_range_mapped(gaddr a, u32 len);
bool is_range_free(gaddr a, u32 len);

// Read-only ranges for Dynarmic constant folding
void add_readonly_range(gaddr lo, gaddr hi);
bool is_readonly(gaddr a);

// Guest heap (dlmalloc living in guest memory)
gaddr gmalloc(u32 n);
gaddr gcalloc(u32 n, u32 size);
gaddr grealloc(gaddr p, u32 n);
void gfree(gaddr p);
gaddr gmemalign(u32 align, u32 n);
u32 gusable_size(gaddr p);

struct HeapInfo {
    u32 arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost;
};
HeapInfo heap_info();

gaddr gstrdup(const char* s);
gaddr gdup(const void* p, u32 n);

MemoryStats get_memory_stats();

template <typename T>
inline T rd(gaddr a) { return zb::rd<T>(a); }

template <typename T>
inline void wr(gaddr a, T v) { zb::wr<T>(a, v); }

} // namespace zb::mem
