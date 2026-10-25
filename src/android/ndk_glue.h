// Windows emulation of the Android NDK native APIs the game imports:
// AAssetManager, AConfiguration, AInputQueue/AInputEvent, ALooper,
// ANativeWindow, ASensor*, plus the POSIX pipe/read/write/close overrides the
// guest's android_native_app_glue relies on.
#pragma once

#include <string>

#include "core/common.h"

namespace zb::android::ndk {

void register_all();

// Opaque guest handles used by the activity bootstrap and lifecycle driver.
u32 window_handle();
u32 input_queue_handle();
u32 asset_manager_handle();

// Directory the AAssetManager reads from (the APK's assets/ folder, extracted).
void set_asset_root(const std::string& dir);

// Device locale reported by AConfiguration (two-letter language and country codes, e.g. "fr", "FR").
void set_locale(const std::string& language, const std::string& country);

}  // namespace zb::android::ndk
