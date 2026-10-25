#include "sync_win32.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <synchapi.h>

#include <cerrno>
#include <climits>
#include <chrono>
#include <algorithm>

namespace zb::sync {

void init() {
    // Win32 WaitOnAddress is natively supported on Windows 8 and later.
    // No special initialization required; synchronization library is linked statically.
}

int futex_wait_ms(gaddr addr, uint32_t expected_val, uint32_t timeout_ms) {
    if (addr % 4 != 0) {
        return EINVAL;
    }

    volatile uint32_t* ptr = hp<volatile uint32_t>(addr);

    // Atomic pre-check: if value changed, return immediately with EWOULDBLOCK
    uint32_t cur = __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
    if (cur != expected_val) {
        return EWOULDBLOCK;
    }

    DWORD dwMs = (timeout_ms == 0xFFFFFFFFu) ? INFINITE : timeout_ms;
    BOOL ok = WaitOnAddress(ptr, &expected_val, sizeof(uint32_t), dwMs);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_TIMEOUT) return ETIMEDOUT;
        return EINVAL;
    }
    return 0;
}

int futex_wait(gaddr addr, uint32_t expected_val, const struct timespec* rel_timeout) {
    uint32_t timeout_ms = 0xFFFFFFFFu;
    if (rel_timeout) {
        if (rel_timeout->tv_sec < 0 || rel_timeout->tv_nsec < 0 || rel_timeout->tv_nsec >= 1000000000L) {
            return EINVAL;
        }
        int64_t ms = static_cast<int64_t>(rel_timeout->tv_sec) * 1000 + (rel_timeout->tv_nsec + 999999) / 1000000;
        if (ms < 0) ms = 0;
        if (ms > 0xFFFFFFFEll) timeout_ms = 0xFFFFFFFE;
        else timeout_ms = static_cast<uint32_t>(ms);
    }
    return futex_wait_ms(addr, expected_val, timeout_ms);
}

int futex_wait_bitset(gaddr addr, uint32_t expected_val, const struct timespec* abs_timeout, uint32_t bitset, bool clock_realtime) {
    if (bitset == 0) {
        return EINVAL;
    }
    if (addr % 4 != 0) {
        return EINVAL;
    }

    volatile uint32_t* ptr = hp<volatile uint32_t>(addr);

    // Atomic pre-check: if value changed, return immediately with EWOULDBLOCK
    uint32_t cur = __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
    if (cur != expected_val) {
        return EWOULDBLOCK;
    }

    uint32_t timeout_ms = 0xFFFFFFFFu;
    if (abs_timeout) {
        if (abs_timeout->tv_sec < 0 || abs_timeout->tv_nsec < 0 || abs_timeout->tv_nsec >= 1000000000L) {
            return EINVAL;
        }

        int64_t target_ns = static_cast<int64_t>(abs_timeout->tv_sec) * 1000000000LL + abs_timeout->tv_nsec;
        int64_t now_ns = clock_realtime
            ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()
            : std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

        int64_t diff_ns = target_ns - now_ns;
        if (diff_ns <= 0) {
            return ETIMEDOUT;
        }

        int64_t ms = (diff_ns + 999999LL) / 1000000LL;
        if (ms > 0xFFFFFFFELL) {
            timeout_ms = 0xFFFFFFFE;
        } else {
            timeout_ms = static_cast<uint32_t>(ms);
        }
    }

    DWORD dwMs = (timeout_ms == 0xFFFFFFFFu) ? INFINITE : timeout_ms;
    BOOL ok = WaitOnAddress(ptr, &expected_val, sizeof(uint32_t), dwMs);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_TIMEOUT) return ETIMEDOUT;
        return EINVAL;
    }
    return 0;
}

int futex_wake(gaddr addr, int count) {
    if (count <= 0) return 0;
    void* ptr = hp(addr);
    if (count == 1) {
        WakeByAddressSingle(ptr);
        return 1;
    } else {
        WakeByAddressAll(ptr);
        return count;
    }
}

int futex_wake_bitset(gaddr addr, int count, uint32_t bitset) {
    if (bitset == 0) {
        return EINVAL;
    }
    if (addr % 4 != 0) {
        return EINVAL;
    }
    return futex_wake(addr, count);
}

int futex_requeue(gaddr addr1, int wake_count, int requeue_count, gaddr /*addr2*/) {
    void* ptr1 = hp(addr1);
    for (int i = 0; i < wake_count; ++i) {
        WakeByAddressSingle(ptr1);
    }
    if (requeue_count > 0) {
        WakeByAddressAll(ptr1);
    }
    return wake_count + requeue_count;
}

int sys_futex(gaddr uaddr, int op, uint32_t val, const struct timespec* timeout, gaddr uaddr2, uint32_t val3) {
    int cmd = op & 127;
    bool clock_realtime = (op & 256) != 0;
    switch (cmd) {
    case kFutexWait:
        return futex_wait(uaddr, val, timeout);
    case kFutexWake:
        return futex_wake(uaddr, static_cast<int>(val));
    case kFutexWaitBitset:
        return futex_wait_bitset(uaddr, val, timeout, val3, clock_realtime);
    case kFutexWakeBitset:
        return futex_wake_bitset(uaddr, static_cast<int>(val), val3);
    case kFutexRequeue: {
        int requeue_count = timeout ? static_cast<int>(reinterpret_cast<uintptr_t>(timeout)) : 0;
        return futex_requeue(uaddr, static_cast<int>(val), requeue_count, uaddr2);
    }
    case kFutexCmpRequeue: {
        if (uaddr % 4 != 0) {
            return EINVAL;
        }
        volatile uint32_t* ptr = hp<volatile uint32_t>(uaddr);
        uint32_t cur = __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
        if (cur != val3) {
            return EAGAIN;
        }
        int requeue_count = timeout ? static_cast<int>(reinterpret_cast<uintptr_t>(timeout)) : 0;
        return futex_requeue(uaddr, static_cast<int>(val), requeue_count, uaddr2);
    }
    default:
        ZLOGW("sys_futex: unsupported op %d", op);
        return ENOSYS;
    }
}

} // namespace zb::sync
