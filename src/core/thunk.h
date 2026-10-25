// AAPCS (ARM Architecture Procedure Call Standard, softfp, ILP32) Thunk Gateway
// Enables type-safe dispatch between ARM32 guest execution and Win32 host functions.
#pragma once

#include <tuple>
#include <type_traits>
#include <cstring>
#include "cpu.h"

namespace zb {

class ArgReader {
public:
    explicit ArgReader(JitCtx& j) : j_(j), nsaa_(j.sp()) {}

    // Read 32-bit argument word from core register (r0-r3) or stack
    u32 w32() {
        if (ncrn_ < 4) return j_.r()[ncrn_++];
        u32 v = rd<u32>(nsaa_);
        nsaa_ += 4;
        return v;
    }

    // Read 64-bit argument word pair (aligned to even register or 8-byte stack slot)
    u64 w64() {
        if (ncrn_ & 1) ncrn_++; // Even register alignment for 64-bit types
        if (ncrn_ < 4) {
            u64 v = static_cast<u64>(j_.r()[ncrn_]) | (static_cast<u64>(j_.r()[ncrn_ + 1]) << 32);
            ncrn_ += 2;
            return v;
        }
        nsaa_ = (nsaa_ + 7) & ~7u; // 8-byte alignment on stack
        u64 v = rd<u64>(nsaa_);
        nsaa_ += 8;
        return v;
    }

    // Address of the first variadic argument, following AAPCS va_start convention:
    // Spills r0-r3 onto the stack frame below the stack arguments.
    u32 va_list_here() {
        u32 sp = j_.sp();
        u32 save = sp - 16;
        for (int i = 0; i < 4; ++i) wr<u32>(save + 4 * i, j_.r()[i]);
        if (ncrn_ < 4) return save + 4 * ncrn_;
        return nsaa_;
    }

private:
    JitCtx& j_;
    unsigned ncrn_ = 0;
    u32 nsaa_;
};

template <typename T>
struct is_gp : std::false_type {};
template <typename T>
struct is_gp<gp<T>> : std::true_type {};

template <typename T>
T fetch_arg(ArgReader& r) {
    if constexpr (is_gp<T>::value) {
        return T{r.w32()};
    } else if constexpr (std::is_same_v<T, float>) {
        u32 b = r.w32();
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    } else if constexpr (std::is_same_v<T, double>) {
        u64 b = r.w64();
        double d;
        std::memcpy(&d, &b, 8);
        return d;
    } else if constexpr (std::is_integral_v<T> && sizeof(T) == 8) {
        return static_cast<T>(r.w64());
    } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
        return static_cast<T>(r.w32());
    } else {
        static_assert(sizeof(T) == 0, "unsupported thunk argument type");
    }
}

template <typename T>
void store_ret(JitCtx& j, T v) {
    if constexpr (is_gp<T>::value) {
        j.set_ret(v.a);
    } else if constexpr (std::is_same_v<T, float>) {
        u32 b;
        std::memcpy(&b, &v, 4);
        j.set_ret(b);
    } else if constexpr (std::is_same_v<T, double>) {
        u64 b;
        std::memcpy(&b, &v, 8);
        j.set_ret64(b);
    } else if constexpr (std::is_integral_v<T> && sizeof(T) == 8) {
        j.set_ret64(static_cast<u64>(v));
    } else if constexpr (std::is_same_v<T, bool>) {
        j.set_ret(v ? 1 : 0);
    } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
        j.set_ret(static_cast<u32>(static_cast<std::conditional_t<std::is_signed_v<T>, i32, u32>>(v)));
    } else {
        static_assert(sizeof(T) == 0, "unsupported thunk return type");
    }
}

template <typename F>
struct fn_traits;
template <typename R, typename... A>
struct fn_traits<R (*)(A...)> {
    using ret = R;
    using args = std::tuple<A...>;
    static constexpr bool wants_ctx = false;
};
template <typename R, typename... A>
struct fn_traits<R (*)(JitCtx&, A...)> {
    using ret = R;
    using args = std::tuple<A...>;
    static constexpr bool wants_ctx = true;
};

template <typename Tuple>
struct fetch_all;
template <typename... A>
struct fetch_all<std::tuple<A...>> {
    static std::tuple<A...> get(ArgReader& r) { return std::tuple<A...>{fetch_arg<A>(r)...}; }
};

template <auto Fn>
void adapt(JitCtx& j) {
    using T = fn_traits<decltype(Fn)>;
    ArgReader reader(j);
    auto args = fetch_all<typename T::args>::get(reader);
    if constexpr (T::wants_ctx) {
        if constexpr (std::is_void_v<typename T::ret>) {
            std::apply([&](auto&&... a) { Fn(j, a...); }, args);
        } else {
            store_ret(j, std::apply([&](auto&&... a) { return Fn(j, a...); }, args));
        }
    } else {
        if constexpr (std::is_void_v<typename T::ret>) {
            std::apply(Fn, args);
        } else {
            store_ret(j, std::apply(Fn, args));
        }
    }
}

} // namespace zb

// Macros for registering AAPCS host thunks
#define REG(sym, fn) ::zb::cpu::register_thunk(#sym, &::zb::adapt<fn>)
#define REG_AS(name, fn) ::zb::cpu::register_thunk(name, &::zb::adapt<fn>)
#define REG_RAW(sym, fn) ::zb::cpu::register_thunk(#sym, fn)
