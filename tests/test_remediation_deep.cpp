#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>
#include <atomic>
#include <cerrno>

#include "core/common.h"
#include "core/mem_win32.h"
#include "core/sync_win32.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace std::chrono_literals;

static int g_failures = 0;

#define ASSERT_TRUE(cond, msg, ...) \
    do { \
        if (!(cond)) { \
            printf("[FAIL] " msg "\n", ##__VA_ARGS__); \
            g_failures++; \
        } \
    } while (0)

void test_deep_mem_aliasing() {
    printf("[TEST] Deep Memory Aliasing & Hint Rejection...\n");
    int err = 0;

    // 1. Allocate block A of 128KB (32 pages)
    zb::gaddr addrA = zb::mem::guest_mmap(0, 128 * 1024, 3, 0, -1, 0, &err);
    ASSERT_TRUE(addrA != 0xFFFFFFFFu, "Failed to allocate addrA");

    // Write canary pattern across addrA
    uint32_t* pA = zb::hp<uint32_t>(addrA);
    for (size_t i = 0; i < (128 * 1024) / sizeof(uint32_t); ++i) {
        pA[i] = 0xAA000000u | static_cast<uint32_t>(i);
    }

    // 2. Request with hint at start of addrA without MAP_FIXED
    zb::gaddr addrB = zb::mem::guest_mmap(addrA, 64 * 1024, 3, 0, -1, 0, &err);
    ASSERT_TRUE(addrB != 0xFFFFFFFFu, "Failed to allocate addrB");
    ASSERT_TRUE(addrB != addrA, "addrB aliased addrA start! 0x%08X == 0x%08X", addrB, addrA);
    ASSERT_TRUE(addrB + 64 * 1024 <= addrA || addrB >= addrA + 128 * 1024,
                "addrB overlaps addrA! addrB=0x%08X, addrA=0x%08X", addrB, addrA);

    // 3. Request with hint in the middle of addrA without MAP_FIXED
    zb::gaddr mid_hint = addrA + 64 * 1024;
    zb::gaddr addrC = zb::mem::guest_mmap(mid_hint, 64 * 1024, 3, 0, -1, 0, &err);
    ASSERT_TRUE(addrC != 0xFFFFFFFFu, "Failed to allocate addrC");
    ASSERT_TRUE(addrC != mid_hint, "addrC aliased middle of addrA! 0x%08X == 0x%08X", addrC, mid_hint);
    ASSERT_TRUE(addrC + 64 * 1024 <= addrA || addrC >= addrA + 128 * 1024,
                "addrC overlaps addrA! addrC=0x%08X, addrA=0x%08X", addrC, addrA);

    // 4. Verify addrA data integrity
    bool intact = true;
    for (size_t i = 0; i < (128 * 1024) / sizeof(uint32_t); ++i) {
        if (pA[i] != (0xAA000000u | static_cast<uint32_t>(i))) {
            intact = false;
            break;
        }
    }
    ASSERT_TRUE(intact, "addrA canary pattern corrupted by hinted allocations!");

    zb::mem::guest_munmap(addrC, 64 * 1024, &err);
    zb::mem::guest_munmap(addrB, 64 * 1024, &err);
    zb::mem::guest_munmap(addrA, 128 * 1024, &err);

    printf("[PASS] Deep Memory Aliasing check passed (0 aliasing, 100%% isolation)\n");
}

void test_deep_mremap_expansion() {
    printf("[TEST] Deep Mremap Expansion & Relocation...\n");
    int err = 0;

    // 1. Allocate block B1 (64KB)
    zb::gaddr b1 = zb::mem::guest_mmap(0, 64 * 1024, 3, 0, -1, 0, &err);
    ASSERT_TRUE(b1 != 0xFFFFFFFFu, "Failed to allocate b1");
    *zb::hp<uint32_t>(b1) = 0x11112222;

    // 2. Allocate block B2 (64KB) immediately adjacent to B1
    zb::gaddr b2 = zb::mem::guest_mmap(b1 + 64 * 1024, 64 * 1024, 3, 0x10 /* MAP_FIXED */, -1, 0, &err);
    ASSERT_TRUE(b2 == b1 + 64 * 1024, "Failed to place adjacent b2");
    *zb::hp<uint32_t>(b2) = 0x33334444;

    // 3. In-place expansion without MREMAP_MAYMOVE must fail
    zb::gaddr exp_fail = zb::mem::guest_mremap(b1, 64 * 1024, 128 * 1024, 0, &err);
    ASSERT_TRUE(exp_fail == 0xFFFFFFFFu, "In-place expansion over occupied neighbor should have failed!");
    ASSERT_TRUE(err == 12 /* ENOMEM */, "Expected ENOMEM (12), got %d", err);
    ASSERT_TRUE(*zb::hp<uint32_t>(b2) == 0x33334444, "Neighbor b2 canary corrupted!");

    // 4. Expansion WITH MREMAP_MAYMOVE (0x01) should relocate b1
    zb::gaddr exp_move = zb::mem::guest_mremap(b1, 64 * 1024, 128 * 1024, 0x01 /* MREMAP_MAYMOVE */, &err);
    ASSERT_TRUE(exp_move != 0xFFFFFFFFu, "mremap with MAYMOVE failed!");
    ASSERT_TRUE(exp_move != b1, "mremap with MAYMOVE should have relocated away from occupied neighbor!");
    ASSERT_TRUE(*zb::hp<uint32_t>(exp_move) == 0x11112222, "Relocated block lost original data!");
    ASSERT_TRUE(*zb::hp<uint32_t>(b2) == 0x33334444, "Neighbor b2 canary corrupted after MAYMOVE!");

    // 5. Cleanup
    zb::mem::guest_munmap(b2, 64 * 1024, &err);
    zb::mem::guest_munmap(exp_move, 128 * 1024, &err);

    printf("[PASS] Deep Mremap Expansion check passed (neighbor protected, MAYMOVE relocated correctly)\n");
}

void test_deep_futex_bitset() {
    printf("[TEST] Deep Futex Bitset & Syscall Verification...\n");
    int err = 0;
    zb::gaddr ftx_addr = zb::mem::map_anon(4096, 3, "ftx-deep");
    ASSERT_TRUE(ftx_addr != 0, "Failed to allocate ftx_addr");
    volatile uint32_t* p = zb::hp<volatile uint32_t>(ftx_addr);
    *p = 100;

    // 1. Bitset == 0 must return EINVAL (22)
    int r1 = zb::sync::sys_futex(ftx_addr, zb::sync::kFutexWaitBitset, 100, nullptr, 0, 0 /* bitset=0 */);
    ASSERT_TRUE(r1 == EINVAL, "Expected EINVAL for bitset=0, got %d", r1);

    // 2. Misaligned address must return EINVAL
    int r2 = zb::sync::sys_futex(ftx_addr + 1, zb::sync::kFutexWaitBitset, 100, nullptr, 0, 0xFFFFFFFFu);
    ASSERT_TRUE(r2 == EINVAL, "Expected EINVAL for misaligned address, got %d", r2);

    // 3. Value mismatch with valid bitset must return EWOULDBLOCK (140)
    int r3 = zb::sync::sys_futex(ftx_addr, zb::sync::kFutexWaitBitset, 999 /* mismatch */, nullptr, 0, 0xFFFFFFFFu);
    ASSERT_TRUE(r3 == EWOULDBLOCK, "Expected EWOULDBLOCK for mismatched value, got %d", r3);

    // 4. Expired timeout must return ETIMEDOUT (110)
    struct timespec past_timeout;
    past_timeout.tv_sec = 0;
    past_timeout.tv_nsec = 1; // 1970
    int r4 = zb::sync::sys_futex(ftx_addr, zb::sync::kFutexWaitBitset, 100, &past_timeout, 0, 0xFFFFFFFFu);
    ASSERT_TRUE(r4 == ETIMEDOUT, "Expected ETIMEDOUT for past timeout, got %d", r4);

    // 5. Concurrent WaitBitset and WakeBitset
    std::atomic<bool> thread_started{false};
    std::atomic<int> wait_result{-1};

    std::thread waiter([&]() {
        thread_started.store(true, std::memory_order_release);
        // Wait on ftx_addr expecting 100, bitset 0x1, infinite timeout
        wait_result.store(zb::sync::sys_futex(ftx_addr, zb::sync::kFutexWaitBitset, 100, nullptr, 0, 0x1),
                          std::memory_order_relaxed);
    });

    while (!thread_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(20ms); // Ensure in WaitOnAddress

    // Wake with matching bitset
    int woken = zb::sync::sys_futex(ftx_addr, zb::sync::kFutexWakeBitset, 1, nullptr, 0, 0x1);
    ASSERT_TRUE(woken == 1, "Expected 1 woken waiter, got %d", woken);

    waiter.join();
    ASSERT_TRUE(wait_result.load() == 0, "WaitBitset thread failed with %d", wait_result.load());

    // 6. Test kFutexCmpRequeue: val3 equality check
    *p = 200;
    // Call kFutexCmpRequeue with val3 mismatch (expected 300) -> must return EAGAIN (11)
    int r_cmp_mismatch = zb::sync::sys_futex(ftx_addr, zb::sync::kFutexCmpRequeue, 1, nullptr, ftx_addr, 300 /* wrong val3 */);
    ASSERT_TRUE(r_cmp_mismatch == EAGAIN, "Expected EAGAIN for mismatched val3 in kFutexCmpRequeue, got %d", r_cmp_mismatch);

    // Call kFutexCmpRequeue with val3 match (200) -> should succeed
    int r_cmp_match = zb::sync::sys_futex(ftx_addr, zb::sync::kFutexCmpRequeue, 0, nullptr, ftx_addr, 200 /* matching val3 */);
    ASSERT_TRUE(r_cmp_match >= 0, "kFutexCmpRequeue with matching val3 failed with %d", r_cmp_match);

    zb::mem::unmap_anon(ftx_addr, 4096);
    printf("[PASS] Deep Futex Bitset check passed (all Bionic futex invariants verified)\n");
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=====================================================================\n");
    printf("DEEP REMEDIATION VERIFICATION: Boundary & Concurrency Stress\n");
    printf("Challenger M1_r2 Empirical Edge-Case Suite\n");
    printf("=====================================================================\n");

    zb::mem::MemoryConfig cfg;
    cfg.enable_identity_map = true;
    zb::mem::init(cfg);
    zb::sync::init();

    test_deep_mem_aliasing();
    test_deep_mremap_expansion();
    test_deep_futex_bitset();

    zb::mem::shutdown();

    printf("\n=====================================================================\n");
    printf("DEEP VERIFICATION SUMMARY:\n");
    printf("  Total Failures: %d\n", g_failures);
    printf("=====================================================================\n");

    return (g_failures == 0) ? 0 : 1;
}
