// Host-side I/O primitives shared by the Android emulation layers:
// in-process pipes (the guest's android_native_app_glue command pipe) and a
// wake-up mechanism used by the emulated ALooper.
#pragma once

#include <cstddef>
#include <cstdint>

namespace zb::hostio {

// Pipes. File descriptors are allocated from a private range (>= 100) so they
// never collide with the stdio descriptors the guest knows.
int pipe_create(int fds[2]);
long pipe_read(int fd, void* buf, size_t n);          // -1 when empty (EAGAIN) or invalid
long pipe_write(int fd, const void* buf, size_t n);   // -1 when invalid
bool pipe_close(int fd);
bool is_pipe(int fd);
bool pipe_readable(int fd);

// Looper wake-up. A poller reads generation() BEFORE checking its sources and
// then calls wait(); wait() returns immediately if anything was notified in
// between, so no wake-up is lost.
void notify();
uint64_t generation();
void wait(uint64_t seen_generation, int timeout_ms);

}  // namespace zb::hostio
