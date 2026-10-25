#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <string>
#include <vector>

#include "common.h"

namespace Dynarmic::A32 {
class Jit;
}

namespace zb {

struct JitCtx;
struct GuestThread;

// Thunk handler: reads guest arguments from the context, writes the result.
using ThunkFn = void (*)(JitCtx&);

namespace cpu {

using zb::u8;
using zb::u16;
using zb::u32;
using zb::u64;
using zb::i8;
using zb::i16;
using zb::i32;
using zb::i64;
using zb::gaddr;

constexpr u32 kSvcReturn    = 0x00F00001;  // "return to host" trap
constexpr u32 kSvcThunkBase = 0x00100000;
constexpr u32 kSvcHookBase  = 0x00000080;

struct Config {
    bool fastmem = true;
    bool fast_dispatch = true;
    u32 stack_size = 8 * 1024 * 1024;
};

void init(const Config& cfg = {});

// The current thread's guest state (created on first use).
GuestThread* current_thread();
GuestThread* current_thread_if_any();
GuestThread* new_thread_state(u32 stack_size);
void adopt_thread(GuestThread* t);

// Builds AAPCS argument words for a guest call.
class Args {
public:
    Args& u32v(u32 v) {
        w_.push_back(v);
        return *this;
    }
    Args& i32v(i32 v) { return u32v(static_cast<u32>(v)); }
    Args& u64v(u64 v) {
        if (w_.size() < 4 && (w_.size() & 1)) w_.push_back(0);  // even register pair
        if (w_.size() >= 4 && ((w_.size() - 4) & 1)) w_.push_back(0);  // 8-byte aligned stack slot
        w_.push_back(static_cast<u32>(v));
        w_.push_back(static_cast<u32>(v >> 32));
        return *this;
    }
    Args& f32v(float f) {
        u32 b;
        std::memcpy(&b, &f, 4);
        return u32v(b);
    }
    Args& f64v(double d) {
        u64 b;
        std::memcpy(&b, &d, 8);
        return u64v(b);
    }
    const std::vector<u32>& words() const { return w_; }

private:
    std::vector<u32> w_;
};

// Calls guest function `fn` (bit 0 selects Thumb). Returns r0 | r1 << 32.
u64 call(gaddr fn, const u32* words, size_t nwords);
inline u64 call(gaddr fn, const Args& a) { return call(fn, a.words().data(), a.words().size()); }
inline u64 call(gaddr fn) { return call(fn, nullptr, 0); }

// Thunks.
gaddr register_thunk(std::string_view name, ThunkFn fn);
gaddr register_thunk(u32 svc_id, ThunkFn fn);
gaddr make_thunk(ThunkFn fn, std::string_view debug_name);
gaddr find_thunk(std::string_view name);
void register_data(std::string_view name, gaddr addr);
gaddr find_data(std::string_view name);
gaddr missing_import_stub(std::string_view name);
const char* thunk_name_for_addr(gaddr a);

// Invalidate translated code in every JIT
void invalidate_code(gaddr addr, u32 len);

// Hooks
bool install_thumb_entry_hook(gaddr fn, ThunkFn hook, const char* name);
bool install_thumb_replace_hook(gaddr fn, ThunkFn hook, const char* name);
void continue_at(JitCtx& j, gaddr target);
void skip_function(JitCtx& j);

void dump_state(JitCtx& j, const char* why);
void dump_all_threads(const char* why);

void log_call_stats(double seconds);
// --verbose: also report which guest functions call these thunks (comma-separated names).
void trace_callers(std::string_view names);
void set_code_range(gaddr lo, gaddr hi);  // game code, for the heuristic backtraces of trace_callers
void profile_dump(const char* path);
void dump_own_map_if_requested(GuestThread* t);
void note_wait(const char* what, int64_t ns);

using Symbolizer = std::string (*)(gaddr);
void set_symbolizer(Symbolizer s);
std::string symbolize(gaddr a);

void set_syscall_handler(ThunkFn fn);

using ThreadExitHook = void (*)(GuestThread*);
void add_thread_exit_hook(ThreadExitHook h);
void run_thread_exit_hooks(GuestThread* t);
void add_thread_destroy_hook(ThreadExitHook h);

}  // namespace cpu

struct GuestThread {
    u32 id = 0;
    gaddr tls = 0;
    gaddr stack_lo = 0;
    gaddr stack_hi = 0;
    u32 stack_size = 0;
    std::vector<std::unique_ptr<JitCtx>> pool;
    int depth = 0;
    bool exiting = false;
    u32 exit_value = 0;
    std::array<u32, 128> keys{};
    void* host_env = nullptr;
    bool jni_attached_by_us = false;
    gaddr guest_env = 0;
    bool owns_stack = true;
    u32 dump_seen = 0;

    JitCtx* top();
};

namespace tls {
constexpr u32 kErrno = 0x000;
constexpr u32 kSelf = 0x004;
constexpr u32 kJniEnv = 0x008;
constexpr u32 kTm = 0x010;
constexpr u32 kStrerror = 0x040;
constexpr u32 kNtoa = 0x140;
constexpr u32 kStrsignal = 0x160;
constexpr u32 kPasswd = 0x1A0;
constexpr u32 kScratch = 0x400;
constexpr u32 kSize = 0x1000;
}  // namespace tls

struct JitCtx {
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    GuestThread* thread = nullptr;
    u32 proc_id = 0;
    bool returned = false;
    gaddr cur_thunk = 0;
    bool skip_function = false;
    u32 tpidruro = 0;
    u32 tpidrurw = 0;

    std::array<u32, 16>& r();
    u32 sp();

    u32 arg(unsigned i);
    void set_ret(u32 v);
    void set_ret64(u64 v);
    void set_errno(int e);
    int guest_errno();
};

namespace cpu {
using zb::GuestThread;
using zb::JitCtx;
}

}  // namespace zb
