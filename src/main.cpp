#include <iostream>
#include <string_view>
#include <vector>
#include <cstring>

#include "android/runner.h"
#include "core/common.h"
#include "core/mem_win32.h"
#include "core/sync_win32.h"
#include "core/cpu.h"

int main(int argc, char* argv[]) {
    bool test_mode = false;
    zb::android::RunOptions run = zb::android::default_options();

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        auto next = [&](std::string& dst) -> bool {
            if (i + 1 < argc) {
                dst = argv[++i];
                return true;
            }
            std::cerr << "[sbr64_pc] Missing value for option: " << arg << "\n";
            return false;
        };
        if (arg == "--help" || arg == "-h") {
            std::cout << "Run Sackboy! Run! Native PC Port (sbr64_pc)\n";
            std::cout << "Usage: sbr64_pc.exe [options]\n";
            std::cout << "Options:\n";
            std::cout << "  --help, -h         Show help message\n";
            std::cout << "  --version, -v      Show version information\n";
            std::cout << "  --test-mode        Run self-tests and exit\n";
            std::cout << "  --headless         Run without showing the desktop window\n";
            std::cout << "  --frames <n>       Close automatically once n frames have been presented\n";
            std::cout << "  --so <path>        ARM32 game library (default " << run.so_path << ")\n";
            std::cout << "  --assets <dir>     Extracted APK assets directory (default " << run.asset_root << ")\n";
            std::cout << "  --obb <dir>        Directory with the OBB files (default " << run.obb_dir << ")\n";
            std::cout << "  --save <dir>       Save data directory (default " << run.save_dir << ")\n";
            std::cout << "  --music <dir>      Folder with the background music MP3s (default " << run.music_dir << ")\n";
            std::cout << "  --size <W>x<H>     Resolution the game renders at (default 1280x720; the window scales it)\n";
            std::cout << "  --lang <ll-CC>     Game language, e.g. fr-FR, en-US, de-DE (default: Windows language)\n";
            std::cout << "  --fullscreen       Start in borderless full screen (F11 or Alt+Enter toggle it)\n";
            std::cout << "  --max-seconds <n>  Close automatically after n seconds\n";
            std::cout << "  --vsync <auto|on|off>  Swap interval (auto: only on 60/120/180 Hz displays)\n";
            std::cout << "  --trace-callers <names>  With --verbose: report the game functions calling these imports\n";
            std::cout << "  --watch <symbols>  With --verbose: print these game variables every 2 s\n";
            std::cout << "  --script <s>       Scripted input, e.g. \"600:key=space*10;900:click=640,360\"\n";
            std::cout << "  --screenshot <bmp> Save the back buffer to a BMP file (see --screenshot-frame)\n";
            std::cout << "  --screenshot-frame <n[,n...]>  Presented frame(s) to capture (default 300)\n";
            std::cout << "  --verbose          Detailed logging (includes frame-rate statistics)\n";
            return 0;
        } else if (arg == "--version" || arg == "-v") {
            std::cout << "sbr64_pc version 1.0.0\n";
            return 0;
        } else if (arg == "--test-mode") {
            test_mode = true;
        } else if (arg == "--headless") {
            run.headless = true;
        } else if (arg == "--lang") {
            if (!next(run.locale)) return 1;
        } else if (arg == "--fullscreen") {
            run.fullscreen = true;
        } else if (arg == "--frames") {
            std::string s;
            if (!next(s)) return 1;
            run.max_frames = std::atoi(s.c_str());
            if (run.max_frames <= 0) {
                std::cerr << "[sbr64_pc] Invalid frame count: " << s << "\n";
                return 1;
            }
        } else if (arg == "--so") {
            if (!next(run.so_path)) return 1;
        } else if (arg == "--assets") {
            if (!next(run.asset_root)) return 1;
        } else if (arg == "--obb") {
            if (!next(run.obb_dir)) return 1;
        } else if (arg == "--save") {
            if (!next(run.save_dir)) return 1;
        } else if (arg == "--music") {
            if (!next(run.music_dir)) return 1;
        } else if (arg == "--size") {
            std::string s;
            if (!next(s)) return 1;
            int w = 0, h = 0;
            if (std::sscanf(s.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                run.width = w;
                run.height = h;
                run.size_set = true;
            } else {
                std::cerr << "[sbr64_pc] Invalid window size: " << s << "\n";
                return 1;
            }
        } else if (arg == "--max-seconds") {
            std::string s;
            if (!next(s)) return 1;
            run.max_seconds = std::atoi(s.c_str());
            if (run.max_seconds <= 0) {
                std::cerr << "[sbr64_pc] Invalid max-seconds value: " << s << "\n";
                return 1;
            }
        } else if (arg == "--vsync") {
            std::string s;
            if (!next(s)) return 1;
            if (s == "auto") run.vsync = -1;
            else if (s == "on") run.vsync = 1;
            else if (s == "off") run.vsync = 0;
            else {
                std::cerr << "[sbr64_pc] --vsync expects auto, on or off: " << s << "\n";
                return 1;
            }
        } else if (arg == "--trace-callers") {
            if (!next(run.trace_callers)) return 1;
        } else if (arg == "--watch") {
            if (!next(run.watch)) return 1;
        } else if (arg == "--script") {
            if (!next(run.input_script)) return 1;
        } else if (arg == "--screenshot") {
            if (!next(run.screenshot_path)) return 1;
        } else if (arg == "--screenshot-frame") {
            std::string s;
            if (!next(s)) return 1;
            run.screenshot_frames.clear();
            for (size_t at = 0; at < s.size();) {
                const int frame = std::atoi(s.c_str() + at);
                if (frame <= 0) {
                    std::cerr << "[sbr64_pc] Invalid screenshot frame list: " << s << "\n";
                    return 1;
                }
                run.screenshot_frames.push_back(frame);
                const size_t comma = s.find(',', at);
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
        } else if (arg == "--verbose") {
            zb::g_debug = true;
        } else {
            std::cerr << "[sbr64_pc] Unknown option: " << arg << "\n";
            return 1;
        }
    }

    if (!test_mode) {
        return zb::android::run_game(run);
    }

    std::cout << "[sbr64_pc] Initializing Win32 Core Systems...\n";
    zb::sync::init();

    zb::mem::MemoryConfig mem_cfg;
    mem_cfg.enable_identity_map = true;
    zb::mem::init(mem_cfg);

    zb::cpu::Config cpu_cfg;
    cpu_cfg.fastmem = true;
    cpu_cfg.fast_dispatch = true;
    zb::cpu::init(cpu_cfg);

    std::cout << "[sbr64_pc] Core subsystems initialized successfully.\n";

    std::cout << "[sbr64_pc] Running quick engine self-test...\n";
    zb::gaddr p = zb::mem::gmalloc(128);
    if (!p) {
        std::cerr << "[sbr64_pc] Self-test failed: gmalloc returned NULL\n";
        return 1;
    }
    std::memset(zb::hp(p), 0x5A, 128);
    zb::mem::gfree(p);
    std::cout << "[sbr64_pc] Self-test PASSED.\n";
    zb::mem::shutdown();
    return 0;
}
