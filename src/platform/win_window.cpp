#include "platform/win_window.h"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

#include "core/common.h"
#include "platform/host_io.h"

namespace zb::win {
namespace {

// Dynamic XInput definitions (avoids hard link dependency on xinput.lib)
struct XInputGamepad {
    uint16_t wButtons;
    uint8_t bLeftTrigger;
    uint8_t bRightTrigger;
    int16_t sThumbLX;
    int16_t sThumbLY;
    int16_t sThumbRX;
    int16_t sThumbRY;
};

struct XInputState {
    uint32_t dwPacketNumber;
    XInputGamepad Gamepad;
};

using PFN_XInputGetState = DWORD(WINAPI*)(DWORD, XInputState*);

constexpr uint16_t kXInputDpadUp        = 0x0001;
constexpr uint16_t kXInputDpadDown      = 0x0002;
constexpr uint16_t kXInputDpadLeft      = 0x0004;
constexpr uint16_t kXInputDpadRight     = 0x0008;
constexpr uint16_t kXInputStart         = 0x0010;
constexpr uint16_t kXInputBack          = 0x0020;
constexpr uint16_t kXInputLeftShoulder  = 0x0100;
constexpr uint16_t kXInputRightShoulder = 0x0200;
constexpr uint16_t kXInputA             = 0x1000;
constexpr uint16_t kXInputB             = 0x2000;
constexpr uint16_t kXInputX             = 0x4000;
constexpr uint16_t kXInputY             = 0x8000;

PFN_XInputGetState g_xinput_get_state = nullptr;
bool g_xinput_init_done = false;

void init_xinput() {
    if (g_xinput_init_done) return;
    g_xinput_init_done = true;
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
        if (HMODULE m = LoadLibraryA(dll)) {
            g_xinput_get_state = reinterpret_cast<PFN_XInputGetState>(GetProcAddress(m, "XInputGetState"));
            if (g_xinput_get_state) {
                ZLOGI("input: loaded XInput from %s", dll);
                return;
            }
        }
    }
}

std::thread g_thread;
std::mutex g_mutex;
std::condition_variable g_ready_cv;
bool g_ready = false;
bool g_ok = false;

std::mutex g_ev_mutex;
std::deque<Event> g_events;

std::atomic<HWND> g_hwnd{nullptr};
std::atomic<HDC> g_hdc{nullptr};
std::atomic<int> g_w{1280}, g_h{720};            // client area of the window
std::atomic<int> g_game_w{1280}, g_game_h{720};  // fixed size the game renders at (scaled into the client area)
std::atomic<bool> g_close{false};
bool g_fullscreen = false;
WINDOWPLACEMENT g_windowed_place{sizeof(WINDOWPLACEMENT)};

constexpr UINT WM_APP_TOGGLE_FULLSCREEN = WM_APP + 1;
constexpr UINT WM_APP_SET_CLIENT_SIZE = WM_APP + 2;
std::atomic<bool> g_focus{false};
bool g_mouse_down = false;
float g_mouse_x = 640.0f;
float g_mouse_y = 360.0f;
DWORD g_ui_thread_id = 0;

void push(const Event& e) {
    {
        std::lock_guard lk(g_ev_mutex);
        // Motion moves are coalesced so a slow game never builds up a backlog.
        if (e.type == EV_MOTION && e.action == ACT_MOVE && !g_events.empty()) {
            Event& b = g_events.back();
            if (b.type == EV_MOTION && b.action == ACT_MOVE && b.pointer == e.pointer) {
                b.x = e.x;
                b.y = e.y;
                hostio::notify();
                return;
            }
        }
        g_events.push_back(e);
    }
    hostio::notify();
}

float game_w() { return static_cast<float>(std::max(1, g_game_w.load())); }
float game_h() { return static_cast<float>(std::max(1, g_game_h.load())); }

// Client pixels -> game pixels, through the letterboxed area the game image is scaled into.
float map_x(int raw_x) {
    const Rect r = game_rect();
    const float x = r.w > 0 ? (static_cast<float>(raw_x - r.x) * game_w() / static_cast<float>(r.w)) : 0.0f;
    return std::clamp(x, 0.0f, game_w());
}

float map_y(int raw_y) {
    const Rect r = game_rect();
    const float y = r.h > 0 ? (static_cast<float>(raw_y - r.y) * game_h() / static_cast<float>(r.h)) : 0.0f;
    return std::clamp(y, 0.0f, game_h());
}

// Pixels per 96-dpi unit of the primary monitor (1.25 at 125 % display scaling).
float dpi_scale() {
    using PFN_GetDpiForSystem = UINT(WINAPI*)();
    static auto get_dpi = reinterpret_cast<PFN_GetDpiForSystem>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForSystem")));
    UINT dpi = get_dpi ? get_dpi() : 96;
    if (!get_dpi) {
        HDC screen = GetDC(nullptr);
        dpi = static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX));
        ReleaseDC(nullptr, screen);
    }
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

// Borderless window covering the monitor, and back to the previous windowed placement.
void apply_fullscreen(HWND h, bool on) {
    if (on == g_fullscreen) return;
    const LONG style = GetWindowLongA(h, GWL_STYLE);
    if (on) {
        MONITORINFO mi{sizeof mi};
        if (!GetWindowPlacement(h, &g_windowed_place) ||
            !GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi))
            return;
        SetWindowLongA(h, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongA(h, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &g_windowed_place);
        SetWindowPos(h, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen = on;
    ZLOGI("window: %s", on ? "full screen" : "windowed");
}

// Multi-frame synthesized touch gesture state for keyboard & gamepad
// Player::Update in libRSBR_arm32.so requires a touch to stay active for >= 3 frames
// (m_touchFrames >= 2 at 0x19109c) before evaluating Jump or Dash (TouchGetDragDelta.x > 40.0f).
enum class GestureKind {
    None,
    TapHold,
    SwipeUp,
    SwipeDown,
    SwipeLeft,
    SwipeRight,
};

struct SynthTouchState {
    bool active = false;
    bool key_held = false;
    WPARAM owner_vk = 0;
    GestureKind kind = GestureKind::None;
    float x = 640.0f;
    float y = 360.0f;
    float dx = 0.0f;
    float dy = 0.0f;
    int ticks_elapsed = 0;
    int move_ticks = 0;
    int min_hold_ticks = 4;
};

SynthTouchState g_synth;
uint32_t g_prev_pad_mask = 0;

void end_synth_touch() {
    if (!g_synth.active) return;
    push(Event{EV_MOTION, ACT_UP, 0, 0, g_synth.x, g_synth.y});
    g_synth = SynthTouchState{};
}

void start_synth_gesture(GestureKind kind, WPARAM owner_vk) {
    if (g_synth.active) {
        end_synth_touch();
    }
    const float w = game_w();
    const float h = game_h();
    g_synth.active = true;
    g_synth.key_held = true;
    g_synth.owner_vk = owner_vk;
    g_synth.kind = kind;
    g_synth.ticks_elapsed = 0;

    switch (kind) {
        case GestureKind::TapHold:
            g_synth.x = w * 0.5f;
            g_synth.y = h * 0.5f;
            g_synth.dx = 0.0f;
            g_synth.dy = 0.0f;
            g_synth.move_ticks = 0;
            g_synth.min_hold_ticks = 4;
            break;
        case GestureKind::SwipeUp:
            g_synth.x = w * 0.5f;
            g_synth.y = h * 0.65f;
            g_synth.dx = 0.0f;
            g_synth.dy = -std::max(48.0f, h * 0.08f);
            g_synth.move_ticks = 4;
            g_synth.min_hold_ticks = 5;
            break;
        case GestureKind::SwipeDown:
            g_synth.x = w * 0.5f;
            g_synth.y = h * 0.35f;
            g_synth.dx = 0.0f;
            g_synth.dy = std::max(48.0f, h * 0.08f);
            g_synth.move_ticks = 4;
            g_synth.min_hold_ticks = 5;
            break;
        case GestureKind::SwipeRight:
            g_synth.x = w * 0.35f;
            g_synth.y = h * 0.5f;
            g_synth.dx = std::max(96.0f, w * 0.11f);
            g_synth.dy = 0.0f;
            g_synth.move_ticks = 4;
            g_synth.min_hold_ticks = 5;
            break;
        case GestureKind::SwipeLeft:
            g_synth.x = w * 0.65f;
            g_synth.y = h * 0.5f;
            g_synth.dx = -std::max(96.0f, w * 0.11f);
            g_synth.dy = 0.0f;
            g_synth.move_ticks = 4;
            g_synth.min_hold_ticks = 5;
            break;
        default:
            break;
    }
    push(Event{EV_MOTION, ACT_DOWN, 0, 0, g_synth.x, g_synth.y});
}

void release_synth_owner(WPARAM owner_vk) {
    if (!g_synth.active) return;
    if (owner_vk == 0 || g_synth.owner_vk == owner_vk) {
        g_synth.key_held = false;
        if (g_synth.ticks_elapsed >= g_synth.min_hold_ticks) {
            end_synth_touch();
        }
    }
}

void tick_synth_and_gamepad() {
    if (g_synth.active) {
        g_synth.ticks_elapsed++;
        const float w = game_w();
        const float h = game_h();
        if (g_synth.ticks_elapsed <= g_synth.move_ticks) {
            g_synth.x = std::clamp(g_synth.x + g_synth.dx, 0.0f, w);
            g_synth.y = std::clamp(g_synth.y + g_synth.dy, 0.0f, h);
            push(Event{EV_MOTION, ACT_MOVE, 0, 0, g_synth.x, g_synth.y});
        }
        // Swipe gestures auto-release after completing their drag trajectory unless it's a jump hold
        bool is_holdable_jump = (g_synth.kind == GestureKind::TapHold || g_synth.kind == GestureKind::SwipeUp);
        if (!is_holdable_jump && g_synth.ticks_elapsed >= g_synth.min_hold_ticks) {
            end_synth_touch();
        } else if (!g_synth.key_held && g_synth.ticks_elapsed >= g_synth.min_hold_ticks) {
            end_synth_touch();
        }
    }

    init_xinput();
    if (!g_xinput_get_state || !g_focus.load()) return;

    XInputState st{};
    bool got_pad = false;
    for (DWORD idx = 0; idx < 4; ++idx) {
        if (g_xinput_get_state(idx, &st) == ERROR_SUCCESS) {
            got_pad = true;
            break;
        }
    }
    if (!got_pad) {
        if (g_prev_pad_mask != 0) {
            release_synth_owner(0xF000);
            g_prev_pad_mask = 0;
        }
        return;
    }

    const auto& gp = st.Gamepad;
    constexpr int16_t kStickDeadzone = 16384;
    constexpr uint8_t kTriggerThreshold = 128;

    bool pad_jump  = (gp.wButtons & kXInputA) != 0;
    bool pad_up    = (gp.wButtons & kXInputDpadUp) != 0 || (gp.sThumbLY > kStickDeadzone);
    bool pad_down  = (gp.wButtons & kXInputDpadDown) != 0 || (gp.sThumbLY < -kStickDeadzone);
    bool pad_right = (gp.wButtons & (kXInputB | kXInputX | kXInputRightShoulder | kXInputDpadRight)) != 0 ||
                     (gp.bRightTrigger > kTriggerThreshold) || (gp.sThumbLX > kStickDeadzone);
    bool pad_left  = (gp.wButtons & (kXInputY | kXInputLeftShoulder | kXInputDpadLeft)) != 0 ||
                     (gp.bLeftTrigger > kTriggerThreshold) || (gp.sThumbLX < -kStickDeadzone);
    bool pad_back  = (gp.wButtons & (kXInputStart | kXInputBack)) != 0;

    uint32_t mask = (pad_jump  ? (1u << 0) : 0u) |
                    (pad_up    ? (1u << 1) : 0u) |
                    (pad_down  ? (1u << 2) : 0u) |
                    (pad_right ? (1u << 3) : 0u) |
                    (pad_left  ? (1u << 4) : 0u) |
                    (pad_back  ? (1u << 5) : 0u);

    uint32_t pressed = mask & ~g_prev_pad_mask;
    uint32_t released = g_prev_pad_mask & ~mask;
    g_prev_pad_mask = mask;

    if (pressed & (1u << 5)) {
        push(Event{EV_KEY, ACT_DOWN, 4 /* AKEYCODE_BACK */, 0, 0, 0});
    }
    if (released & (1u << 5)) {
        push(Event{EV_KEY, ACT_UP, 4 /* AKEYCODE_BACK */, 0, 0, 0});
    }

    if (pressed & (1u << 3)) {
        start_synth_gesture(GestureKind::SwipeRight, 0xF003);
    } else if (pressed & (1u << 2)) {
        start_synth_gesture(GestureKind::SwipeDown, 0xF002);
    } else if (pressed & (1u << 4)) {
        start_synth_gesture(GestureKind::SwipeLeft, 0xF004);
    } else if (pressed & (1u << 1)) {
        start_synth_gesture(GestureKind::SwipeUp, 0xF001);
    } else if (pressed & (1u << 0)) {
        start_synth_gesture(GestureKind::TapHold, 0xF000);
    }

    if (released & (1u << 0)) release_synth_owner(0xF000);
    if (released & (1u << 1)) release_synth_owner(0xF001);
    if (released & (1u << 2)) release_synth_owner(0xF002);
    if (released & (1u << 3)) release_synth_owner(0xF003);
    if (released & (1u << 4)) release_synth_owner(0xF004);
}

int android_keycode(WPARAM vk) {
    switch (vk) {
        case VK_ESCAPE:
        case VK_BACK: return 4;        // AKEYCODE_BACK
        case VK_RETURN: return 66;     // AKEYCODE_ENTER
        case VK_SPACE: return 62;      // AKEYCODE_SPACE
        case VK_UP:
        case 'W':
        case 'Z': return 19;           // AKEYCODE_DPAD_UP
        case VK_DOWN:
        case 'S': return 20;           // AKEYCODE_DPAD_DOWN
        case VK_LEFT:
        case 'A':
        case 'Q': return 21;           // AKEYCODE_DPAD_LEFT
        case VK_RIGHT:
        case 'D': return 22;           // AKEYCODE_DPAD_RIGHT
        default: return 0;
    }
}

GestureKind gesture_for_vk(WPARAM vk) {
    switch (vk) {
        case VK_SPACE:
        case VK_RETURN:
            return GestureKind::TapHold;
        case VK_UP:
        case 'W':
        case 'Z':
            return GestureKind::SwipeUp;
        case VK_DOWN:
        case 'S':
            return GestureKind::SwipeDown;
        case VK_RIGHT:
        case 'D':
            return GestureKind::SwipeRight;
        case VK_LEFT:
        case 'A':
        case 'Q':
            return GestureKind::SwipeLeft;
        default:
            return GestureKind::None;
    }
}

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            g_w = LOWORD(lp);
            g_h = HIWORD(lp);
            return 0;
        case WM_SETFOCUS:
            g_focus = true;
            hostio::notify();
            return 0;
        case WM_KILLFOCUS:
            g_focus = false;
            if (g_mouse_down) {
                g_mouse_down = false;
                ReleaseCapture();
                push(Event{EV_MOTION, ACT_UP, 0, 0, g_mouse_x, g_mouse_y});
            }
            end_synth_touch();
            g_prev_pad_mask = 0;
            hostio::notify();
            return 0;
        case WM_TIMER:
            if (wp == 1) {
                tick_synth_and_gamepad();
            }
            return 0;
        case WM_CLOSE:
            g_close = true;
            hostio::notify();
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_APP_TOGGLE_FULLSCREEN:
            apply_fullscreen(h, !g_fullscreen);
            return 0;
        case WM_APP_SET_CLIENT_SIZE: {
            apply_fullscreen(h, false);
            RECT r{0, 0, LOWORD(lp), HIWORD(lp)};
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            ShowWindow(h, SW_RESTORE);
            SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
            return 0;
        }
        case WM_DPICHANGED:
            if (!g_fullscreen) {  // moved to a monitor with another scale: take the size Windows suggests
                const RECT* r = reinterpret_cast<const RECT*>(lp);
                SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        case WM_SYSKEYDOWN:
            if (wp == VK_RETURN && (lp & (1 << 29))) {  // Alt+Enter
                if (!(lp & (1 << 30))) apply_fullscreen(h, !g_fullscreen);
                return 0;
            }
            break;
        case WM_SYSCHAR:
            if (wp == '\r') return 0;  // no beep for Alt+Enter
            break;
        case WM_LBUTTONDOWN:
            g_mouse_down = true;
            g_mouse_x = map_x(GET_X_LPARAM(lp));
            g_mouse_y = map_y(GET_Y_LPARAM(lp));
            SetCapture(h);
            push(Event{EV_MOTION, ACT_DOWN, 0, 0, g_mouse_x, g_mouse_y});
            return 0;
        case WM_MOUSEMOVE:
            if (g_mouse_down) {
                g_mouse_x = map_x(GET_X_LPARAM(lp));
                g_mouse_y = map_y(GET_Y_LPARAM(lp));
                push(Event{EV_MOTION, ACT_MOVE, 0, 0, g_mouse_x, g_mouse_y});
            }
            return 0;
        case WM_LBUTTONUP:
            if (g_mouse_down) {
                g_mouse_down = false;
                g_mouse_x = map_x(GET_X_LPARAM(lp));
                g_mouse_y = map_y(GET_Y_LPARAM(lp));
                ReleaseCapture();
                push(Event{EV_MOTION, ACT_UP, 0, 0, g_mouse_x, g_mouse_y});
            }
            return 0;
        case WM_KEYDOWN:
        case WM_KEYUP: {
            if (msg == WM_KEYDOWN && (lp & (1 << 30))) return 0;  // auto-repeat
            if (wp == VK_F11) {
                if (msg == WM_KEYDOWN) apply_fullscreen(h, !g_fullscreen);
                return 0;
            }
            int k = android_keycode(wp);
            if (k) push(Event{EV_KEY, msg == WM_KEYDOWN ? 0 : 1, k, 0, 0, 0});
            GestureKind gk = gesture_for_vk(wp);
            if (gk != GestureKind::None) {
                if (msg == WM_KEYDOWN) {
                    start_synth_gesture(gk, wp);
                } else {
                    release_synth_owner(wp);
                }
            }
            return 0;
        }
        default: break;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

void ui_thread(int cw, int ch, std::string title, bool fullscreen) {
    g_ui_thread_id = GetCurrentThreadId();
    WNDCLASSA wc{};
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = "SBR64PCWindow";
    RegisterClassA(&wc);

    // The process is DPI aware (see enable_dpi_awareness): size the window in physical pixels so that it
    // looks as large as an unscaled cw x ch window would on this display.
    const float scale = dpi_scale();
    RECT r{0, 0, static_cast<LONG>(static_cast<float>(cw) * scale), static_cast<LONG>(static_cast<float>(ch) * scale)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND h = CreateWindowExA(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                             CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    bool ok = false;
    if (h) {
        HDC dc = GetDC(h);
        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cAlphaBits = 8;
        pfd.cDepthBits = 24;
        pfd.cStencilBits = 8;
        pfd.iLayerType = PFD_MAIN_PLANE;
        int fmt = ChoosePixelFormat(dc, &pfd);
        if (fmt && SetPixelFormat(dc, fmt, &pfd)) {
            ok = true;
            g_hwnd = h;
            g_hdc = dc;
            RECT cr;
            GetClientRect(h, &cr);
            g_w = cr.right - cr.left;
            g_h = cr.bottom - cr.top;
            g_focus = true;
        } else {
            ZLOGE("window: no OpenGL pixel format (error %lu)", GetLastError());
        }
    } else {
        ZLOGE("window: CreateWindowEx failed (error %lu)", GetLastError());
    }
    {
        std::lock_guard lk(g_mutex);
        g_ok = ok;
        g_ready = true;
    }
    g_ready_cv.notify_all();
    if (!ok) return;
    if (fullscreen) apply_fullscreen(h, true);

    SetTimer(h, 1, 16, nullptr);
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    KillTimer(h, 1);
    ReleaseDC(h, g_hdc.load());
    g_hdc = nullptr;
    g_hwnd = nullptr;
}

}  // namespace

void enable_dpi_awareness() {
    static std::once_flag once;
    std::call_once(once, [] {
        // Per-monitor v2 (Windows 10 1703+), else system-wide awareness: Windows must not bitmap-stretch the
        // window, or full screen would render at the scaled-down "virtual" resolution and look blurry.
        using PFN_SetCtx = BOOL(WINAPI*)(HANDLE);
        auto set_ctx = reinterpret_cast<PFN_SetCtx>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext")));
        if (!set_ctx || !set_ctx(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4))))  // PER_MONITOR_AWARE_V2
            SetProcessDPIAware();
    });
}

void primary_monitor_size(int* w, int* h) {
    enable_dpi_awareness();
    *w = GetSystemMetrics(SM_CXSCREEN);
    *h = GetSystemMetrics(SM_CYSCREEN);
}

bool start(int client_w, int client_h, const char* title, bool fullscreen) {
    if (g_thread.joinable()) return true;
    enable_dpi_awareness();
    g_ready = false;
    g_game_w = client_w;
    g_game_h = client_h;
    g_thread = std::thread(ui_thread, client_w, client_h, std::string(title), fullscreen);
    std::unique_lock lk(g_mutex);
    g_ready_cv.wait(lk, [] { return g_ready; });
    return g_ok;
}

void stop() {
    if (!g_thread.joinable()) return;
    HWND h = g_hwnd.load();
    if (h) PostMessageA(h, WM_QUIT, 0, 0);
    if (g_ui_thread_id) PostThreadMessageA(g_ui_thread_id, WM_QUIT, 0, 0);
    if (h) DestroyWindow(h);
    g_thread.join();
}

void* hwnd() { return g_hwnd.load(); }
void* hdc() { return g_hdc.load(); }
int width() { return g_game_w.load(); }
int height() { return g_game_h.load(); }
int client_width() { return g_w.load(); }
int client_height() { return g_h.load(); }

Rect game_rect() {
    const int cw = g_w.load(), ch = g_h.load(), gw = std::max(1, g_game_w.load()), gh = std::max(1, g_game_h.load());
    if (cw <= 0 || ch <= 0) return {};
    // Largest area with the game's aspect ratio, centred (black bars on the remaining sides).
    Rect r;
    if (static_cast<long long>(cw) * gh <= static_cast<long long>(ch) * gw) {
        r.w = cw;
        r.h = static_cast<int>(static_cast<long long>(cw) * gh / gw);
    } else {
        r.h = ch;
        r.w = static_cast<int>(static_cast<long long>(ch) * gw / gh);
    }
    r.x = (cw - r.w) / 2;
    r.y = (ch - r.h) / 2;
    return r;
}

void toggle_fullscreen() {
    if (HWND h = g_hwnd.load()) PostMessageA(h, WM_APP_TOGGLE_FULLSCREEN, 0, 0);
}

void simulate_focus(bool gained) {
    if (HWND h = g_hwnd.load()) PostMessageA(h, gained ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
}

void set_client_size(int w, int h) {
    if (HWND wnd = g_hwnd.load()) PostMessageA(wnd, WM_APP_SET_CLIENT_SIZE, 0, MAKELPARAM(w, h));
}

bool pop_event(Event* out) {
    std::lock_guard lk(g_ev_mutex);
    if (g_events.empty()) return false;
    *out = g_events.front();
    g_events.pop_front();
    return true;
}

bool has_events() {
    std::lock_guard lk(g_ev_mutex);
    return !g_events.empty();
}

void post_key(unsigned vk, bool down) {
    HWND h = g_hwnd.load();
    if (!h) return;
    const LPARAM lp = down ? 1 : static_cast<LPARAM>(0xC0000001);  // repeat count 1; up: previous+transition bits
    PostMessageA(h, down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
}

void post_mouse(int x, int y, bool down) {
    HWND h = g_hwnd.load();
    if (!h) return;
    PostMessageA(h, down ? WM_LBUTTONDOWN : WM_LBUTTONUP, down ? MK_LBUTTON : 0, MAKELPARAM(x, y));
}

bool close_requested() { return g_close.load(); }
void request_close() {
    g_close = true;
    hostio::notify();
}
bool focused() { return g_focus.load(); }

}  // namespace zb::win
