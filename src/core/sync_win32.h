#pragma once

#include <cstdint>
#include <cstddef>
#include <ctime>

#include "common.h"

namespace zb::sync {

// Standard Linux Futex constants for emulation
enum FutexOp {
    kFutexWait = 0,
    kFutexWake = 1,
    kFutexFd = 2,
    kFutexRequeue = 3,
    kFutexCmpRequeue = 4,
    kFutexWaitBitset = 9,
    kFutexWakeBitset = 10,
    kFutexWaitPrivate = 128,
    kFutexWakePrivate = 129,
    kFutexRequeuePrivate = 131,
    kFutexCmpRequeuePrivate = 132,
    kFutexWaitBitsetPrivate = 137,
    kFutexWakeBitsetPrivate = 138,
    kFutexClockRealtime = 256,
    kFutexBitsetMatchAny = 0xFFFFFFFFu,
};

// Lifecycle
void init();

// Core futex operations (returns 0 on success, POSIX errno on failure)
int futex_wait(gaddr addr, uint32_t expected_val, const struct timespec* rel_timeout);
int futex_wait_ms(gaddr addr, uint32_t expected_val, uint32_t timeout_ms = 0xFFFFFFFFu);
int futex_wait_bitset(gaddr addr, uint32_t expected_val, const struct timespec* abs_timeout, uint32_t bitset, bool clock_realtime);
int futex_wake(gaddr addr, int count);
int futex_wake_bitset(gaddr addr, int count, uint32_t bitset);
int futex_requeue(gaddr addr1, int wake_count, int requeue_count, gaddr addr2);

// Overloads taking uint32_t* host pointers (matching PROJECT.md interface contract)
inline int futex_wait(uint32_t* addr, uint32_t val, uint32_t timeout_ms = 0xFFFFFFFFu) {
    return futex_wait_ms(ga(addr), val, timeout_ms);
}
inline int futex_wake(uint32_t* addr, int count) {
    return futex_wake(ga(addr), count);
}

// Direct Linux syscall replacement
int sys_futex(gaddr uaddr, int op, uint32_t val, const struct timespec* timeout, gaddr uaddr2, uint32_t val3);

} // namespace zb::sync
