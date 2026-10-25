#include "platform/host_io.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace zb::hostio {
namespace {

struct Pipe {
    std::deque<uint8_t> data;
    bool write_closed = false;
};

std::mutex g_mutex;
std::condition_variable g_cv;
uint64_t g_generation = 0;

struct End {
    std::shared_ptr<Pipe> pipe;
    bool is_write;
};
std::unordered_map<int, End> g_ends;
int g_next_fd = 100;

}  // namespace

int pipe_create(int fds[2]) {
    auto p = std::make_shared<Pipe>();
    std::lock_guard lk(g_mutex);
    fds[0] = g_next_fd++;
    fds[1] = g_next_fd++;
    g_ends[fds[0]] = End{p, false};
    g_ends[fds[1]] = End{p, true};
    return 0;
}

long pipe_read(int fd, void* buf, size_t n) {
    std::lock_guard lk(g_mutex);
    auto it = g_ends.find(fd);
    if (it == g_ends.end() || it->second.is_write) return -1;
    Pipe& p = *it->second.pipe;
    if (p.data.empty()) return p.write_closed ? 0 : -1;
    size_t k = n < p.data.size() ? n : p.data.size();
    auto* out = static_cast<uint8_t*>(buf);
    for (size_t i = 0; i < k; ++i) {
        out[i] = p.data.front();
        p.data.pop_front();
    }
    return static_cast<long>(k);
}

long pipe_write(int fd, const void* buf, size_t n) {
    {
        std::lock_guard lk(g_mutex);
        auto it = g_ends.find(fd);
        if (it == g_ends.end() || !it->second.is_write) return -1;
        auto* in = static_cast<const uint8_t*>(buf);
        it->second.pipe->data.insert(it->second.pipe->data.end(), in, in + n);
        ++g_generation;
    }
    g_cv.notify_all();
    return static_cast<long>(n);
}

bool pipe_close(int fd) {
    {
        std::lock_guard lk(g_mutex);
        auto it = g_ends.find(fd);
        if (it == g_ends.end()) return false;
        if (it->second.is_write) it->second.pipe->write_closed = true;
        g_ends.erase(it);
        ++g_generation;
    }
    g_cv.notify_all();
    return true;
}

bool is_pipe(int fd) {
    std::lock_guard lk(g_mutex);
    return g_ends.count(fd) != 0;
}

bool pipe_readable(int fd) {
    std::lock_guard lk(g_mutex);
    auto it = g_ends.find(fd);
    if (it == g_ends.end() || it->second.is_write) return false;
    return !it->second.pipe->data.empty();
}

void notify() {
    {
        std::lock_guard lk(g_mutex);
        ++g_generation;
    }
    g_cv.notify_all();
}

uint64_t generation() {
    std::lock_guard lk(g_mutex);
    return g_generation;
}

void wait(uint64_t seen, int timeout_ms) {
    std::unique_lock lk(g_mutex);
    auto pred = [&] { return g_generation != seen; };
    if (timeout_ms < 0) {
        g_cv.wait(lk, pred);
    } else {
        g_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), pred);
    }
}

}  // namespace zb::hostio
