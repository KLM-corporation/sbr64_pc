#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cerrno>
#include "core/common.h"
#include "core/mem_win32.h"
#include "core/sync_win32.h"

// Simulated Bionic LP32 Mutex on a guest word
struct BionicMutex {
    zb::gaddr addr = 0;

    BionicMutex() {
        addr = zb::mem::map_anon(4096, 3);
        *zb::hp<uint32_t>(addr) = 0;
    }
    ~BionicMutex() {
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

void test_futex_wait_timeout() {
    printf("[TEST] Testing Futex Wait Timeout...\n");
    zb::gaddr sync_addr = zb::mem::map_anon(4096, 3);
    *zb::hp<uint32_t>(sync_addr) = 100;
    auto t0 = std::chrono::steady_clock::now();
    int r = zb::sync::futex_wait_ms(sync_addr, 100, 50); // 50 ms timeout
    auto t1 = std::chrono::steady_clock::now();
    int elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    (void)r;
    assert(r == ETIMEDOUT);
    assert(elapsed_ms >= 40 && elapsed_ms <= 200);
    printf("       Timed out after %d ms as expected\n", elapsed_ms);
    printf("[PASS] Futex Timeout OK\n");
    zb::mem::unmap_anon(sync_addr, 4096);
}

void test_futex_wait_mismatch() {
    printf("[TEST] Testing Futex Wait Value Mismatch...\n");
    zb::gaddr sync_addr = zb::mem::map_anon(4096, 3);
    *zb::hp<uint32_t>(sync_addr) = 200;
    int r = zb::sync::futex_wait_ms(sync_addr, 150, 1000); // expect 150, actually 200
    (void)r;
    assert(r == EWOULDBLOCK);
    printf("[PASS] Futex Value Mismatch OK\n");
    zb::mem::unmap_anon(sync_addr, 4096);
}

void test_bionic_mutex_concurrency() {
    printf("[TEST] Testing High-Concurrency Bionic Mutex Emulation...\n");
    BionicMutex mtx;
    int counter = 0;
    constexpr int kThreads = 8;
    constexpr int kIters = 20000;

    std::vector<std::thread> ths;
    for (int t = 0; t < kThreads; ++t) {
        ths.emplace_back([&]() {
            for (int i = 0; i < kIters; ++i) {
                mtx.lock();
                counter++;
                mtx.unlock();
            }
        });
    }
    for (auto& th : ths) th.join();

    assert(counter == kThreads * kIters);
    printf("       Final counter = %d across %d threads\n", counter, kThreads);
    printf("[PASS] Concurrent Mutex OK\n");
}

int main() {
    printf("=== Starting Win32 Synchronization Test Suite ===\n");
    zb::mem::init();
    zb::sync::init();
    test_futex_wait_timeout();
    test_futex_wait_mismatch();
    test_bionic_mutex_concurrency();
    printf("=== All Synchronization Tests PASSED ===\n");
    return 0;
}
