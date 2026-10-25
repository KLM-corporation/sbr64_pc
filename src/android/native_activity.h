#pragma once

#include <cstdint>
#include <string>

#include "core/common.h"
#include "core/cpu.h"
#include "jni_env.h"

namespace zb::android {

// Forward declarations of Android opaque handles in guest memory
using gNativeActivity = gaddr;
using gNativeWindow   = gaddr;
using gInputQueue     = gaddr;
using gAssetManager   = gaddr;

// Android Activity lifecycle states
enum class ActivityState {
    Uninitialized,
    Created,
    Started,
    Resumed,
    Paused,
    Stopped,
    Destroyed
};

// Android Glue Command constants (sent through android_app cmd pipe)
enum AppCmd {
    APP_CMD_INPUT_CHANGED = 0,
    APP_CMD_INIT_WINDOW   = 1,
    APP_CMD_TERM_WINDOW   = 2,
    APP_CMD_WINDOW_RESIZED = 3,
    APP_CMD_WINDOW_REDRAW_NEEDED = 4,
    APP_CMD_CONTENT_RECT_CHANGED = 5,
    APP_CMD_GAINED_FOCUS  = 6,
    APP_CMD_LOST_FOCUS    = 7,
    APP_CMD_CONFIG_CHANGED = 8,
    APP_CMD_LOW_MEMORY    = 9,
    APP_CMD_START         = 10,
    APP_CMD_RESUME        = 11,
    APP_CMD_SAVE_STATE    = 12,
    APP_CMD_PAUSE         = 13,
    APP_CMD_STOP          = 14,
    APP_CMD_DESTROY       = 15
};

struct ActivityConfig {
    std::string internal_data_path = ".\\save\\";
    std::string external_data_path = ".\\save\\";
    std::string obb_path           = "obb/";
    int32_t sdk_version            = 19; // Android 4.4 KitKat
    gaddr entry_onCreate           = 0x001bf5c8;
    gaddr entry_androidMain        = 0x001b9d10;
    u32   asset_manager            = 0;  // opaque AAssetManager handle written into ANativeActivity::assetManager
};

// -----------------------------------------------------------------------------
// Subsystem Lifecycle
// -----------------------------------------------------------------------------
void init(const ActivityConfig& cfg = {});
void shutdown();

// Bootstrap into ANativeActivity_onCreate
bool create_activity(void* saved_state = nullptr, size_t saved_state_size = 0);

// Guest pointers
gNativeActivity get_guest_activity();
gaddr get_guest_callbacks();
gaddr get_guest_app_instance(); // Pointer to struct android_app

// Lifecycle event triggers (Host -> Guest Callbacks)
void notify_start();
void notify_resume();
void notify_native_window_created(gNativeWindow window);
void notify_native_window_destroyed(gNativeWindow window);
void notify_input_queue_created(gInputQueue queue);
void notify_input_queue_destroyed(gInputQueue queue);
void notify_window_focus_changed(bool has_focus);
void notify_pause();
void notify_stop();
void notify_destroy();

// State queries
ActivityState get_current_state();
bool is_running();

} // namespace zb::android
