#include "android/ndk_glue.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "android/bionic_hle.h"
#include "core/cpu.h"
#include "core/elf_loader.h"
#include "core/mem_win32.h"
#include "core/thunk.h"
#include "platform/host_io.h"
#include "platform/win_window.h"

namespace zb::android::ndk {
namespace {

constexpr u32 kWindowHandle = 0x17000001;
constexpr u32 kQueueHandle = 0x14000001;
constexpr u32 kAssetMgrHandle = 0x11000001;
constexpr u32 kLooperHandle = 0x16000001;
constexpr u32 kSensorMgrHandle = 0x18000001;
constexpr u32 kSensorQueueHandle = 0x1A000001;

std::string g_asset_root = "data\\assets";

// ------------------------------------------------------------ assets

struct Asset {
    FILE* f = nullptr;
    i64 len = 0;
    i64 pos = 0;
};
std::mutex g_asset_mutex;
std::unordered_map<u32, std::unique_ptr<Asset>> g_assets;
u32 g_next_asset = 0x12000001;

u32 a_AssetManager_fromJava(u32, u32) { return kAssetMgrHandle; }

u32 a_AssetManager_open(u32, gp<const char> name, i32) {
    std::string path = g_asset_root + "\\" + name.get();
    for (char& c : path)
        if (c == '/') c = '\\';
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        ZLOGW("AAssetManager_open(%s): not found (%s)", name.get(), path.c_str());
        return 0;
    }
    auto a = std::make_unique<Asset>();
    a->f = f;
    std::fseek(f, 0, SEEK_END);
    a->len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::lock_guard lk(g_asset_mutex);
    u32 id = g_next_asset++;
    g_assets[id] = std::move(a);
    return id;
}

Asset* asset(u32 h) {
    auto it = g_assets.find(h);
    return it == g_assets.end() ? nullptr : it->second.get();
}

void a_Asset_close(u32 h) {
    std::lock_guard lk(g_asset_mutex);
    auto it = g_assets.find(h);
    if (it == g_assets.end()) return;
    if (it->second->f) std::fclose(it->second->f);
    g_assets.erase(it);
}
i32 a_Asset_getLength(u32 h) {
    std::lock_guard lk(g_asset_mutex);
    Asset* a = asset(h);
    return a ? static_cast<i32>(a->len) : 0;
}
i32 a_Asset_getRemainingLength(u32 h) {
    std::lock_guard lk(g_asset_mutex);
    Asset* a = asset(h);
    return a ? static_cast<i32>(a->len - a->pos) : 0;
}
i32 a_Asset_read(u32 h, gaddr buf, u32 n) {
    std::lock_guard lk(g_asset_mutex);
    Asset* a = asset(h);
    if (!a) return -1;
    size_t r = std::fread(hp(buf), 1, n, a->f);
    a->pos += static_cast<i64>(r);
    return static_cast<i32>(r);
}
i32 a_Asset_seek(u32 h, i32 off, i32 whence) {
    std::lock_guard lk(g_asset_mutex);
    Asset* a = asset(h);
    if (!a) return -1;
    if (std::fseek(a->f, off, whence) != 0) return -1;
    a->pos = std::ftell(a->f);
    return static_cast<i32>(a->pos);
}

// ------------------------------------------------------------ configuration

char g_language[2] = {'e', 'n'};  // set_locale()
char g_country[2] = {'U', 'S'};

struct Config {
    char language[2] = {g_language[0], g_language[1]};
    char country[2] = {g_country[0], g_country[1]};
};
std::mutex g_cfg_mutex;
std::unordered_map<u32, Config> g_configs;
u32 g_next_cfg = 0x13000001;

u32 a_Configuration_new() {
    std::lock_guard lk(g_cfg_mutex);
    u32 id = g_next_cfg++;
    g_configs[id] = Config{};
    return id;
}
void a_Configuration_delete(u32 h) {
    std::lock_guard lk(g_cfg_mutex);
    g_configs.erase(h);
}
void a_Configuration_fromAssetManager(u32, u32) {}
void a_Configuration_getCountry(u32 c, gaddr out) {
    std::lock_guard lk(g_cfg_mutex);
    auto it = g_configs.find(c);
    if (it != g_configs.end()) std::memcpy(hp(out), it->second.country, 2);
}
void a_Configuration_getLanguage(u32 c, gaddr out) {
    std::lock_guard lk(g_cfg_mutex);
    auto it = g_configs.find(c);
    if (it != g_configs.end()) std::memcpy(hp(out), it->second.language, 2);
}
i32 a_Configuration_getSdkVersion(u32) { return 24; }
i32 a_Configuration_getMcc(u32) { return 0; }
i32 a_Configuration_getMnc(u32) { return 0; }
i32 a_Configuration_getOrientation(u32) { return 2; }   // ACONFIGURATION_ORIENTATION_LAND
i32 a_Configuration_getTouchscreen(u32) { return 3; }   // FINGER
i32 a_Configuration_getDensity(u32) { return 320; }
i32 a_Configuration_getKeyboard(u32) { return 1; }      // NOKEYS
i32 a_Configuration_getNavigation(u32) { return 1; }    // NONAV
i32 a_Configuration_getKeysHidden(u32) { return 2; }    // YES
i32 a_Configuration_getNavHidden(u32) { return 2; }     // YES
i32 a_Configuration_getScreenSize(u32) { return 3; }    // LARGE
i32 a_Configuration_getScreenLong(u32) { return 2; }    // YES
i32 a_Configuration_getUiModeType(u32) { return 1; }    // NORMAL
i32 a_Configuration_getUiModeNight(u32) { return 1; }   // NO

// ------------------------------------------------------------ input

std::mutex g_ev_mutex;
std::unordered_map<u32, win::Event> g_live_events;
u32 g_next_event = 0x15000001;

bool event_of(u32 h, win::Event* out) {
    std::lock_guard lk(g_ev_mutex);
    auto it = g_live_events.find(h);
    if (it == g_live_events.end()) return false;
    *out = it->second;
    return true;
}

i32 a_InputEvent_getDeviceId(u32) { return 0; }
i32 a_InputEvent_getSource(u32 e) {
    win::Event ev;
    if (!event_of(e, &ev)) return 0;
    return ev.type == win::EV_MOTION ? 0x00001002 /* TOUCHSCREEN */ : 0x00000101 /* KEYBOARD */;
}
i32 a_InputEvent_getType(u32 e) {
    win::Event ev;
    return event_of(e, &ev) ? ev.type : 0;
}
i32 a_KeyEvent_getAction(u32 e) {
    win::Event ev;
    return event_of(e, &ev) ? ev.action : 0;
}
i32 a_KeyEvent_getKeyCode(u32 e) {
    win::Event ev;
    return event_of(e, &ev) ? ev.keycode : 0;
}
i32 a_KeyEvent_getMetaState(u32) { return 0; }
i32 a_KeyEvent_getScanCode(u32) { return 0; }
i32 a_MotionEvent_getAction(u32 e) {
    win::Event ev;
    return event_of(e, &ev) ? ev.action : 0;
}
float a_MotionEvent_getAxisValue(u32 e, i32 axis, u32) {
    win::Event ev;
    if (!event_of(e, &ev)) return 0;
    return axis == 0 ? ev.x : axis == 1 ? ev.y : 0.f;
}
u32 a_MotionEvent_getPointerCount(u32) { return 1; }
i32 a_MotionEvent_getPointerId(u32 e, u32) {
    win::Event ev;
    return event_of(e, &ev) ? ev.pointer : 0;
}
float a_MotionEvent_getPressure(u32, u32) { return 1.0f; }
float a_MotionEvent_getX(u32 e, u32) {
    win::Event ev;
    return event_of(e, &ev) ? ev.x : 0.f;
}
float a_MotionEvent_getY(u32 e, u32) {
    win::Event ev;
    return event_of(e, &ev) ? ev.y : 0.f;
}

i32 a_InputQueue_getEvent(u32, gp<u32> out) {
    win::Event ev;
    if (!win::pop_event(&ev)) {
        *out.get() = 0;
        return -1;
    }
    std::lock_guard lk(g_ev_mutex);
    u32 id = g_next_event++;
    g_live_events[id] = ev;
    *out.get() = id;
    return 0;
}
i32 a_InputQueue_preDispatchEvent(u32, u32 e) {
    win::Event ev;
    if (event_of(e, &ev) && ev.type == win::EV_KEY && ev.keycode == 4 && ev.action == win::ACT_DOWN) {
        if (gaddr bp = elf::resolve_symbol("_ZN7AppMain14g_bBackPressedE")) {
            wr<u8>(bp, 1);
        }
    }
    return 0;
}
void a_InputQueue_finishEvent(u32, u32 e, i32) {
    std::lock_guard lk(g_ev_mutex);
    g_live_events.erase(e);
}

// ------------------------------------------------------------ looper

struct Source {
    int ident = 0;
    int fd = -1;
    bool is_queue = false;
    u32 callback = 0;
    u32 data = 0;
};
std::mutex g_looper_mutex;
std::vector<Source> g_sources;

u32 a_Looper_prepare(i32) { return kLooperHandle; }

i32 a_Looper_addFd(u32, i32 fd, i32 ident, i32, gaddr fn, gaddr data) {
    std::lock_guard lk(g_looper_mutex);
    g_sources.push_back(Source{ident, fd, false, fn, data});
    return 1;
}

void a_InputQueue_attachLooper(u32, u32, i32 ident, gaddr fn, gaddr data) {
    std::lock_guard lk(g_looper_mutex);
    g_sources.push_back(Source{ident, -1, true, fn, data});
}
void a_InputQueue_detachLooper(u32) {
    std::lock_guard lk(g_looper_mutex);
    for (size_t i = 0; i < g_sources.size();) {
        if (g_sources[i].is_queue) g_sources.erase(g_sources.begin() + static_cast<long>(i));
        else ++i;
    }
}

bool source_ready(const Source& s) { return s.is_queue ? win::has_events() : hostio::pipe_readable(s.fd); }

i32 a_Looper_pollAll(i32 timeout, gp<i32> out_fd, gp<i32> out_events, gp<u32> out_data) {
    using clock = std::chrono::steady_clock;
    const auto deadline = timeout > 0 ? clock::now() + std::chrono::milliseconds(timeout) : clock::time_point{};
    for (;;) {
        uint64_t gen = hostio::generation();
        std::vector<Source> sources;
        {
            std::lock_guard lk(g_looper_mutex);
            sources = g_sources;
        }
        for (const Source& s : sources) {
            if (!source_ready(s)) continue;
            if (s.callback) {
                u32 r = static_cast<u32>(cpu::call(s.callback, cpu::Args().i32v(s.fd).i32v(1).u32v(s.data)));
                if (r == 0) {
                    std::lock_guard lk(g_looper_mutex);
                    for (size_t i = 0; i < g_sources.size(); ++i) {
                        if (g_sources[i].fd == s.fd && g_sources[i].is_queue == s.is_queue &&
                            g_sources[i].callback == s.callback) {
                            g_sources.erase(g_sources.begin() + static_cast<long>(i));
                            break;
                        }
                    }
                }
                continue;
            }
            if (out_fd.a) *out_fd.get() = s.fd;
            if (out_events.a) *out_events.get() = 1;  // ALOOPER_EVENT_INPUT
            if (out_data.a) *out_data.get() = s.data;
            return s.ident;
        }
        if (timeout == 0) break;
        int wait_ms = -1;
        if (timeout > 0) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
            if (left <= 0) break;
            wait_ms = static_cast<int>(left);
        }
        hostio::wait(gen, wait_ms);
    }
    if (out_fd.a) *out_fd.get() = 0;
    if (out_events.a) *out_events.get() = 0;
    if (out_data.a) *out_data.get() = 0;
    return -3;  // ALOOPER_POLL_TIMEOUT
}

// ------------------------------------------------------------ window / sensors

i32 a_NativeWindow_getFormat(u32) { return 1; }  // WINDOW_FORMAT_RGBA_8888
i32 a_NativeWindow_getHeight(u32) { return win::height(); }
i32 a_NativeWindow_getWidth(u32) { return win::width(); }
i32 a_NativeWindow_setBuffersGeometry(u32, i32, i32, i32) { return 0; }

// No sensors on the PC: the game gets a manager but no default sensor.
u32 a_SensorManager_getInstance() { return kSensorMgrHandle; }
u32 a_SensorManager_getDefaultSensor(u32, i32) { return 0; }
u32 a_SensorManager_createEventQueue(u32, u32, i32, gaddr, gaddr) { return kSensorQueueHandle; }
i32 a_SensorManager_destroyEventQueue(u32, u32) { return 0; }
i32 a_SensorEventQueue_enableSensor(u32, u32) { return -1; }
i32 a_SensorEventQueue_disableSensor(u32, u32) { return 0; }
i32 a_SensorEventQueue_setEventRate(u32, u32, i32) { return 0; }
i32 a_SensorEventQueue_getEvents(u32, gaddr, u32) { return 0; }

void a_NativeActivity_finish(gaddr) { win::request_close(); }

// ------------------------------------------------------------ pipes (override of the HLE stubs)

i32 p_pipe(gp<i32> fds) {
    int f[2];
    hostio::pipe_create(f);
    fds.get()[0] = f[0];
    fds.get()[1] = f[1];
    return 0;
}
i32 p_close(i32 fd) {
    if (hostio::is_pipe(fd)) hostio::pipe_close(fd);
    return 0;
}
i32 p_read(JitCtx& j, i32 fd, gaddr buf, u32 n) {
    if (!hostio::is_pipe(fd)) return 0;
    long r = hostio::pipe_read(fd, hp(buf), n);
    if (r < 0) j.set_errno(11);  // EAGAIN
    return static_cast<i32>(r);
}
i32 p_write(i32 fd, gaddr buf, u32 n) {
    if (hostio::is_pipe(fd)) return static_cast<i32>(hostio::pipe_write(fd, hp(buf), n));
    if (fd == 1 || fd == 2) {
        FILE* out = fd == 2 ? stderr : stdout;
        std::fwrite(hp(buf), 1, n, out);
        std::fflush(out);
    }
    return static_cast<i32>(n);
}

}  // namespace

u32 window_handle() { return kWindowHandle; }
u32 input_queue_handle() { return kQueueHandle; }
u32 asset_manager_handle() { return kAssetMgrHandle; }
void set_asset_root(const std::string& dir) { g_asset_root = dir; }
void set_locale(const std::string& language, const std::string& country) {
    if (language.size() == 2) std::memcpy(g_language, language.data(), 2);
    if (country.size() == 2) std::memcpy(g_country, country.data(), 2);
}

void register_all() {
    REG(AAssetManager_fromJava, a_AssetManager_fromJava);
    REG(AAssetManager_open, a_AssetManager_open);
    REG(AAsset_close, a_Asset_close);
    REG(AAsset_getLength, a_Asset_getLength);
    REG(AAsset_getRemainingLength, a_Asset_getRemainingLength);
    REG(AAsset_read, a_Asset_read);
    REG(AAsset_seek, a_Asset_seek);
    REG(AConfiguration_new, a_Configuration_new);
    REG(AConfiguration_delete, a_Configuration_delete);
    REG(AConfiguration_fromAssetManager, a_Configuration_fromAssetManager);
    REG(AConfiguration_getCountry, a_Configuration_getCountry);
    REG(AConfiguration_getLanguage, a_Configuration_getLanguage);
    REG(AConfiguration_getSdkVersion, a_Configuration_getSdkVersion);
    REG(AConfiguration_getMcc, a_Configuration_getMcc);
    REG(AConfiguration_getMnc, a_Configuration_getMnc);
    REG(AConfiguration_getOrientation, a_Configuration_getOrientation);
    REG(AConfiguration_getTouchscreen, a_Configuration_getTouchscreen);
    REG(AConfiguration_getDensity, a_Configuration_getDensity);
    REG(AConfiguration_getKeyboard, a_Configuration_getKeyboard);
    REG(AConfiguration_getNavigation, a_Configuration_getNavigation);
    REG(AConfiguration_getKeysHidden, a_Configuration_getKeysHidden);
    REG(AConfiguration_getNavHidden, a_Configuration_getNavHidden);
    REG(AConfiguration_getScreenSize, a_Configuration_getScreenSize);
    REG(AConfiguration_getScreenLong, a_Configuration_getScreenLong);
    REG(AConfiguration_getUiModeType, a_Configuration_getUiModeType);
    REG(AConfiguration_getUiModeNight, a_Configuration_getUiModeNight);
    REG(AInputEvent_getDeviceId, a_InputEvent_getDeviceId);
    REG(AInputEvent_getSource, a_InputEvent_getSource);
    REG(AInputEvent_getType, a_InputEvent_getType);
    REG(AKeyEvent_getAction, a_KeyEvent_getAction);
    REG(AKeyEvent_getKeyCode, a_KeyEvent_getKeyCode);
    REG(AKeyEvent_getMetaState, a_KeyEvent_getMetaState);
    REG(AKeyEvent_getScanCode, a_KeyEvent_getScanCode);
    REG(AMotionEvent_getAction, a_MotionEvent_getAction);
    REG(AMotionEvent_getAxisValue, a_MotionEvent_getAxisValue);
    REG(AMotionEvent_getPointerCount, a_MotionEvent_getPointerCount);
    REG(AMotionEvent_getPointerId, a_MotionEvent_getPointerId);
    REG(AMotionEvent_getPressure, a_MotionEvent_getPressure);
    REG(AMotionEvent_getX, a_MotionEvent_getX);
    REG(AMotionEvent_getY, a_MotionEvent_getY);
    REG(AInputQueue_attachLooper, a_InputQueue_attachLooper);
    REG(AInputQueue_detachLooper, a_InputQueue_detachLooper);
    REG(AInputQueue_getEvent, a_InputQueue_getEvent);
    REG(AInputQueue_preDispatchEvent, a_InputQueue_preDispatchEvent);
    REG(AInputQueue_finishEvent, a_InputQueue_finishEvent);
    REG(ALooper_prepare, a_Looper_prepare);
    REG(ALooper_addFd, a_Looper_addFd);
    REG(ALooper_pollAll, a_Looper_pollAll);
    REG(ANativeWindow_getFormat, a_NativeWindow_getFormat);
    REG(ANativeWindow_getHeight, a_NativeWindow_getHeight);
    REG(ANativeWindow_getWidth, a_NativeWindow_getWidth);
    REG(ANativeWindow_setBuffersGeometry, a_NativeWindow_setBuffersGeometry);
    REG(ASensorManager_getInstance, a_SensorManager_getInstance);
    REG(ASensorManager_getDefaultSensor, a_SensorManager_getDefaultSensor);
    REG(ASensorManager_createEventQueue, a_SensorManager_createEventQueue);
    REG(ASensorManager_destroyEventQueue, a_SensorManager_destroyEventQueue);
    REG(ASensorEventQueue_enableSensor, a_SensorEventQueue_enableSensor);
    REG(ASensorEventQueue_disableSensor, a_SensorEventQueue_disableSensor);
    REG(ASensorEventQueue_setEventRate, a_SensorEventQueue_setEventRate);
    REG(ASensorEventQueue_getEvents, a_SensorEventQueue_getEvents);
    REG(ANativeActivity_finish, a_NativeActivity_finish);

    // Replace the HLE's placeholder pipe/read/write/close with a working pipe.
    REG(pipe, p_pipe);
    REG(close, p_close);
    REG(read, p_read);
    REG(write, p_write);
}

}  // namespace zb::android::ndk
