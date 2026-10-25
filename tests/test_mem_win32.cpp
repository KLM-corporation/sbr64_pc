#include <cassert>
#include <cstdio>
#include <cstring>
#include "core/mem_win32.h"

void test_identity_and_pagetable() {
    printf("[TEST] Testing Win32 Memory Initialization...\n");
    zb::mem::MemoryConfig cfg;
    cfg.enable_identity_map = true;
    zb::mem::init(cfg);

    auto stats = zb::mem::get_memory_stats();
    printf("       Reserved: %llu MB, Identity: %s, Base: 0x%llX\n",
           stats.total_reserved_bytes >> 20, stats.is_identity_mapped ? "YES" : "NO",
           (unsigned long long)stats.guest_base);
    assert(zb::mem::page_table() != nullptr);

    // Verify NULL page is unmapped
    assert(!zb::mem::is_mapped(0x00000000));
    assert(!zb::mem::is_mapped(0x00001000));

    printf("[PASS] Memory Initialization & PageTable OK\n");
}

void test_anon_alloc_and_pages() {
    printf("[TEST] Testing Anonymous Mapping & Page Table Tracking...\n");
    zb::gaddr addr = zb::mem::map_anon(64 * 1024, 3 /* PROT_READ | PROT_WRITE */, "test-vma");
    assert(addr != 0);
    assert(addr >= 0x00100000 && addr < 0xFFFF0000);
    assert(zb::mem::is_mapped(addr));
    assert(zb::mem::is_range_mapped(addr, 64 * 1024));

    // Verify host pointer access & page table entry
    uint8_t* host_ptr = zb::hp<uint8_t>(addr);
    auto* pt = zb::mem::page_table();
    assert((*pt)[addr >> zb::mem::kGuestPageBits] == host_ptr);

    // Write & verify memory pattern
    std::memset(host_ptr, 0xAA, 64 * 1024);
    assert(host_ptr[0] == 0xAA);
    assert(host_ptr[64 * 1024 - 1] == 0xAA);

    // Unmap and verify pages reset
    zb::mem::unmap_anon(addr, 64 * 1024);
    assert(!zb::mem::is_mapped(addr));
    assert((*pt)[addr >> zb::mem::kGuestPageBits] == nullptr);

    printf("[PASS] Anonymous Mapping & Unmapping OK\n");
}

void test_guest_heap() {
    printf("[TEST] Testing Guest dlmalloc Heap...\n");
    zb::gaddr p1 = zb::mem::gmalloc(256);
    assert(p1 != 0);
    zb::gaddr p2 = zb::mem::gcalloc(100, sizeof(uint32_t));
    assert(p2 != 0);

    // Check memory zeroed
    uint32_t* h2 = zb::hp<uint32_t>(p2);
    for (int i = 0; i < 100; ++i) assert(h2[i] == 0);

    zb::gaddr p3 = zb::mem::gstrdup("Sackboy Native PC");
    assert(std::strcmp(zb::hp<char>(p3), "Sackboy Native PC") == 0);

    zb::mem::gfree(p1);
    zb::mem::gfree(p2);
    zb::mem::gfree(p3);

    auto hi = zb::mem::heap_info();
    printf("       Heap arena: %u bytes, in-use: %u bytes\n", hi.arena, hi.uordblks);
    printf("[PASS] Guest Heap OK\n");
}

int main() {
    printf("=== Starting Win32 Memory Test Suite ===\n");
    test_identity_and_pagetable();
    test_anon_alloc_and_pages();
    test_guest_heap();
    zb::mem::shutdown();
    printf("=== All Memory Tests PASSED ===\n");
    return 0;
}
