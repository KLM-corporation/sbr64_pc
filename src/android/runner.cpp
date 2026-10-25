#include "android/runner.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "android/bionic_hle.h"
#include "android/ndk_glue.h"
#include "android/jni_env.h"
#include "android/native_activity.h"
#include "audio/music_player.h"
#include "core/common.h"
#include "core/cpu.h"
#include "core/elf_loader.h"
#include "core/mem_win32.h"
#include "core/sync_win32.h"
#include "platform/win_window.h"

namespace zb::android {
namespace {

// One scripted input: presses at `frame`, releases `hold` frames later (window actions happen once).
struct ScriptedInput {
    enum Kind { Key, Click, Fullscreen, Resize, Focus } kind = Key;
    unsigned long long frame = 0;  // presented frame, or milliseconds since start with an '@' prefix
    bool timed = false;
    unsigned long long hold = 6;
    unsigned vk = 0;
    int x = 0, y = 0;      // click position or new client size
    bool pressed = false;
    bool done = false;
};

unsigned vk_for(const std::string& name) {
    if (name == "space") return VK_SPACE;
    if (name == "enter") return VK_RETURN;
    if (name == "esc") return VK_ESCAPE;
    if (name == "up") return VK_UP;
    if (name == "down") return VK_DOWN;
    if (name == "left") return VK_LEFT;
    if (name == "right") return VK_RIGHT;
    return 0;
}

bool parse_script(const std::string& text, std::vector<ScriptedInput>* out) {
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(';', at);
        if (end == std::string::npos) end = text.size();
        std::string item = text.substr(at, end - at);
        at = end + 1;
        if (item.empty()) continue;
        ScriptedInput in;
        const size_t colon = item.find(':');
        const size_t eq = item.find('=');
        if (colon == std::string::npos || eq == std::string::npos || eq < colon) return false;
        in.timed = item[0] == '@';
        in.frame = std::strtoull(item.c_str() + (in.timed ? 1 : 0), nullptr, 10);
        const std::string kind = item.substr(colon + 1, eq - colon - 1);
        std::string arg = item.substr(eq + 1);
        if (const size_t star = arg.find('*'); star != std::string::npos) {
            in.hold = std::strtoull(arg.c_str() + star + 1, nullptr, 10);
            arg.resize(star);
        }
        if (kind == "key") {
            in.vk = vk_for(arg);
            if (!in.vk) return false;
        } else if (kind == "click") {
            in.kind = ScriptedInput::Click;
            if (std::sscanf(arg.c_str(), "%d,%d", &in.x, &in.y) != 2) return false;
        } else if (kind == "fullscreen") {
            in.kind = ScriptedInput::Fullscreen;
        } else if (kind == "focus") {
            in.kind = ScriptedInput::Focus;
            if (arg != "lost" && arg != "gained") return false;
            in.x = arg == "gained";
        } else if (kind == "size") {
            in.kind = ScriptedInput::Resize;
            if (std::sscanf(arg.c_str(), "%dx%d", &in.x, &in.y) != 2 || in.x <= 0 || in.y <= 0) return false;
        } else {
            return false;
        }
        out->push_back(in);
    }
    return true;
}

void run_script(std::vector<ScriptedInput>& script, unsigned long long elapsed_ms) {
    const unsigned long long now = presented_frames();
    for (ScriptedInput& in : script) {
        if (in.done) continue;
        if (!in.pressed && (in.timed ? elapsed_ms : now) >= in.frame) {
            if (in.timed) in.frame = now;  // the hold below counts frames from here
            in.pressed = true;
            static const char* const names[] = {"key down", "click down", "toggle full screen", "resize", "focus"};
            ZLOGI("script: frame %llu: %s", now, names[in.kind]);
            switch (in.kind) {
                case ScriptedInput::Key: win::post_key(in.vk, true); break;
                case ScriptedInput::Click: win::post_mouse(in.x, in.y, true); break;
                case ScriptedInput::Fullscreen: win::toggle_fullscreen(); in.done = true; break;
                case ScriptedInput::Resize: win::set_client_size(in.x, in.y); in.done = true; break;
                case ScriptedInput::Focus: win::simulate_focus(in.x != 0); in.done = true; break;
            }
        } else if (in.pressed && now >= in.frame + in.hold) {
            in.done = true;
            if (in.kind == ScriptedInput::Key) win::post_key(in.vk, false);
            else win::post_mouse(in.x, in.y, false);
        }
    }
}

// --watch: prints the first bytes of exported game variables (comma-separated mangled names).
void log_watched(const std::string& names) {
    std::string line;
    size_t at = 0;
    while (at < names.size()) {
        size_t end = names.find(',', at);
        if (end == std::string::npos) end = names.size();
        const std::string name = names.substr(at, end - at);
        at = end + 1;
        char buf[160];
        gaddr a = 0;
        if (name.rfind("got:", 0) == 0) {  // got:<offset> reads through the GOT slot at bias + offset
            if (elf::Module* m = elf::find_module("libRSBR_arm32.so"))
                a = rd<u32>(m->bias + static_cast<gaddr>(std::strtoul(name.c_str() + 4, nullptr, 16)));
        } else {
            a = elf::resolve_symbol(name.c_str());
        }
        if (a) {
            std::snprintf(buf, sizeof buf, " %s=%02x %02x %02x %02x", name.c_str(), rd<u8>(a), rd<u8>(a + 1),
                          rd<u8>(a + 2), rd<u8>(a + 3));
        } else {
            std::snprintf(buf, sizeof buf, " %s=?", name.c_str());
        }
        line += buf;
    }
    if (!line.empty()) ZLOGI("watch:%s", line.c_str());
}

// Two-letter language and country the game is told the device uses ("fr-FR" -> "fr", "FR").
void apply_locale(std::string locale) {
    if (locale.empty()) {
        wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
        if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0)
            for (const wchar_t* c = name; *c; ++c) locale += static_cast<char>(*c);
    }
    std::string lang = locale.substr(0, 2), country;
    for (char& c : lang) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (const size_t dash = locale.find_first_of("-_"); dash != std::string::npos) country = locale.substr(dash + 1, 2);
    for (char& c : country) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (lang.size() != 2) lang = "en";
    if (country.size() != 2) country = lang == "en" ? "US" : lang == "fr" ? "FR" : "US";
    ndk::set_locale(lang, country);
    jni::OfflineConfig cfg = jni::get_offline_config();
    cfg.language = lang;
    cfg.country = country;
    jni::set_offline_config(cfg);
    ZLOGI("locale: %s-%s", lang.c_str(), country.c_str());
}

}  // namespace

RunOptions default_options() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    dir.resize(dir.find_last_of("\\/") == std::string::npos ? 0 : dir.find_last_of("\\/"));
    auto has_data = [](const std::string& d) {
        return GetFileAttributesA((d + "\\data").c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    if (!has_data(dir)) {
        const size_t up = dir.find_last_of("\\/");
        if (up != std::string::npos && has_data(dir.substr(0, up))) dir.resize(up);
    }
    RunOptions o;
    for (std::string* p : {&o.so_path, &o.asset_root, &o.obb_dir, &o.save_dir, &o.music_dir}) *p = dir + "\\" + *p;
    return o;
}

int run_game(const RunOptions& o) {
    std::vector<ScriptedInput> script;
    if (!o.input_script.empty() && !parse_script(o.input_script, &script)) {
        ZLOGE("invalid --script: %s", o.input_script.c_str());
        return 1;
    }

    CreateDirectoryA(o.save_dir.c_str(), nullptr);

    // Full screen without an explicit --size: render at the monitor's resolution, so nothing is scaled.
    int render_w = o.width, render_h = o.height;
    if (o.fullscreen && !o.size_set) win::primary_monitor_size(&render_w, &render_h);
    if (!win::start(render_w, render_h, "Run Sackboy! Run!", o.fullscreen)) {
        ZLOGE("could not create the game window");
        return 1;
    }
    if (o.headless && win::hwnd()) {
        ShowWindow(static_cast<HWND>(win::hwnd()), SW_HIDE);
    }

    sync::init();
    mem::MemoryConfig mc;
    mc.enable_identity_map = true;
    mem::init(mc);
    cpu::init();

    bionic::set_save_dir(o.save_dir);
    bionic::set_obb_dir(o.obb_dir);
    bionic::register_all();
    ndk::set_asset_root(o.asset_root);
    apply_locale(o.locale);
    ndk::register_all();
    register_egl_gles();
    set_screenshot(o.screenshot_path, o.screenshot_frames);
    set_vsync_mode(o.vsync);
    cpu::trace_callers(o.trace_callers);
    register_opensl();
    audio::music::set_music_dir(o.music_dir);
    audio::music::set_output_starter(&start_audio_mixer);

    elf::Module* m = elf::load_file(o.so_path, nullptr);
    if (!m) {
        ZLOGE("could not load %s", o.so_path.c_str());
        return 1;
    }
    // elf::load_file installed the symbolizer (exported functions of the game library).
    cpu::set_code_range(m->rx_lo, m->rx_hi);
    ZLOGI("libRSBR loaded at 0x%08x, %zu imports without an implementation", m->bias, m->missing.size());
    elf::run_initializers(m);

    gaddr on_create = elf::resolve_symbol("ANativeActivity_onCreate");
    if (!on_create) {
        ZLOGE("ANativeActivity_onCreate not found in the game library");
        return 1;
    }
    ActivityConfig cfg;
    cfg.internal_data_path = o.save_dir;
    cfg.external_data_path = o.save_dir;
    cfg.obb_path = o.obb_dir;
    cfg.sdk_version = 24;
    cfg.entry_onCreate = on_create;
    cfg.asset_manager = ndk::asset_manager_handle();
    android::init(cfg);
    if (!create_activity()) {
        ZLOGE("ANativeActivity_onCreate failed");
        return 1;
    }

    notify_start();
    notify_resume();
    notify_native_window_created(ndk::window_handle());
    notify_input_queue_created(ndk::input_queue_handle());
    notify_window_focus_changed(true);
    ZLOGI("game started");

    const auto t0 = std::chrono::steady_clock::now();
    bool focus = true;
    auto last_stats = t0;
    while (!win::close_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        run_script(script, static_cast<unsigned long long>(
                               std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
                                   .count()));
        if (g_debug && std::chrono::steady_clock::now() - last_stats >= std::chrono::seconds(2)) {
            const auto now = std::chrono::steady_clock::now();
            cpu::log_call_stats(std::chrono::duration<double>(now - last_stats).count());
            last_stats = now;
            log_watched(o.watch);
        }
        if (!o.headless) {
            bool f = win::focused();
            if (f != focus) {
                focus = f;
                ZLOGI("window focus %s (frame %llu)", f ? "gained" : "lost", presented_frames());
                audio::music::set_suspended(!f);
                notify_window_focus_changed(f);
                ZLOGD("window focus change delivered");
            }
        }
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        if (o.max_seconds > 0 && elapsed >= std::chrono::seconds(o.max_seconds)) break;
        // Counted in presented frames, so loading time (slow machines, battery saver) does not cut a run short.
        if (o.max_frames > 0 && presented_frames() >= static_cast<unsigned long long>(o.max_frames)) break;
    }

    ZLOGI("closing the game");
    notify_pause();
    notify_input_queue_destroyed(ndk::input_queue_handle());
    notify_native_window_destroyed(ndk::window_handle());
    notify_stop();
    notify_destroy();
    // The game's engine threads never end by themselves: leave the process.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::fflush(stdout);
    std::fflush(stderr);
    ExitProcess(0);
}

}  // namespace zb::android
