// EGL and OpenGL ES 2.0 for the guest, on top of the desktop OpenGL (WGL) of
// the host. GLES2 entry points map 1:1 onto the same-named compatibility
// profile functions; the few ES-only behaviours (shader dialect, precision
// qualifiers, ES-only queries, ETC1 textures, VAO/discard extensions) are
// translated here. Guest pointers are host pointers (identity mapping).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "android/bionic_hle.h"
#include "core/cpu.h"
#include "core/mem_win32.h"
#include "core/thunk.h"
#include "platform/win_window.h"

#ifndef APIENTRY
#define APIENTRY __stdcall
#endif

namespace zb::android {
namespace {

// ------------------------------------------------------------ GL loader

void* glproc(const char* name) {
    void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
    auto v = reinterpret_cast<intptr_t>(p);
    if (!p || v == 1 || v == 2 || v == 3 || v == -1) {
        static HMODULE lib = LoadLibraryA("opengl32.dll");
        p = reinterpret_cast<void*>(GetProcAddress(lib, name));
    }
    return p;
}

template <class T>
struct HostT {
    using type = T;
};
template <class T>
struct HostT<gp<T>> {
    using type = T*;
};

template <class Sig>
struct Gen;
template <class R, class... A>
struct Gen<R(A...)> {
    static void run(JitCtx& j, void* fp) {
        ArgReader r(j);
        std::tuple<A...> args{fetch_arg<A>(r)...};
        using F = R(APIENTRY*)(typename HostT<A>::type...);
        F f = reinterpret_cast<F>(fp);
        if constexpr (std::is_void_v<R>) {
            std::apply([&](auto&&... a) { f(a...); }, args);
        } else {
            store_ret(j, std::apply([&](auto&&... a) { return f(a...); }, args));
        }
    }
};

// --verbose diagnostics: counts draw calls and reports each distinct GL error once. The error check
// consumes the error flag, so the guest's own glGetError sees fewer errors than it would normally.
std::atomic<u64> g_draw_calls{0};

void note_gl_call(const char* name) {
    if (!std::strcmp(name, "glDrawElements") || !std::strcmp(name, "glDrawArrays")) ++g_draw_calls;
    static auto get_error = reinterpret_cast<u32(APIENTRY*)()>(glproc("glGetError"));
    if (!get_error) return;
    const u32 e = get_error();
    if (!e) return;
    static std::mutex m;
    static std::unordered_map<std::string, int> seen;
    char key[96];
    std::snprintf(key, sizeof key, "%s: GL error 0x%x", name, e);
    std::lock_guard lk(m);
    if (++seen[key] == 1) ZLOGW("%s (first occurrence)", key);
}

// Registers a guest thunk `gname` that forwards to host GL function `hname`.
#define GLN(gname, hname, R, ...)                                                      \
    {                                                                                  \
        struct S {                                                                     \
            static void run(JitCtx& j) {                                               \
                if (!wglGetCurrentContext()) {                                         \
                    if constexpr (!std::is_void_v<R>) j.set_ret(0);                    \
                    return;                                                            \
                }                                                                      \
                static void* fp = nullptr;                                             \
                if (!fp) {                                                             \
                    fp = glproc(hname);                                                \
                    if (!fp) {                                                         \
                        ZLOGW("GL: %s is not available on this driver", hname);        \
                        if constexpr (!std::is_void_v<R>) j.set_ret(0);                \
                        return;                                                        \
                    }                                                                  \
                }                                                                      \
                Gen<R(__VA_ARGS__)>::run(j, fp);                                       \
                if (::zb::g_debug) note_gl_call(gname);                                \
            }                                                                          \
        };                                                                             \
        cpu::register_thunk(gname, &S::run);                                           \
    }
#define GL(n, R, ...) GLN("gl" #n, "gl" #n, R, __VA_ARGS__)

using cstr = gp<const char>;
using cvoid = gp<const void>;

// ------------------------------------------------------------ constants used below
constexpr u32 GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02;
constexpr u32 GL_MAX_VERTEX_UNIFORM_VECTORS = 0x8DFB, GL_MAX_VARYING_VECTORS = 0x8DFC,
              GL_MAX_FRAGMENT_UNIFORM_VECTORS = 0x8DFD;
constexpr u32 GL_MAX_VERTEX_UNIFORM_COMPONENTS = 0x8B4A, GL_MAX_FRAGMENT_UNIFORM_COMPONENTS = 0x8B49,
              GL_MAX_VARYING_FLOATS = 0x8B4B;
constexpr u32 GL_ETC1_RGB8_OES = 0x8D64, GL_COMPRESSED_RGB8_ETC2 = 0x9274;

// ------------------------------------------------------------ special GL thunks

using PFN_BufferData = void(APIENTRY*)(u32, intptr_t, const void*, u32);
using PFN_BufferSubData = void(APIENTRY*)(u32, intptr_t, intptr_t, const void*);

void g_BufferData(u32 target, i32 size, gaddr data, u32 usage) {
    static auto f = reinterpret_cast<PFN_BufferData>(glproc("glBufferData"));
    if (f) f(target, size, hp(data), usage);
}
void g_BufferSubData(u32 target, i32 off, i32 size, gaddr data) {
    static auto f = reinterpret_cast<PFN_BufferSubData>(glproc("glBufferSubData"));
    if (f) f(target, off, size, hp(data));
}

void g_ClearDepthf(float d) {
    static auto f = reinterpret_cast<void(APIENTRY*)(double)>(glproc("glClearDepth"));
    if (f) f(d);
}
void g_DepthRangef(float n, float fa) {
    static auto f = reinterpret_cast<void(APIENTRY*)(double, double)>(glproc("glDepthRange"));
    if (f) f(n, fa);
}

// GLSL ES 1.00 -> desktop GLSL 1.20: drop precision statements, neutralise precision qualifiers.
std::string translate_shader(const std::string& src) {
    std::string out = "#version 120\n#define lowp\n#define mediump\n#define highp\n";
    std::istringstream in(src);
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        size_t i = line.find_first_not_of(" \t\r");
        if (i != std::string::npos && line.compare(i, 9, "precision") == 0 &&
            (line.size() <= i + 9 || line[i + 9] == ' ' || line[i + 9] == '\t'))
            line.clear();
        if (i != std::string::npos && line.compare(0, std::min<size_t>(line.size(), 8), "#version") == 0 && first)
            line.clear();
        first = false;
        out += line;
        out += '\n';
    }
    return out;
}

void g_ShaderSource(u32 shader, i32 count, gaddr strings, gaddr lengths) {
    static auto f = reinterpret_cast<void(APIENTRY*)(u32, i32, const char* const*, const i32*)>(glproc("glShaderSource"));
    if (!f) return;
    std::string src;
    for (i32 i = 0; i < count; ++i) {
        const char* s = hp<const char>(rd<u32>(strings + 4 * i));
        i32 len = lengths ? rd<i32>(lengths + 4 * i) : -1;
        if (!s) continue;
        if (len < 0) src += s;
        else src.append(s, static_cast<size_t>(len));
    }
    std::string t = translate_shader(src);
    const char* p = t.c_str();
    f(shader, 1, &p, nullptr);
}

gaddr g_GetString(u32 name) {
    static auto f = reinterpret_cast<const unsigned char*(APIENTRY*)(u32)>(glproc("glGetString"));
    if (!f) return 0;
    const char* s = reinterpret_cast<const char*>(f(name));
    if (!s) return 0;
    if (name == GL_RENDERER || name == GL_VERSION) ZLOGI("glGetString(0x%x) = %s", name, s);
    if (name == GL_VERSION) return bionic::static_string(std::string("OpenGL ES 2.0 (") + s + ")");
    return bionic::static_string(s);
}

u32 current_target_fbo();  // scaled presentation, below

void g_GetIntegerv(u32 pname, gaddr out) {
    static auto f = reinterpret_cast<void(APIENTRY*)(u32, i32*)>(glproc("glGetIntegerv"));
    if (!f) return;
    i32 v = 0;
    switch (pname) {
        case GL_MAX_VERTEX_UNIFORM_VECTORS:
            f(GL_MAX_VERTEX_UNIFORM_COMPONENTS, &v);
            wr<i32>(out, v / 4);
            return;
        case GL_MAX_FRAGMENT_UNIFORM_VECTORS:
            f(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, &v);
            wr<i32>(out, v / 4);
            return;
        case GL_MAX_VARYING_VECTORS:
            f(GL_MAX_VARYING_FLOATS, &v);
            wr<i32>(out, v / 4);
            return;
        case 0x8CA6:  // GL_FRAMEBUFFER_BINDING: the stand-in for the window surface reads as 0
            f(pname, &v);
            if (v != 0 && static_cast<u32>(v) == current_target_fbo()) v = 0;
            wr<i32>(out, v);
            return;
        default: f(pname, hp<i32>(out)); return;
    }
}

void g_CompressedTexImage2D(u32 target, i32 level, u32 fmt, i32 w, i32 h, i32 border, i32 size, gaddr data) {
    static auto f = reinterpret_cast<void(APIENTRY*)(u32, i32, u32, i32, i32, i32, i32, const void*)>(
        glproc("glCompressedTexImage2D"));
    static std::mutex m;
    static std::vector<u32> seen;
    {
        std::lock_guard lk(m);
        if (std::find(seen.begin(), seen.end(), fmt) == seen.end()) {
            seen.push_back(fmt);
            ZLOGI("glCompressedTexImage2D: texture format 0x%x (%dx%d)", fmt, w, h);
        }
    }
    if (fmt == GL_ETC1_RGB8_OES) fmt = GL_COMPRESSED_RGB8_ETC2;  // ETC1 is a subset of ETC2
    if (f) f(target, level, fmt, w, h, border, size, hp(data));
}
void g_CompressedTexSubImage2D(u32 target, i32 level, i32 x, i32 y, i32 w, i32 h, u32 fmt, i32 size, gaddr data) {
    static auto f = reinterpret_cast<void(APIENTRY*)(u32, i32, i32, i32, i32, i32, u32, i32, const void*)>(
        glproc("glCompressedTexSubImage2D"));
    if (fmt == GL_ETC1_RGB8_OES) fmt = GL_COMPRESSED_RGB8_ETC2;
    if (f) f(target, level, x, y, w, h, fmt, size, hp(data));
}

void x_Noop() {}

// ------------------------------------------------------------ EGL

constexpr u32 kDisplay = 0x21000001, kConfig = 0x22000001, kWindowSurface = 0x24000001;
std::mutex g_egl_mutex;
std::unordered_map<u32, HGLRC> g_contexts;
u32 g_next_context = 0x23000001;
u32 g_next_pbuffer = 0x24100001;
std::atomic<i32> g_egl_error{0x3000};
std::atomic<bool> g_swap_interval_set{false};
int g_vsync_mode = -1;  // -1 auto (only when the display runs at a multiple of 60 Hz), 0 off, 1 on

// The game steps its logic once per presented frame at 60 Hz: on a 48/75/144 Hz panel a locked
// vsync would run it too slowly or too fast, so vsync is only used when it divides evenly.
int display_refresh_hz() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(win::hwnd()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXA mi{};
    mi.cbSize = sizeof mi;
    DEVMODEA dm{};
    dm.dmSize = sizeof dm;
    if (GetMonitorInfoA(mon, &mi) && EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        return static_cast<int>(dm.dmDisplayFrequency);
    return 0;
}

// ------------------------------------------------------------ scaled presentation
//
// The game renders at a fixed size (the window's size at start-up, which ANativeWindow and EGL report).
// Its "default framebuffer" is an offscreen FBO of that size, scaled into the window on every
// eglSwapBuffers with the aspect ratio kept: maximising or going full screen fills the screen instead of
// leaving the game in a corner of a black window.

constexpr u32 GL_FRAMEBUFFER = 0x8D40, GL_RENDERBUFFER = 0x8D41, GL_READ_FRAMEBUFFER = 0x8CA8,
              GL_DRAW_FRAMEBUFFER = 0x8CA9, GL_FRAMEBUFFER_BINDING = 0x8CA6, GL_COLOR_ATTACHMENT0 = 0x8CE0,
              GL_DEPTH_STENCIL_ATTACHMENT = 0x821A, GL_RGBA8 = 0x8058, GL_DEPTH24_STENCIL8 = 0x88F0,
              GL_FRAMEBUFFER_COMPLETE = 0x8CD5, GL_COLOR_BUFFER_BIT = 0x4000, GL_LINEAR = 0x2601,
              GL_SCISSOR_TEST = 0x0C11, GL_COLOR_CLEAR_VALUE = 0x0C22, GL_COLOR_WRITEMASK = 0x0C23,
              GL_PACK_ALIGNMENT = 0x0D05;

struct PresentFns {
    void(APIENTRY* GenFramebuffers)(i32, u32*);
    void(APIENTRY* BindFramebuffer)(u32, u32);
    void(APIENTRY* GenRenderbuffers)(i32, u32*);
    void(APIENTRY* BindRenderbuffer)(u32, u32);
    void(APIENTRY* RenderbufferStorage)(u32, u32, i32, i32);
    void(APIENTRY* FramebufferRenderbuffer)(u32, u32, u32, u32);
    u32(APIENTRY* CheckFramebufferStatus)(u32);
    void(APIENTRY* BlitFramebuffer)(i32, i32, i32, i32, i32, i32, i32, i32, u32, u32);
    void(APIENTRY* GetIntegerv)(u32, i32*);
    void(APIENTRY* GetFloatv)(u32, float*);
    void(APIENTRY* GetBooleanv)(u32, u8*);
    u8(APIENTRY* IsEnabled)(u32);
    void(APIENTRY* Enable)(u32);
    void(APIENTRY* Disable)(u32);
    void(APIENTRY* ClearColor)(float, float, float, float);
    void(APIENTRY* ColorMask)(u8, u8, u8, u8);
    void(APIENTRY* Clear)(u32);
    void(APIENTRY* PixelStorei)(u32, i32);
    void(APIENTRY* ReadPixels)(i32, i32, i32, i32, u32, u32, void*);
    bool ok = false;
};

template <class F>
bool load_gl(F& f, const char* name) {
    f = reinterpret_cast<F>(glproc(name));
    return f != nullptr;
}

// Loaded on first use, with a context current.
const PresentFns& present_fns() {
    static const PresentFns fns = [] {
        PresentFns p{};
        p.ok = load_gl(p.GenFramebuffers, "glGenFramebuffers") & load_gl(p.BindFramebuffer, "glBindFramebuffer") &
               load_gl(p.GenRenderbuffers, "glGenRenderbuffers") & load_gl(p.BindRenderbuffer, "glBindRenderbuffer") &
               load_gl(p.RenderbufferStorage, "glRenderbufferStorage") &
               load_gl(p.FramebufferRenderbuffer, "glFramebufferRenderbuffer") &
               load_gl(p.CheckFramebufferStatus, "glCheckFramebufferStatus") &
               load_gl(p.BlitFramebuffer, "glBlitFramebuffer") & load_gl(p.GetIntegerv, "glGetIntegerv") &
               load_gl(p.GetFloatv, "glGetFloatv") & load_gl(p.GetBooleanv, "glGetBooleanv") &
               load_gl(p.IsEnabled, "glIsEnabled") & load_gl(p.Enable, "glEnable") & load_gl(p.Disable, "glDisable") &
               load_gl(p.ClearColor, "glClearColor") & load_gl(p.ColorMask, "glColorMask") &
               load_gl(p.Clear, "glClear") & load_gl(p.PixelStorei, "glPixelStorei") &
               load_gl(p.ReadPixels, "glReadPixels");
        if (!p.ok) ZLOGW("present: framebuffer objects unavailable, the game draws straight into the window");
        return p;
    }();
    return fns;
}

// The FBO standing in for the window surface of one GL context (FBOs are not shared between contexts).
struct Target {
    u32 fbo = 0, color = 0, depth = 0;
    int w = 0, h = 0;
};
std::unordered_map<HGLRC, Target> g_targets;  // guarded by g_egl_mutex
thread_local Target* t_target = nullptr;      // target of the context current on this thread

u32 current_target_fbo() { return t_target ? t_target->fbo : 0; }

// Called with `rc` current. A fresh context has framebuffer 0 bound, which now means the target.
Target* target_for(HGLRC rc) {
    const PresentFns& f = present_fns();
    if (!f.ok) return nullptr;
    std::lock_guard lk(g_egl_mutex);
    auto [it, fresh] = g_targets.try_emplace(rc);
    Target& t = it->second;
    if (fresh) {
        t.w = win::width();
        t.h = win::height();
        f.GenRenderbuffers(1, &t.color);
        f.BindRenderbuffer(GL_RENDERBUFFER, t.color);
        f.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, t.w, t.h);
        f.GenRenderbuffers(1, &t.depth);
        f.BindRenderbuffer(GL_RENDERBUFFER, t.depth);
        f.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, t.w, t.h);
        f.BindRenderbuffer(GL_RENDERBUFFER, 0);
        f.GenFramebuffers(1, &t.fbo);
        f.BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        f.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t.color);
        f.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth);
        if (f.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            ZLOGW("present: offscreen framebuffer incomplete, the game draws straight into the window");
            f.BindFramebuffer(GL_FRAMEBUFFER, 0);
            t.fbo = 0;
        } else {
            ZLOGI("present: game renders at %dx%d, scaled into the window", t.w, t.h);
        }
    }
    return t.fbo ? &t : nullptr;
}

// Scales the target into the window's back buffer; black bars where the aspect ratios differ. The GL state
// it touches is restored by restore_after_present().
struct SavedState {
    i32 fbo = 0;
    u8 scissor = 0;
    float clear[4] = {};
    u8 mask[4] = {};
};

SavedState present(const Target& t) {
    const PresentFns& f = present_fns();
    SavedState s;
    f.GetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    s.scissor = f.IsEnabled(GL_SCISSOR_TEST);
    f.GetFloatv(GL_COLOR_CLEAR_VALUE, s.clear);
    f.GetBooleanv(GL_COLOR_WRITEMASK, s.mask);

    if (s.scissor) f.Disable(GL_SCISSOR_TEST);
    f.ColorMask(1, 1, 1, 1);
    f.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    f.ClearColor(0, 0, 0, 1);
    f.Clear(GL_COLOR_BUFFER_BIT);
    const win::Rect r = win::game_rect();
    if (r.w > 0 && r.h > 0) {
        f.BindFramebuffer(GL_READ_FRAMEBUFFER, t.fbo);
        const int y0 = win::client_height() - r.y - r.h;  // GL's origin is the bottom-left corner
        f.BlitFramebuffer(0, 0, t.w, t.h, r.x, y0, r.x + r.w, y0 + r.h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    f.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    return s;
}

void restore_after_present(const SavedState& s) {
    const PresentFns& f = present_fns();
    f.BindFramebuffer(GL_FRAMEBUFFER, static_cast<u32>(s.fbo));
    f.ColorMask(s.mask[0], s.mask[1], s.mask[2], s.mask[3]);
    f.ClearColor(s.clear[0], s.clear[1], s.clear[2], s.clear[3]);
    if (s.scissor) f.Enable(GL_SCISSOR_TEST);
}

void g_BindFramebuffer(u32 target, u32 fbo) {
    if (!wglGetCurrentContext()) return;
    if (fbo == 0 && t_target) fbo = t_target->fbo;
    if (auto bind = present_fns().BindFramebuffer) bind(target, fbo);
}

u32 e_GetDisplay(u32) { return kDisplay; }
u32 e_Initialize(u32, gp<i32> major, gp<i32> minor) {
    if (major.a) *major.get() = 1;
    if (minor.a) *minor.get() = 4;
    return 1;
}
u32 e_Terminate(u32) { return 1; }
u32 e_ChooseConfig(u32, gaddr, gaddr configs, i32 size, gp<i32> num) {
    if (configs && size > 0) wr<u32>(configs, kConfig);
    if (num.a) *num.get() = 1;
    return 1;
}
u32 e_GetConfigs(u32, gaddr configs, i32 size, gp<i32> num) { return e_ChooseConfig(0, 0, configs, size, num); }
u32 e_GetConfigAttrib(u32, u32, i32 attr, gp<i32> value) {
    i32 v = 0;
    switch (attr) {
        case 0x3020: v = 32; break;                 // EGL_BUFFER_SIZE
        case 0x3021: case 0x3022: case 0x3023: case 0x3024: v = 8; break;  // ALPHA/BLUE/GREEN/RED
        case 0x3025: v = 24; break;                 // EGL_DEPTH_SIZE
        case 0x3026: v = 8; break;                  // EGL_STENCIL_SIZE
        case 0x3027: v = 0x3038; break;             // EGL_CONFIG_CAVEAT = EGL_NONE
        case 0x3028: v = 1; break;                  // EGL_CONFIG_ID
        case 0x302E: v = 1; break;                  // EGL_NATIVE_VISUAL_ID (RGBA_8888)
        case 0x3033: v = 0x5; break;                // EGL_SURFACE_TYPE: WINDOW | PBUFFER
        case 0x3040: v = 0x4; break;                // EGL_RENDERABLE_TYPE: ES2
        case 0x3042: v = 0x4; break;                // EGL_CONFORMANT: ES2
        default: v = 0; break;
    }
    if (value.a) *value.get() = v;
    return 1;
}
u32 e_CreateWindowSurface(u32, u32, u32, gaddr) { return kWindowSurface; }
u32 e_CreatePbufferSurface(u32, u32, gaddr) {
    std::lock_guard lk(g_egl_mutex);
    return g_next_pbuffer++;
}
u32 e_CreateContext(u32, u32, u32 share, gaddr) {
    HDC dc = static_cast<HDC>(win::hdc());
    if (!dc) {
        g_egl_error = 0x3008;  // EGL_BAD_DISPLAY
        return 0;
    }
    HGLRC rc = wglCreateContext(dc);
    if (!rc) {
        ZLOGE("eglCreateContext: wglCreateContext failed (%lu)", GetLastError());
        g_egl_error = 0x3009;  // EGL_BAD_MATCH
        return 0;
    }
    std::lock_guard lk(g_egl_mutex);
    if (share) {
        auto it = g_contexts.find(share);
        if (it != g_contexts.end() && !wglShareLists(it->second, rc)) ZLOGW("wglShareLists failed (%lu)", GetLastError());
    }
    u32 id = g_next_context++;
    g_contexts[id] = rc;
    return id;
}
u32 e_DestroyContext(u32, u32 ctx) {
    std::lock_guard lk(g_egl_mutex);
    auto it = g_contexts.find(ctx);
    if (it == g_contexts.end()) return 0;
    if (wglGetCurrentContext() == it->second) {
        wglMakeCurrent(nullptr, nullptr);
        t_target = nullptr;
    }
    g_targets.erase(it->second);  // its GL objects go with the context
    wglDeleteContext(it->second);
    g_contexts.erase(it);
    return 1;
}
u32 e_DestroySurface(u32, u32) { return 1; }
u32 e_MakeCurrent(u32, u32, u32, u32 ctx) {
    if (!ctx) {
        t_target = nullptr;
        return wglMakeCurrent(nullptr, nullptr) ? 1 : 0;
    }
    HGLRC rc;
    {
        std::lock_guard lk(g_egl_mutex);
        auto it = g_contexts.find(ctx);
        if (it == g_contexts.end()) {
            g_egl_error = 0x3006;  // EGL_BAD_CONTEXT
            return 0;
        }
        rc = it->second;
    }
    if (!wglMakeCurrent(static_cast<HDC>(win::hdc()), rc)) {
        ZLOGE("eglMakeCurrent: wglMakeCurrent failed (%lu)", GetLastError());
        g_egl_error = 0x3002;  // EGL_BAD_ACCESS
        return 0;
    }
    t_target = target_for(rc);
    if (!g_swap_interval_set.exchange(true)) {
        if (auto si = reinterpret_cast<BOOL(APIENTRY*)(int)>(glproc("wglSwapIntervalEXT"))) {
            const int hz = display_refresh_hz();
            const bool vsync = g_vsync_mode > 0 || (g_vsync_mode < 0 && hz > 0 && hz % 60 == 0);
            ZLOGI("display refresh %d Hz: vsync %s (game paced at 60 Hz by the frame limiter)", hz,
                  vsync ? "on" : "off");
            si(vsync ? 1 : 0);
        }
    }
    return 1;
}

// The game steps its logic once per frame and is written for 60 Hz: never present faster than that,
// whatever the refresh rate of the monitor.
void limit_frame_rate() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next{};
    constexpr auto period = std::chrono::nanoseconds(1'000'000'000 / 60);
    auto now = clock::now();
    if (next.time_since_epoch().count() == 0) next = now;
    if (now < next) {
        auto left = next - now;
        if (left > std::chrono::milliseconds(2)) std::this_thread::sleep_for(left - std::chrono::milliseconds(1));
        while (clock::now() < next) std::this_thread::yield();
        now = clock::now();
    }
    next = (now - next > period * 3) ? now + period : next + period;  // never rush after a hitch
}

// Diagnostics: optional back-buffer capture and frame-time statistics (logged with --verbose).
std::string g_shot_path;
std::vector<int> g_shot_frames;
std::atomic<u64> g_frame_count{0};

// With several frames requested, "shot.bmp" becomes "shot_<frame>.bmp".
std::string shot_file(u64 frame) {
    if (g_shot_frames.size() < 2) return g_shot_path;
    const size_t dot = g_shot_path.rfind('.');
    const std::string stem = g_shot_path.substr(0, dot == std::string::npos ? g_shot_path.size() : dot);
    const std::string ext = dot == std::string::npos ? "" : g_shot_path.substr(dot);
    return stem + "_" + std::to_string(frame) + ext;
}

// Reads what is about to be presented: the window's back buffer, at its client size.
void save_screenshot(const std::string& path) {
    const PresentFns& f = present_fns();
    const int w = win::client_width(), h = win::client_height();
    if (!f.ReadPixels || w <= 0 || h <= 0) return;
    constexpr u32 GL_BGR = 0x80E0, GL_UNSIGNED_BYTE = 0x1401;
    const size_t stride = (static_cast<size_t>(w) * 3 + 3) & ~size_t{3};  // rows packed on 4 bytes
    std::vector<u8> px(stride * static_cast<size_t>(h));
    i32 pack = 4;
    f.GetIntegerv(GL_PACK_ALIGNMENT, &pack);
    f.PixelStorei(GL_PACK_ALIGNMENT, 4);
    f.ReadPixels(0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, px.data());
    f.PixelStorei(GL_PACK_ALIGNMENT, pack);

    // GL rows run bottom-up, exactly like a BMP.
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = static_cast<DWORD>(fh.bfOffBits + px.size());
    ih.biSize = sizeof ih;
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biSizeImage = static_cast<DWORD>(px.size());
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(&fh, sizeof fh, 1, f);
        std::fwrite(&ih, sizeof ih, 1, f);
        std::fwrite(px.data(), 1, px.size(), f);
        std::fclose(f);
        ZLOGI("screenshot: frame %llu saved to %s (%dx%d)", static_cast<unsigned long long>(g_frame_count),
              path.c_str(), w, h);
    } else {
        ZLOGW("screenshot: cannot write %s", path.c_str());
    }
}

void frame_stats() {
    using clock = std::chrono::steady_clock;
    static clock::time_point last{};
    static double sum_ms = 0, max_ms = 0;
    static int n = 0;
    const auto now = clock::now();
    if (last.time_since_epoch().count() != 0) {
        const double ms = std::chrono::duration<double, std::milli>(now - last).count();
        sum_ms += ms;
        max_ms = std::max(max_ms, ms);
        if (++n == 120) {
            ZLOGD("frames %llu: %.1f fps average, worst frame %.1f ms, %llu draw calls/frame",
                  static_cast<unsigned long long>(g_frame_count), 1000.0 * n / sum_ms, max_ms,
                  static_cast<unsigned long long>(g_draw_calls.exchange(0) / n));
            sum_ms = max_ms = 0;
            n = 0;
        }
    }
    last = now;
}

u32 e_SwapBuffers(u32, u32 surface) {
    if (surface != kWindowSurface) return 1;  // pbuffers are not presented
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    ++g_frame_count;
    SavedState saved;
    if (t_target) saved = present(*t_target);
    if (!g_shot_frames.empty() &&
        std::find(g_shot_frames.begin(), g_shot_frames.end(), static_cast<int>(g_frame_count)) != g_shot_frames.end())
        save_screenshot(shot_file(g_frame_count));
    if (t_target) restore_after_present(saved);
    const auto t1 = clock::now();
    SwapBuffers(static_cast<HDC>(win::hdc()));
    const auto t2 = clock::now();
    limit_frame_rate();
    frame_stats();
    const auto ms = [](clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
    if (ms(t2 - t0) > 100) {
        ZLOGW("present: frame %llu took %.0f ms (scale %.0f ms, SwapBuffers %.0f ms, client %dx%d)",
              static_cast<unsigned long long>(g_frame_count.load()), ms(t2 - t0), ms(t1 - t0), ms(t2 - t1),
              win::client_width(), win::client_height());
    }
    return 1;
}
u32 e_SwapInterval(u32, i32) { return 1; }
i32 e_GetError() {
    i32 e = g_egl_error.exchange(0x3000);
    return e;
}
u32 e_QuerySurface(u32, u32, i32 attr, gp<i32> v) {
    if (!v.a) return 0;
    switch (attr) {
        case 0x3057: *v.get() = win::width(); return 1;    // EGL_WIDTH
        case 0x3056: *v.get() = win::height(); return 1;   // EGL_HEIGHT
        default: *v.get() = 0; return 1;
    }
}
u32 e_QueryContext(u32, u32, i32, gp<i32> v) {
    if (v.a) *v.get() = 2;
    return 1;
}
gaddr e_QueryString(u32, i32 name) {
    switch (name) {
        case 0x3053: return bionic::static_string("sbr64_pc");           // EGL_VENDOR
        case 0x3054: return bionic::static_string("1.4 sbr64_pc");       // EGL_VERSION
        case 0x3055: return bionic::static_string("");                   // EGL_EXTENSIONS
        default: return bionic::static_string("");
    }
}
u32 e_GetCurrentContext() { return wglGetCurrentContext() ? 1 : 0; }
u32 e_GetCurrentDisplay() { return kDisplay; }
u32 e_GetCurrentSurface(i32) { return kWindowSurface; }
u32 e_BindAPI(u32) { return 1; }
u32 e_WaitGL() { return 1; }
gaddr e_GetProcAddress(cstr name) {
    gaddr a = cpu::find_thunk(name.get());
    if (!a) ZLOGW("eglGetProcAddress(%s): not bridged", name.get());
    return a;
}

}  // namespace

void set_vsync_mode(int mode) { g_vsync_mode = mode; }

unsigned long long presented_frames() { return g_frame_count.load(); }

void set_screenshot(const std::string& path, const std::vector<int>& frames) {
    g_shot_path = path;
    g_shot_frames = path.empty() ? std::vector<int>{} : frames;
}

void register_egl_gles() {
    REG(eglGetDisplay, e_GetDisplay);
    REG(eglInitialize, e_Initialize);
    REG(eglTerminate, e_Terminate);
    REG(eglChooseConfig, e_ChooseConfig);
    REG(eglGetConfigs, e_GetConfigs);
    REG(eglGetConfigAttrib, e_GetConfigAttrib);
    REG(eglCreateWindowSurface, e_CreateWindowSurface);
    REG(eglCreatePbufferSurface, e_CreatePbufferSurface);
    REG(eglCreateContext, e_CreateContext);
    REG(eglDestroyContext, e_DestroyContext);
    REG(eglDestroySurface, e_DestroySurface);
    REG(eglMakeCurrent, e_MakeCurrent);
    REG(eglSwapBuffers, e_SwapBuffers);
    REG(eglSwapInterval, e_SwapInterval);
    REG(eglGetError, e_GetError);
    REG(eglQuerySurface, e_QuerySurface);
    REG(eglQueryContext, e_QueryContext);
    REG(eglQueryString, e_QueryString);
    REG(eglGetCurrentContext, e_GetCurrentContext);
    REG(eglGetCurrentDisplay, e_GetCurrentDisplay);
    REG(eglGetCurrentSurface, e_GetCurrentSurface);
    REG(eglBindAPI, e_BindAPI);
    REG(eglWaitGL, e_WaitGL);
    REG(eglGetProcAddress, e_GetProcAddress);

    // Special cases
    REG(glBindFramebuffer, g_BindFramebuffer);
    REG(glBufferData, g_BufferData);
    REG(glBufferSubData, g_BufferSubData);
    REG(glClearDepthf, g_ClearDepthf);
    REG(glDepthRangef, g_DepthRangef);
    REG(glShaderSource, g_ShaderSource);
    REG(glGetString, g_GetString);
    REG(glGetIntegerv, g_GetIntegerv);
    REG(glCompressedTexImage2D, g_CompressedTexImage2D);
    REG(glCompressedTexSubImage2D, g_CompressedTexSubImage2D);

    // Direct 1:1 forwarding
    GL(ActiveTexture, void, u32)
    GL(AttachShader, void, u32, u32)
    GL(BindAttribLocation, void, u32, u32, cstr)
    GL(BindBuffer, void, u32, u32)
    GL(BindRenderbuffer, void, u32, u32)
    GL(BindTexture, void, u32, u32)
    GL(BlendColor, void, float, float, float, float)
    GL(BlendEquation, void, u32)
    GL(BlendEquationSeparate, void, u32, u32)
    GL(BlendFunc, void, u32, u32)
    GL(BlendFuncSeparate, void, u32, u32, u32, u32)
    GL(CheckFramebufferStatus, u32, u32)
    GL(Clear, void, u32)
    GL(ClearColor, void, float, float, float, float)
    GL(ClearStencil, void, i32)
    GL(ColorMask, void, u8, u8, u8, u8)
    GL(CompileShader, void, u32)
    GL(CopyTexImage2D, void, u32, i32, u32, i32, i32, i32, i32, i32)
    GL(CopyTexSubImage2D, void, u32, i32, i32, i32, i32, i32, i32, i32)
    GL(CreateProgram, u32)
    GL(CreateShader, u32, u32)
    GL(CullFace, void, u32)
    GL(DeleteBuffers, void, i32, gp<const u32>)
    GL(DeleteFramebuffers, void, i32, gp<const u32>)
    GL(DeleteProgram, void, u32)
    GL(DeleteRenderbuffers, void, i32, gp<const u32>)
    GL(DeleteShader, void, u32)
    GL(DeleteTextures, void, i32, gp<const u32>)
    GL(DepthFunc, void, u32)
    GL(DepthMask, void, u8)
    GL(DetachShader, void, u32, u32)
    GL(Disable, void, u32)
    GL(DisableVertexAttribArray, void, u32)
    GL(DrawArrays, void, u32, i32, i32)
    GL(DrawElements, void, u32, i32, u32, cvoid)
    GL(Enable, void, u32)
    GL(EnableVertexAttribArray, void, u32)
    GL(Finish, void)
    GL(Flush, void)
    GL(FramebufferRenderbuffer, void, u32, u32, u32, u32)
    GL(FramebufferTexture2D, void, u32, u32, u32, u32, i32)
    GL(FrontFace, void, u32)
    GL(GenBuffers, void, i32, gp<u32>)
    GL(GenFramebuffers, void, i32, gp<u32>)
    GL(GenRenderbuffers, void, i32, gp<u32>)
    GL(GenTextures, void, i32, gp<u32>)
    GL(GenerateMipmap, void, u32)
    GL(GetActiveAttrib, void, u32, u32, i32, gp<i32>, gp<i32>, gp<u32>, gp<char>)
    GL(GetActiveUniform, void, u32, u32, i32, gp<i32>, gp<i32>, gp<u32>, gp<char>)
    GL(GetAttribLocation, i32, u32, cstr)
    GL(GetBooleanv, void, u32, gp<u8>)
    GL(GetError, u32)
    GL(GetFloatv, void, u32, gp<float>)
    GL(GetProgramInfoLog, void, u32, i32, gp<i32>, gp<char>)
    GL(GetProgramiv, void, u32, u32, gp<i32>)
    GL(GetShaderInfoLog, void, u32, i32, gp<i32>, gp<char>)
    GL(GetShaderiv, void, u32, u32, gp<i32>)
    GL(GetUniformLocation, i32, u32, cstr)
    GL(Hint, void, u32, u32)
    GL(IsEnabled, u8, u32)
    GL(IsTexture, u8, u32)
    GL(LineWidth, void, float)
    GL(LinkProgram, void, u32)
    GL(PixelStorei, void, u32, i32)
    GL(PolygonOffset, void, float, float)
    GL(ReadPixels, void, i32, i32, i32, i32, u32, u32, gp<void>)
    GL(RenderbufferStorage, void, u32, u32, i32, i32)
    GL(Scissor, void, i32, i32, i32, i32)
    GL(StencilFunc, void, u32, i32, u32)
    GL(StencilFuncSeparate, void, u32, u32, i32, u32)
    GL(StencilMask, void, u32)
    GL(StencilMaskSeparate, void, u32, u32)
    GL(StencilOp, void, u32, u32, u32)
    GL(StencilOpSeparate, void, u32, u32, u32, u32)
    GL(TexImage2D, void, u32, i32, i32, i32, i32, i32, u32, u32, cvoid)
    GL(TexParameterf, void, u32, u32, float)
    GL(TexParameteri, void, u32, u32, i32)
    GL(TexSubImage2D, void, u32, i32, i32, i32, i32, i32, u32, u32, cvoid)
    GL(Uniform1f, void, i32, float)
    GL(Uniform2f, void, i32, float, float)
    GL(Uniform3f, void, i32, float, float, float)
    GL(Uniform4f, void, i32, float, float, float, float)
    GL(Uniform1i, void, i32, i32)
    GL(Uniform1fv, void, i32, i32, gp<const float>)
    GL(Uniform2fv, void, i32, i32, gp<const float>)
    GL(Uniform3fv, void, i32, i32, gp<const float>)
    GL(Uniform4fv, void, i32, i32, gp<const float>)
    GL(Uniform1iv, void, i32, i32, gp<const i32>)
    GL(UniformMatrix2fv, void, i32, i32, u8, gp<const float>)
    GL(UniformMatrix3fv, void, i32, i32, u8, gp<const float>)
    GL(UniformMatrix4fv, void, i32, i32, u8, gp<const float>)
    GL(UseProgram, void, u32)
    GL(ValidateProgram, void, u32)
    GL(VertexAttrib4f, void, u32, float, float, float, float)
    GL(VertexAttribPointer, void, u32, i32, u32, u8, i32, cvoid)
    GL(Viewport, void, i32, i32, i32, i32)

    // Extensions the game may query through eglGetProcAddress
    GLN("glGenVertexArraysOES", "glGenVertexArrays", void, i32, gp<u32>)
    GLN("glBindVertexArrayOES", "glBindVertexArray", void, u32)
    GLN("glDeleteVertexArraysOES", "glDeleteVertexArrays", void, i32, gp<const u32>)
    GLN("glIsVertexArrayOES", "glIsVertexArray", u8, u32)
    REG_AS("glDiscardFramebufferEXT", x_Noop);
    REG_AS("glGetProgramBinaryOES", x_Noop);
    REG_AS("glProgramBinaryOES", x_Noop);
    GLN("glRenderbufferStorageMultisampleEXT", "glRenderbufferStorageMultisample", void, u32, i32, u32, i32, i32)
}

}  // namespace zb::android
