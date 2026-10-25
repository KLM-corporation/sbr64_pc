// Bionic libc, libm, libstdc++, libdl, libz, and liblog High-Level Emulation for Windows
#pragma once

#include <cstdio>
#include <string>
#include "core/common.h"
#include "core/cpu.h"

namespace zb::bionic {

// Guest ILP32 scalar types
using gsize  = u32;
using gssize = i32;
using glong  = i32;
using gulong = u32;
using goff   = i32;
using gtime  = i32;
using gcstr  = gp<const char>;
using gstr   = gp<char>;

// Guest struct timespec (32-bit fields)
struct GTimespec {
    i32 tv_sec;
    i32 tv_nsec;
};

// Guest struct timeval (32-bit fields)
struct GTimeval {
    i32 tv_sec;
    i32 tv_usec;
};

// Subsystem thunk registration functions
void register_all();
void register_libc();
void register_math();
void register_cxx();
void register_stdio();
void register_pthread();
void register_posix();
void register_time();
void register_dl();
void register_log();
void register_zlib();
void register_all_symbols_stubs(); // Ensures every one of the 240 symbols is bound

// Formatted I/O processing guest va_list (address of next variadic argument in memory)
std::string vformat(const char* fmt, u32 ap, int* consumed_ap = nullptr);
std::wstring vwformat(const wchar_t* fmt, u32 ap);
int vscan_string(const char* input, const char* fmt, u32 ap);
int vscan_file(FILE* f, const char* fmt, u32 ap);
i32 copy_out(gaddr buf, u32 cap, const std::string& s);

// Memory & Strings
gaddr hle_malloc(gsize n);
gaddr hle_calloc(gsize n, gsize s);
gaddr hle_realloc(gaddr p, gsize n);
void  hle_free(gaddr p);
gsize hle_strlen(gcstr s);
i32   hle_strcmp(gcstr a, gcstr b);
i32   hle_strncmp(gcstr a, gcstr b, gsize n);
gaddr hle_strcpy(gstr d, gcstr s);
gaddr hle_strncpy(gstr d, gcstr s, gsize n);
gaddr hle_strcat(gstr d, gcstr s);
gaddr hle_memset(gaddr d, i32 c, gsize n);
gaddr hle_memcpy(gaddr d, gaddr s, gsize n);
gaddr hle_memmove(gaddr d, gaddr s, gsize n);
i32   hle_memcmp(gaddr a, gaddr b, gsize n);

// Math
float m_sinf(float x);
float m_cosf(float x);
float m_tanf(float x);
float m_atan2f(float y, float x);
float m_sqrtf(float x);
float m_powf(float x, float y);
float m_floorf(float x);
float m_ceilf(float x);
float m_log10f(float x);
double m_sin(double x);

// C++ Mangled & EABI
gaddr cxx_new(gsize n);
void  cxx_delete(gaddr p);
i32   cxa_guard_acquire(gp<u32> g);
void  cxa_guard_release(gp<u32> g);
void  aeabi_memset(gaddr d, gsize n, i32 c);
void  aeabi_memcpy(gaddr d, gaddr s, gsize n);
void  aeabi_memmove(gaddr d, gaddr s, gsize n);

// Stdio
gaddr s_fopen(JitCtx& j, gcstr path, gcstr mode);
i32   s_fclose(JitCtx& j, gaddr gf);
gsize s_fread(JitCtx& j, gaddr buf, gsize size, gsize n, gaddr gf);
gsize s_fwrite(JitCtx& j, gaddr buf, gsize size, gsize n, gaddr gf);
glong s_ftell(JitCtx& j, gaddr gf);
i32   s_fseek(JitCtx& j, gaddr gf, glong off, i32 whence);
i32   s_fflush(JitCtx& j, gaddr gf);

// Pthreads & Mutex
int   mutex_lock(GuestThread* t, gaddr m);
int   mutex_trylock(GuestThread* t, gaddr m);
int   mutex_unlock(GuestThread* t, gaddr m);
i32   p_mutex_init(gaddr m, gaddr attr);
i32   p_mutex_destroy(gaddr m);

// Guest FILE* <-> Host FILE* mapping table
FILE* host_file(gaddr gf);
gaddr guest_file(FILE* f);
void forget_file(gaddr gf);
gaddr get_sF(); // Returns base address of __sF array (stdin, stdout, stderr)

// Deduplicated persistent static strings in guest heap
gaddr static_string(const std::string& s);

// Per-thread guest scratch buffer at specified TLS offset
gaddr tls_buffer(JitCtx& j, u32 off);

// Synchronizes host errno to guest thread TLS slot
inline void sync_errno(JitCtx& j) {
    j.set_errno(errno);
}

// Writes a host pointer result into a guest char** style slot
inline void write_ptr(gaddr slot, const void* p) {
    if (slot) wr<u32>(slot, ga(p));
}

// VFS Path Translation for Windows host
void set_save_dir(const std::string& path);
const std::string& get_save_dir();
void set_obb_dir(const std::string& path);
const std::string& get_obb_dir();
std::string translate_path(const char* guest_path);

} // namespace zb::bionic
