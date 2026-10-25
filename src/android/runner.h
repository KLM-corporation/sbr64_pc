// Drives the game end to end on Windows: window, GL, audio, ELF load and the Android activity lifecycle.
#pragma once

#include <string>
#include <vector>

namespace zb::android {

// Default paths are relative to the install folder: see default_options().
struct RunOptions {
    std::string so_path = "data\\lib\\libRSBR_arm32.so";
    std::string asset_root = "data\\assets";
    std::string obb_dir = "data\\obb";
    std::string save_dir = "save";
    std::string music_dir = "data\\music";
    int width = 1280;   // size the game renders at (the window scales it)
    int height = 720;
    bool size_set = false;    // --size given: otherwise full screen renders at the monitor's resolution
    bool fullscreen = false;  // borderless full screen at start-up (F11 / Alt+Enter toggle it)
    int max_seconds = 0;  // >0: close the window automatically after this many seconds (for automated runs)
    int max_frames = 0;   // >0: close automatically once this many frames have been presented
    bool headless = false;
    std::string locale;  // "fr-FR", "en-US"...; empty: the Windows user's locale
    std::string screenshot_path;  // non-empty: save the back buffer as a BMP at these presented frames
    std::vector<int> screenshot_frames{300};
    int vsync = -1;  // -1 auto, 0 off, 1 on
    // Scripted input for automated runs: "frame:key=space*10;frame:click=640,360*6" (frame = presented frame,
    // *N = frames held, default 6). Keys: space enter esc up down left right. Window actions:
    // "frame:fullscreen=toggle" and "frame:size=1600x700" (client size, leaves full screen).
    std::string input_script;
    std::string trace_callers;  // --verbose diagnostics: thunk names whose guest callers are reported
    std::string watch;          // --verbose diagnostics: exported game variables printed every 2 s
};

// Options whose paths point into the install folder: the folder holding the executable, or its parent
// when the executable runs from build\ (the folder that contains data\).
RunOptions default_options();

int run_game(const RunOptions& opts);

void register_egl_gles();
void set_screenshot(const std::string& path, const std::vector<int>& frames);
void set_vsync_mode(int mode);
unsigned long long presented_frames();  // frames handed to eglSwapBuffers so far
void register_opensl();
void start_audio_mixer();

}  // namespace zb::android
