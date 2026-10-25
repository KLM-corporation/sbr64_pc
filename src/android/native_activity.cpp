#include "native_activity.h"

#include <cstring>
#include "core/mem_win32.h"
#include "core/cpu.h"
#include "jni_env.h"

namespace zb::android {

namespace {

ActivityConfig g_config;
ActivityState g_state = ActivityState::Uninitialized;

gNativeActivity g_act_ptr = 0;
gaddr g_cbs_ptr = 0;
gaddr g_app_ptr = 0;

// Callback table offsets in ANativeActivityCallbacks (64 bytes total)
constexpr u32 kCbOnStart                    = 0x00;
constexpr u32 kCbOnResume                   = 0x04;
constexpr u32 kCbOnSaveInstanceState        = 0x08;
constexpr u32 kCbOnPause                    = 0x0c;
constexpr u32 kCbOnStop                     = 0x10;
constexpr u32 kCbOnDestroy                  = 0x14;
constexpr u32 kCbOnWindowFocusChanged       = 0x18;
constexpr u32 kCbOnNativeWindowCreated      = 0x1c;
constexpr u32 kCbOnNativeWindowResized      = 0x20;
constexpr u32 kCbOnNativeWindowRedrawNeeded = 0x24;
constexpr u32 kCbOnNativeWindowDestroyed    = 0x28;
constexpr u32 kCbOnInputQueueCreated        = 0x2c;
constexpr u32 kCbOnInputQueueDestroyed      = 0x30;
constexpr u32 kCbOnContentRectChanged       = 0x34;
constexpr u32 kCbOnConfigurationChanged     = 0x38;
constexpr u32 kCbOnLowMemory                = 0x3c;

void invoke_callback(u32 offset, const cpu::Args& args) {
    if (!g_cbs_ptr) return;
    gaddr fn = rd<u32>(g_cbs_ptr + offset);
    if (fn) {
        cpu::call(fn, args);
    }
}

} // namespace

void init(const ActivityConfig& cfg) {
    g_config = cfg;
    g_state = ActivityState::Uninitialized;
    g_act_ptr = 0;
    g_cbs_ptr = 0;
    g_app_ptr = 0;
}

void shutdown() {
    if (g_state != ActivityState::Uninitialized && g_state != ActivityState::Destroyed) {
        notify_destroy();
    }
    g_state = ActivityState::Destroyed;
}

bool create_activity(void* saved_state, size_t saved_state_size) {
    jni::init();

    // 1. Allocate ANativeActivity struct (40 bytes) and callbacks struct (64 bytes)
    g_act_ptr = mem::gcalloc(1, 40);
    g_cbs_ptr = mem::gcalloc(16, 4);

    if (!g_act_ptr || !g_cbs_ptr) {
        fatal("Failed to allocate guest ANativeActivity or callbacks struct");
        return false;
    }

    gaddr env = jni::bind_env();
    u32 act_cls = jni::create_local_class("com/playstation/runsackboyrun/RSBR");

    wr<u32>(g_act_ptr + 0x00, g_cbs_ptr);                                         // callbacks
    wr<u32>(g_act_ptr + 0x04, jni::guest_vm());                                   // vm
    wr<u32>(g_act_ptr + 0x08, env);                                               // env
    wr<u32>(g_act_ptr + 0x0c, jni::to_global_handle(act_cls));                    // clazz
    wr<u32>(g_act_ptr + 0x10, mem::gstrdup(g_config.internal_data_path.c_str()));// internalDataPath
    wr<u32>(g_act_ptr + 0x14, mem::gstrdup(g_config.external_data_path.c_str()));// externalDataPath
    wr<i32>(g_act_ptr + 0x18, g_config.sdk_version);                              // sdkVersion
    wr<u32>(g_act_ptr + 0x1c, 0);                                                 // instance (populated by onCreate)
    wr<u32>(g_act_ptr + 0x20, g_config.asset_manager);                            // assetManager
    wr<u32>(g_act_ptr + 0x24, mem::gstrdup(g_config.obb_path.c_str()));          // obbPath

    // 2. Duplicate saved state if supplied
    gaddr saved_g = 0;
    if (saved_state && saved_state_size > 0) {
        saved_g = mem::gdup(saved_state, static_cast<u32>(saved_state_size));
    }

    ZLOGI("Bootstrapping ANativeActivity_onCreate at 0x%08x (activity=0x%08x)", g_config.entry_onCreate, g_act_ptr);

    // 3. Invoke ANativeActivity_onCreate
    cpu::call(g_config.entry_onCreate, cpu::Args().u32v(g_act_ptr).u32v(saved_g).u32v(static_cast<u32>(saved_state_size)));

    // 4. Retrieve allocated android_app instance pointer from activity->instance (offset 0x1c)
    g_app_ptr = rd<u32>(g_act_ptr + 0x1c);
    if (!g_app_ptr) {
        ZLOGE("ANativeActivity_onCreate failed: activity->instance was not populated");
        return false;
    }

    ZLOGI("ANativeActivity_onCreate succeeded! Guest android_app instance at 0x%08x", g_app_ptr);
    g_state = ActivityState::Created;
    return true;
}

gNativeActivity get_guest_activity() {
    return g_act_ptr;
}

gaddr get_guest_callbacks() {
    return g_cbs_ptr;
}

gaddr get_guest_app_instance() {
    return g_app_ptr;
}

void notify_start() {
    if (g_state == ActivityState::Created || g_state == ActivityState::Stopped) {
        ZLOGI("[ActivityLifecycle] -> onStart");
        invoke_callback(kCbOnStart, cpu::Args().u32v(g_act_ptr));
        g_state = ActivityState::Started;
    }
}

void notify_resume() {
    if (g_state == ActivityState::Started || g_state == ActivityState::Paused) {
        ZLOGI("[ActivityLifecycle] -> onResume");
        invoke_callback(kCbOnResume, cpu::Args().u32v(g_act_ptr));
        g_state = ActivityState::Resumed;
    }
}

void notify_native_window_created(gNativeWindow window) {
    ZLOGI("[ActivityLifecycle] -> onNativeWindowCreated (window=0x%08x)", window);
    invoke_callback(kCbOnNativeWindowCreated, cpu::Args().u32v(g_act_ptr).u32v(window));
}

void notify_native_window_destroyed(gNativeWindow window) {
    ZLOGI("[ActivityLifecycle] -> onNativeWindowDestroyed (window=0x%08x)", window);
    invoke_callback(kCbOnNativeWindowDestroyed, cpu::Args().u32v(g_act_ptr).u32v(window));
}

void notify_input_queue_created(gInputQueue queue) {
    ZLOGI("[ActivityLifecycle] -> onInputQueueCreated (queue=0x%08x)", queue);
    invoke_callback(kCbOnInputQueueCreated, cpu::Args().u32v(g_act_ptr).u32v(queue));
}

void notify_input_queue_destroyed(gInputQueue queue) {
    ZLOGI("[ActivityLifecycle] -> onInputQueueDestroyed (queue=0x%08x)", queue);
    invoke_callback(kCbOnInputQueueDestroyed, cpu::Args().u32v(g_act_ptr).u32v(queue));
}

void notify_window_focus_changed(bool has_focus) {
    ZLOGD("[ActivityLifecycle] -> onWindowFocusChanged (focus=%d)", has_focus);
    invoke_callback(kCbOnWindowFocusChanged, cpu::Args().u32v(g_act_ptr).i32v(has_focus ? 1 : 0));
}

void notify_pause() {
    if (g_state == ActivityState::Resumed) {
        ZLOGI("[ActivityLifecycle] -> onPause");
        invoke_callback(kCbOnPause, cpu::Args().u32v(g_act_ptr));
        g_state = ActivityState::Paused;
    }
}

void notify_stop() {
    if (g_state == ActivityState::Paused || g_state == ActivityState::Started) {
        ZLOGI("[ActivityLifecycle] -> onStop");
        invoke_callback(kCbOnStop, cpu::Args().u32v(g_act_ptr));
        g_state = ActivityState::Stopped;
    }
}

void notify_destroy() {
    ZLOGI("[ActivityLifecycle] -> onDestroy");
    invoke_callback(kCbOnDestroy, cpu::Args().u32v(g_act_ptr));
    g_state = ActivityState::Destroyed;
}

ActivityState get_current_state() {
    return g_state;
}

bool is_running() {
    return g_state == ActivityState::Started || g_state == ActivityState::Resumed;
}

} // namespace zb::android
