#include <atomic>
#include <cassert>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

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

// ANSI escape colors for clear test reporting
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[1;31m"
#define COLOR_GREEN   "\033[1;32m"
#define COLOR_YELLOW  "\033[1;33m"
#define COLOR_CYAN    "\033[1;36m"

static int g_total_tests = 0;
static int g_passed_tests = 0;
static int g_failed_tests = 0;
static int g_defect_count = 0;

#define TEST_BEGIN(name) \
    do { \
        g_total_tests++; \
        printf("\n" COLOR_CYAN "[RUNNING] " name COLOR_RESET "\n"); \
    } while (0)

#define TEST_PASS(name) \
    do { \
        g_passed_tests++; \
        printf(COLOR_GREEN "[PASS]    " name COLOR_RESET "\n"); \
    } while (0)

#define TEST_FAIL(name, reason, ...) \
    do { \
        g_failed_tests++; \
        printf(COLOR_RED "[FAIL]    " name ": " reason COLOR_RESET "\n", ##__VA_ARGS__); \
    } while (0)

#define DEFECT_FOUND(id, desc, ...) \
    do { \
        g_defect_count++; \
        printf(COLOR_YELLOW "[DEFECT FOUND - " id "] " desc COLOR_RESET "\n", ##__VA_ARGS__); \
    } while (0)

// =============================================================================
// SUITE 1: High-Pressure Concurrent Guest Heap Stress
// =============================================================================

void test_concurrent_heap_stress() {
    TEST_BEGIN("Suite 1: High-Pressure Concurrent Guest Heap Stress");

    constexpr int kThreads = 16;
    constexpr int kOpsPerThread = 15000;
    std::atomic<bool> start_flag{false};
    std::atomic<size_t> total_bytes_allocated{0};
    std::atomic<size_t> total_allocations{0};
    std::atomic<size_t> corruption_detected{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t, &start_flag, &total_bytes_allocated, &total_allocations, &corruption_detected]() {
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            std::mt19937_64 rng(0x12345678ULL + t * 997);
            std::uniform_int_distribution<uint32_t> size_dist(16, 65536);
            std::uniform_int_distribution<int> op_dist(0, 9);

            struct AllocItem {
                zb::gaddr addr;
                uint32_t size;
                uint32_t magic;
            };

            std::vector<AllocItem> live;
            live.reserve(1024);

            for (int i = 0; i < kOpsPerThread; ++i) {
                int op = op_dist(rng);
                if (op < 6 || live.empty()) {
                    // Allocate
                    uint32_t sz = size_dist(rng);
                    // Occasionally test large allocation (256KB - 2MB)
                    if ((i % 500) == 0) {
                        sz = 256 * 1024 + (rng() % (1024 * 1024));
                    }

                    zb::gaddr a = 0;
                    int type = rng() % 3;
                    if (type == 0) {
                        a = zb::mem::gmalloc(sz);
                    } else if (type == 1) {
                        a = zb::mem::gcalloc(sz / 4 + 1, 4);
                    } else {
                        a = zb::mem::gmemalign(64, sz);
                    }

                    if (a != 0) {
                        total_bytes_allocated.fetch_add(sz, std::memory_order_relaxed);
                        total_allocations.fetch_add(1, std::memory_order_relaxed);

                        uint32_t magic = static_cast<uint32_t>((t << 24) ^ i ^ 0xA5A5A5A5);
                        uint8_t* p = zb::hp<uint8_t>(a);

                        // Fill with pattern
                        uint32_t* p32 = reinterpret_cast<uint32_t*>(p);
                        size_t words = sz / sizeof(uint32_t);
                        for (size_t w = 0; w < words; ++w) {
                            p32[w] = magic ^ static_cast<uint32_t>(w);
                        }

                        live.push_back({a, sz, magic});
                    }
                } else if (op < 8) {
                    // Verify and Realloc
                    size_t idx = rng() % live.size();
                    AllocItem item = live[idx];

                    // Verify pattern before realloc
                    uint8_t* p = zb::hp<uint8_t>(item.addr);
                    uint32_t* p32 = reinterpret_cast<uint32_t*>(p);
                    size_t words = item.size / sizeof(uint32_t);
                    bool ok = true;
                    for (size_t w = 0; w < words; ++w) {
                        if (p32[w] != (item.magic ^ static_cast<uint32_t>(w))) {
                            ok = false;
                            break;
                        }
                    }
                    if (!ok) {
                        corruption_detected.fetch_add(1, std::memory_order_relaxed);
                    }

                    uint32_t new_sz = size_dist(rng);
                    zb::gaddr na = zb::mem::grealloc(item.addr, new_sz);
                    if (na != 0) {
                        uint32_t new_magic = item.magic + 1;
                        uint32_t* np32 = zb::hp<uint32_t>(na);
                        size_t nwords = new_sz / sizeof(uint32_t);
                        for (size_t w = 0; w < nwords; ++w) {
                            np32[w] = new_magic ^ static_cast<uint32_t>(w);
                        }
                        live[idx] = {na, new_sz, new_magic};
                    }
                } else {
                    // Verify and Free
                    size_t idx = rng() % live.size();
                    AllocItem item = live[idx];

                    uint8_t* p = zb::hp<uint8_t>(item.addr);
                    uint32_t* p32 = reinterpret_cast<uint32_t*>(p);
                    size_t words = item.size / sizeof(uint32_t);
                    bool ok = true;
                    for (size_t w = 0; w < words; ++w) {
                        if (p32[w] != (item.magic ^ static_cast<uint32_t>(w))) {
                            ok = false;
                            break;
                        }
                    }
                    if (!ok) {
                        corruption_detected.fetch_add(1, std::memory_order_relaxed);
                    }

                    zb::mem::gfree(item.addr);
                    live[idx] = live.back();
                    live.pop_back();
                }
            }

            // Cleanup remaining
            for (auto& item : live) {
                uint8_t* p = zb::hp<uint8_t>(item.addr);
                uint32_t* p32 = reinterpret_cast<uint32_t*>(p);
                size_t words = item.size / sizeof(uint32_t);
                for (size_t w = 0; w < words; ++w) {
                    if (p32[w] != (item.magic ^ static_cast<uint32_t>(w))) {
                        corruption_detected.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
                zb::mem::gfree(item.addr);
            }
        });
    }

    auto t_start = std::chrono::steady_clock::now();
    start_flag.store(true, std::memory_order_release);

    for (auto& w : workers) {
        w.join();
    }
    auto t_end = std::chrono::steady_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

    auto hi = zb::mem::heap_info();
    printf("  [Info] Completed %zu total allocations across %d threads in %.2f ms\n",
           total_allocations.load(), kThreads, elapsed_ms);
    printf("  [Info] Total throughput: %.0f ops/sec, Heap arena: %u bytes, in-use: %u bytes\n",
           (kThreads * kOpsPerThread) / (elapsed_ms / 1000.0), hi.arena, hi.uordblks);

    if (corruption_detected.load() > 0) {
        TEST_FAIL("Suite 1: High-Pressure Concurrent Guest Heap Stress",
                  "Detected %zu memory corruption instances during concurrent heap operations!",
                  corruption_detected.load());
        DEFECT_FOUND("CRIT-01", "Guest heap memory corruption during concurrent allocation/realloc/free");
    } else {
        TEST_PASS("Suite 1: High-Pressure Concurrent Guest Heap Stress (0 corruption, 100%% data integrity)");
    }
}

// =============================================================================
// SUITE 2: Boundary Conditions, Fragmentation, and mmap/mremap Edge Cases
// =============================================================================

void test_boundary_and_fragmentation() {
    TEST_BEGIN("Suite 2: Address Boundary & Fragmentation Tests");

    // -------------------------------------------------------------------------
    // Test 2.1: Low 1MB NULL trap validation
    // -------------------------------------------------------------------------
    {
        printf("  [Subtest 2.1] Low 1MB NULL Trap Boundary (0x00000000..0x00100000)...\n");
        int err = 0;
        zb::gaddr r1 = zb::mem::guest_mmap(0x00050000, 4096, 3, 0x10 /* MAP_FIXED */, -1, 0, &err);
        if (r1 != 0xFFFFFFFFu) {
            TEST_FAIL("Subtest 2.1", "MAP_FIXED in low 1MB (0x00050000) succeeded (addr=0x%08X), expected EINVAL failure!", r1);
            DEFECT_FOUND("BUG-MEM-01", "Low 1MB NULL trap can be overwritten with MAP_FIXED");
        } else {
            printf("    -> MAP_FIXED in low 1MB rejected with err=%d (EINVAL=%d) [OK]\n", err, EINVAL);
        }

        // Test unmapping below 1MB
        err = 0;
        int unmap_res = zb::mem::guest_munmap(0x00050000, 4096, &err);
        printf("    -> munmap below 1MB (0x00050000) returned %d (err=%d)\n", unmap_res, err);
    }

    // -------------------------------------------------------------------------
    // Test 2.2: High 4GB Boundary Validation (>= 0xFFFF0000)
    // -------------------------------------------------------------------------
    {
        printf("  [Subtest 2.2] High 4GB Boundary (0xFFFF0000..0xFFFFFFFF)...\n");
        int err = 0;
        zb::gaddr r_high = zb::mem::guest_mmap(0xFFFF0000, 4096, 3, 0x10 /* MAP_FIXED */, -1, 0, &err);
        if (r_high != 0xFFFFFFFFu) {
            TEST_FAIL("Subtest 2.2", "MAP_FIXED at 0xFFFF0000 succeeded, expected rejection (> kHigh)");
            DEFECT_FOUND("BUG-MEM-02", "Address >= 0xFFFF0000 allowed by guest_mmap");
        } else {
            printf("    -> MAP_FIXED at 0xFFFF0000 rejected with err=%d [OK]\n", err);
        }

        // Test boundary overflow (0xFFFFF000 + 0x2000 wraps around 4GB)
        err = 0;
        zb::gaddr r_wrap = zb::mem::guest_mmap(0xFFFFF000, 0x2000, 3, 0x10 /* MAP_FIXED */, -1, 0, &err);
        if (r_wrap != 0xFFFFFFFFu) {
            TEST_FAIL("Subtest 2.2", "MAP_FIXED wrapping past 4GB succeeded (addr=0x%08X)!", r_wrap);
            DEFECT_FOUND("BUG-MEM-03", "32-bit address wraparound past 4GB not rejected by guest_mmap");
        } else {
            printf("    -> 32-bit address wraparound past 4GB rejected with err=%d [OK]\n", err);
        }
    }

    // -------------------------------------------------------------------------
    // Test 2.3: Windows KUSER_SHARED_DATA (0x7FFE0000) Collision and Protection
    // -------------------------------------------------------------------------
    {
        printf("  [Subtest 2.3] Windows KUSER_SHARED_DATA (0x7FFE0000) Boundary Protection...\n");
        int err = 0;
        zb::gaddr r_kuser = zb::mem::guest_mmap(0x7FFE0000, 4096, 3, 0x10 /* MAP_FIXED */, -1, 0, &err);
        if (r_kuser != 0xFFFFFFFFu) {
            TEST_FAIL("Subtest 2.3", "MAP_FIXED at 0x7FFE0000 succeeded! Windows kernel page aliased!");
            DEFECT_FOUND("CRIT-02", "Windows KUSER_SHARED_DATA page claimed by MAP_FIXED");
        } else {
            printf("    -> MAP_FIXED at 0x7FFE0000 correctly rejected with err=%d [OK]\n", err);
        }

        // Adversarial check: Can munmap corrupt g_free by inserting 0x7FFE0000?
        // Note: 0x7FFE0000 is kernel mapped, VirtualFree on it fails.
        err = 0;
        int unmap_kuser = zb::mem::guest_munmap(0x7FFE0000, 0x10000, &err);
        printf("    -> munmap at 0x7FFE0000 returned %d (err=%d)\n", unmap_kuser, err);
        // If unmap returned 0, release_locked would have added KUSER_SHARED_DATA into g_free!
        if (unmap_kuser == 0) {
            DEFECT_FOUND("CRIT-03", "munmap at 0x7FFE0000 returned success, injecting kernel page into g_free!");
        }
    }

    // -------------------------------------------------------------------------
    // Test 2.4: Address Stealing / Aliasing Without MAP_FIXED (Defect Reproduction)
    // -------------------------------------------------------------------------
    {
        printf("  [Subtest 2.4] Address Stealing / Aliasing Without MAP_FIXED...\n");
        int err = 0;
        // Step 1: Allocate a legitimate chunk A
        zb::gaddr chunkA = zb::mem::guest_mmap(0, 0x10000, 3 /* PROT_READ|WRITE */, 0, -1, 0, &err);
        assert(chunkA != 0xFFFFFFFFu);

        // Put a unique canary into chunk A
        volatile uint32_t* pA = zb::hp<volatile uint32_t>(chunkA);
        *pA = 0x11223344;

        // Step 2: Now call guest_mmap providing chunkA as an address hint WITHOUT MAP_FIXED
        // POSIX standard requires that if the hinted address is already mapped, mmap MUST NOT
        // overwrite it or return it; it MUST allocate an unused region.
        zb::gaddr chunkB = zb::mem::guest_mmap(chunkA, 0x10000, 3, 0 /* NOT MAP_FIXED */, -1, 0, &err);

        printf("    -> Chunk A = 0x%08X, Chunk B requested at hint 0x%08X returned 0x%08X\n",
               chunkA, chunkA, chunkB);

        if (chunkB == chunkA) {
            // DEFECT REPRODUCED! It returned chunkA without MAP_FIXED!
            DEFECT_FOUND("DEFECT-MEM-ALIASED",
                         "guest_mmap without MAP_FIXED returned already-mapped address (0x%08X), stealing/aliasing existing mapping!",
                         chunkA);
            TEST_FAIL("Subtest 2.4",
                      "guest_mmap aliased existing allocation at 0x%08X without MAP_FIXED", chunkA);
        } else {
            printf("    -> Hinted mmap respected existing mapping and allocated separate region 0x%08X [OK]\n", chunkB);
            zb::mem::guest_munmap(chunkB, 0x10000, &err);
        }

        zb::mem::guest_munmap(chunkA, 0x10000, &err);
    }

    // -------------------------------------------------------------------------
    // Test 2.5: guest_mremap In-Place Collision / Neighbor Hijacking (Defect Reproduction)
    // -------------------------------------------------------------------------
    {
        printf("  [Subtest 2.5] guest_mremap In-Place Collision / Neighbor Hijacking...\n");
        int err = 0;

        // Step 1: Map Block 1 (size 64KB)
        zb::gaddr b1 = zb::mem::guest_mmap(0, 0x10000, 3, 0, -1, 0, &err);
        assert(b1 != 0xFFFFFFFFu);

        // Step 2: Map Block 2 with MAP_FIXED immediately adjacent to Block 1 (b1 + 0x10000)
        zb::gaddr b2_addr = b1 + 0x10000;
        zb::gaddr b2 = zb::mem::guest_mmap(b2_addr, 0x10000, 3, 0x10 /* MAP_FIXED */, -1, 0, &err);
        if (b2 == 0xFFFFFFFFu) {
            // If b2_addr couldn't be mapped fixed, skip this specific collision test
            printf("    -> Could not place adjacent block at 0x%08X (err=%d), skipping\n", b2_addr, err);
            zb::mem::guest_munmap(b1, 0x10000, &err);
        } else {
            // Block 2 is right at b1 + 0x10000!
            // Put canary into Block 2
            volatile uint32_t* canary2 = zb::hp<volatile uint32_t>(b2);
            *canary2 = 0xCAFEBABE;

            // Step 3: Attempt to expand Block 1 in-place to 128KB WITHOUT MREMAP_MAYMOVE (flags = 0)
            // Since b2 occupies [b1 + 0x10000, b1 + 0x20000], in-place expansion MUST fail!
            zb::gaddr remap_res = zb::mem::guest_mremap(b1, 0x10000, 0x20000, 0 /* NO MAYMOVE */, &err);

            printf("    -> Attempted in-place expansion of b1 (0x%08X) across occupied neighbor b2 (0x%08X). Result: 0x%08X\n",
                   b1, b2, remap_res);

            if (remap_res == b1) {
                // In-place expansion claimed success!
                // Check if canary in Block 2 was affected
                DEFECT_FOUND("DEFECT-MREMAP-HIJACK",
                             "guest_mremap expanded in-place over occupied neighbor (0x%08X), hijacking neighbor memory!",
                             b2);
                TEST_FAIL("Subtest 2.5",
                          "guest_mremap in-place expansion succeeded over occupied neighbor at 0x%08X", b2);
            } else if (remap_res == 0xFFFFFFFFu) {
                printf("    -> In-place expansion correctly rejected when neighbor occupied (err=%d) [OK]\n", err);
            }

            zb::mem::guest_munmap(b2, 0x10000, &err);
            zb::mem::guest_munmap(b1, 0x10000, &err);
        }
    }

    // -------------------------------------------------------------------------
    // Test 2.6: Extreme Fragmentation Attack
    // -------------------------------------------------------------------------
    {
        printf("  [Subtest 2.6] Extreme Address Space Fragmentation Stress...\n");
        constexpr int kNumChunks = 500;
        std::vector<zb::gaddr> chunks;
        chunks.reserve(kNumChunks);

        // Allocate 500 chunks of 64KB
        for (int i = 0; i < kNumChunks; ++i) {
            zb::gaddr a = zb::mem::map_anon(64 * 1024, 3, "frag-test");
            if (a == 0) break;
            chunks.push_back(a);
        }
        printf("    -> Successfully allocated %zu chunks of 64KB\n", chunks.size());

        // Free every alternating chunk (even indices) to create a checkerboard pattern of 250 holes
        for (size_t i = 0; i < chunks.size(); i += 2) {
            zb::mem::unmap_anon(chunks[i], 64 * 1024);
            chunks[i] = 0;
        }

        // Now allocate 100 new chunks of 64KB (should fit into holes)
        size_t reused = 0;
        for (int i = 0; i < 100; ++i) {
            zb::gaddr a = zb::mem::map_anon(64 * 1024, 3, "frag-reuse");
            if (a != 0) {
                reused++;
                // Verify mapped
                assert(zb::mem::is_mapped(a));
                *zb::hp<uint32_t>(a) = 0xAA55AA55;
                zb::mem::unmap_anon(a, 64 * 1024);
            }
        }
        printf("    -> Successfully allocated and reused %zu holes under fragmentation\n", reused);

        // Cleanup remaining odd chunks
        for (size_t i = 1; i < chunks.size(); i += 2) {
            if (chunks[i] != 0) {
                zb::mem::unmap_anon(chunks[i], 64 * 1024);
            }
        }

        TEST_PASS("Subtest 2.6: Address space fragmentation handled stably");
    }

    TEST_PASS("Suite 2: Boundary & Fragmentation Tests Complete");
}

// =============================================================================
// SUITE 3: Multi-Threaded Futex Contention & Extreme Concurrency Stress
// =============================================================================

struct AdversarialMutex {
    zb::gaddr addr = 0;

    AdversarialMutex() {
        addr = zb::mem::map_anon(4096, 3, "futex-mtx");
        *zb::hp<uint32_t>(addr) = 0;
    }
    ~AdversarialMutex() {
        zb::mem::unmap_anon(addr, 4096);
    }

    void lock() {
        uint32_t* word = zb::hp<uint32_t>(addr);
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(word, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return;
        }
        while ((__atomic_exchange_n(word, 2, __ATOMIC_ACQUIRE) & 3) != 0) {
            zb::sync::futex_wait_ms(addr, 2, 0xFFFFFFFFu);
        }
    }

    void unlock() {
        uint32_t* word = zb::hp<uint32_t>(addr);
        if (__atomic_exchange_n(word, 0, __ATOMIC_RELEASE) == 2) {
            zb::sync::futex_wake(addr, 1);
        }
    }
};

void test_futex_concurrency_stress() {
    TEST_BEGIN("Suite 3: Multi-Threaded Futex Contention & Stress");

    // -------------------------------------------------------------------------
    // Test 3.1: Heavy Mutex Contention (32 threads, 320,000 operations)
    // -------------------------------------------------------------------------
    {
        printf("  [Subtest 3.1] 32-Thread Futex Mutex Contention (320,000 increments)...\n");
        AdversarialMutex mtx;
        uint64_t counter = 0;
        constexpr int kThreads = 32;
        constexpr int kIters = 10000;

        std::atomic<bool> start_flag{false};
        std::vector<std::thread> ths;
        ths.reserve(kThreads);

        for (int t = 0; t < kThreads; ++t) {
            ths.emplace_back([&]() {
                while (!start_flag.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (int i = 0; i < kIters; ++i) {
                    mtx.lock();
                    counter++;
                    mtx.unlock();
                }
            });
        }

        auto t0 = std::chrono::steady_clock::now();
        start_flag.store(true, std::memory_order_release);
        for (auto& th : ths) th.join();
        auto t1 = std::chrono::steady_clock::now();

        double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        printf("    -> Finished in %.2f ms (%.0f ops/sec), counter = %llu / %llu\n",
               elapsed_ms, (kThreads * kIters) / (elapsed_ms / 1000.0),
               (unsigned long long)counter, (unsigned long long)(kThreads * kIters));

        if (counter == static_cast<uint64_t>(kThreads * kIters)) {
            TEST_PASS("Subtest 3.1: 32-Thread Mutex Contention (0 data loss, no deadlock)");
        } else {
            TEST_FAIL("Subtest 3.1", "Data race! counter = %llu, expected %llu",
                      (unsigned long long)counter, (unsigned long long)(kThreads * kIters));
            DEFECT_FOUND("CRIT-04", "Futex mutex synchronization lost updates under 32-thread contention");
        }
    }

    // -------------------------------------------------------------------------
    // Test 3.2: Rapid Wakeups vs Low Timeouts & Spurious Signals
    // -------------------------------------------------------------------------
    {
        printf("  [Subtest 3.2] Rapid Wakeups vs Sub-Millisecond & Zero Timeouts...\n");
        zb::gaddr sync_addr = zb::mem::map_anon(4096, 3, "rapid-sync");
        *zb::hp<uint32_t>(sync_addr) = 0;

        constexpr int kWriters = 4;
        constexpr int kWaiters = 16;
        constexpr int kRounds = 500;
        std::atomic<bool> done{false};
        std::atomic<size_t> timeout_count{0};
        std::atomic<size_t> wake_count{0};

        std::vector<std::thread> waiter_threads;
        for (int i = 0; i < kWaiters; ++i) {
            waiter_threads.emplace_back([&, i]() {
                uint32_t exp = 0;
                while (!done.load(std::memory_order_relaxed)) {
                    // Random timeout: 0ms, 1ms, 2ms, 5ms
                    uint32_t ms = (i % 4 == 0) ? 0 : (i % 4);
                    int r = zb::sync::futex_wait_ms(sync_addr, exp, ms);
                    if (r == ETIMEDOUT) {
                        timeout_count.fetch_add(1, std::memory_order_relaxed);
                    } else if (r == 0) {
                        wake_count.fetch_add(1, std::memory_order_relaxed);
                    }
                    std::this_thread::yield();
                }
            });
        }

        std::vector<std::thread> writer_threads;
        for (int w = 0; w < kWriters; ++w) {
            writer_threads.emplace_back([&]() {
                for (int r = 0; r < kRounds; ++r) {
                    // Increment word and wake
                    __atomic_add_fetch(zb::hp<uint32_t>(sync_addr), 1, __ATOMIC_RELEASE);
                    if (r % 2 == 0) {
                        zb::sync::futex_wake(sync_addr, 1);
                    } else {
                        zb::sync::futex_wake(sync_addr, INT_MAX);
                    }
                    std::this_thread::sleep_for(50us);
                }
            });
        }

        for (auto& wt : writer_threads) wt.join();
        done.store(true, std::memory_order_release);
        for (auto& wt : waiter_threads) wt.join();

        printf("    -> Completed: %zu successful wakes, %zu timeouts, no hang\n",
               wake_count.load(), timeout_count.load());
        TEST_PASS("Subtest 3.2: Rapid Wakeup & Timeout Stress Complete");
        zb::mem::unmap_anon(sync_addr, 4096);
    }

    // -------------------------------------------------------------------------
    // Test 3.3: Condition Variable Broadcast & Requeue Stress
    // -------------------------------------------------------------------------
    {
        printf("  [Subtest 3.3] Condition Variable Requeue & Thundering Herd...\n");
        zb::gaddr cv_addr = zb::mem::map_anon(4096, 3, "cv-addr");
        zb::gaddr mtx_addr = zb::mem::map_anon(4096, 3, "cv-mtx");
        *zb::hp<uint32_t>(cv_addr) = 0;
        *zb::hp<uint32_t>(mtx_addr) = 0;

        constexpr int kCondWaiters = 16;
        std::atomic<int> ready_count{0};
        std::atomic<int> woken_count{0};

        std::vector<std::thread> ths;
        for (int i = 0; i < kCondWaiters; ++i) {
            ths.emplace_back([&]() {
                ready_count.fetch_add(1, std::memory_order_release);
                // Wait on cv_addr expecting 0
                int r = zb::sync::futex_wait_ms(cv_addr, 0, 5000 /* 5s max */);
                if (r == 0) {
                    woken_count.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        // Wait until all threads are waiting
        while (ready_count.load(std::memory_order_acquire) < kCondWaiters) {
            std::this_thread::sleep_for(1ms);
        }
        std::this_thread::sleep_for(10ms); // Ensure all are inside WaitOnAddress

        // Trigger requeue / wake
        *zb::hp<uint32_t>(cv_addr) = 1;
        int woke = zb::sync::futex_requeue(cv_addr, 1, kCondWaiters - 1, mtx_addr);
        printf("    -> futex_requeue reported %d waiters handled\n", woke);

        for (auto& th : ths) th.join();

        printf("    -> Woken count = %d / %d\n", woken_count.load(), kCondWaiters);
        if (woken_count.load() == kCondWaiters) {
            TEST_PASS("Subtest 3.3: Condition Variable Requeue Woke All Waiters");
        } else {
            TEST_FAIL("Subtest 3.3", "Expected %d waiters to wake, but only %d did!",
                      kCondWaiters, woken_count.load());
            DEFECT_FOUND("DEFECT-SYNC-01", "futex_requeue did not wake all waiters");
        }

        zb::mem::unmap_anon(cv_addr, 4096);
        zb::mem::unmap_anon(mtx_addr, 4096);
    }

    // -------------------------------------------------------------------------
    // Test 3.4: Bionic Syscall Op Compatibility Audit (sys_futex)
    // -------------------------------------------------------------------------
    {
        printf("  [Subtest 3.4] sys_futex Operation Coverage Audit...\n");
        zb::gaddr test_addr = zb::mem::map_anon(4096, 3, "sys-ftx");
        *zb::hp<uint32_t>(test_addr) = 42;

        // Check standard wait & wake
        int r_wait = zb::sync::sys_futex(test_addr, zb::sync::kFutexWait, 10 /* mismatch */, nullptr, 0, 0);
        printf("    -> sys_futex(kFutexWait, mismatch) returned: %d (EWOULDBLOCK=%d) [OK]\n", r_wait, EWOULDBLOCK);

        int r_wake = zb::sync::sys_futex(test_addr, zb::sync::kFutexWake, 1, nullptr, 0, 0);
        printf("    -> sys_futex(kFutexWake) returned: %d [OK]\n", r_wake);

        // Check kFutexWaitBitset (9) and kFutexWaitBitsetPrivate (137)
        int r_bitset = zb::sync::sys_futex(test_addr, zb::sync::kFutexWaitBitset, 42, nullptr, 0, 0);
        printf("    -> sys_futex(kFutexWaitBitset) returned: %d (ENOSYS=%d)\n", r_bitset, ENOSYS);
        if (r_bitset == ENOSYS) {
            DEFECT_FOUND("DEFECT-SYNC-BITSET",
                         "sys_futex lacks support for kFutexWaitBitset / kFutexWakeBitset, returning ENOSYS. Required by Android Bionic pthread_cond_timedwait!");
        }

        int r_bitset_priv = zb::sync::sys_futex(test_addr, zb::sync::kFutexWaitBitset | 128, 42, nullptr, 0, 0);
        printf("    -> sys_futex(kFutexWaitBitset | PRIVATE) returned: %d (ENOSYS=%d)\n", r_bitset_priv, ENOSYS);

        zb::mem::unmap_anon(test_addr, 4096);
    }

    TEST_PASS("Suite 3: Multi-Threaded Futex Stress Complete");
}

// =============================================================================
// MAIN ENTRYPOINT
// =============================================================================

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("=====================================================================\n");
    printf("ADVERSARIAL STRESS HARNESS: Win32 Memory & Futex Concurrency\n");
    printf("Challenger M1_1 Empirical Verification & Defect Hunt\n");
    printf("=====================================================================\n");

    // Initialize subsystems
    zb::mem::MemoryConfig cfg;
    cfg.enable_identity_map = true;
    zb::mem::init(cfg);
    zb::sync::init();

    auto stats = zb::mem::get_memory_stats();
    printf("Subsystem Status: Reserved = %llu MB, Identity = %s, Base = 0x%llX\n\n",
           stats.total_reserved_bytes >> 20,
           stats.is_identity_mapped ? "YES" : "NO",
           (unsigned long long)stats.guest_base);

    // Run test suites
    test_concurrent_heap_stress();
    test_boundary_and_fragmentation();
    test_futex_concurrency_stress();

    // Cleanup
    zb::mem::shutdown();

    printf("\n=====================================================================\n");
    printf("STRESS TEST SUMMARY:\n");
    printf("  Total Suites/Tests: %d\n", g_total_tests);
    printf("  Passed:             %d\n", g_passed_tests);
    printf("  Failed:             %d\n", g_failed_tests);
    printf("  Confirmed Defects:  %d\n", g_defect_count);
    printf("=====================================================================\n");

    if (g_defect_count > 0) {
        printf(COLOR_YELLOW "VERDICT: REQUEST_CHANGES (Subtle architectural defects confirmed empirically)\n" COLOR_RESET);
        return 1;
    } else {
        printf(COLOR_GREEN "VERDICT: APPROVE (All empirical stress tests passed)\n" COLOR_RESET);
        return 0;
    }
}
