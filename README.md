# Run Sackboy! Run! — Native Windows PC Port (`x86_64`)

Standalone native Windows (`x86_64`) port of **Run Sackboy! Run!** (v1.0.4, Sony Computer Entertainment / Firesprite), powered by **Dynarmic** (`A32 -> x86_64` JIT), an identity-mapped 4 GB 32-bit guest address space (`VirtualAlloc`), Windows `WaitOnAddress` futex synchronization, and High-Level Emulation (HLE) of Bionic libc, Android NDK (`ANativeActivity`, `ALooper`, `AInputQueue`, `AAssetManager`), JNI/Dalvik, OpenGL ES 2.0 -> Desktop OpenGL (WGL), OpenSL ES -> WinMM (`waveOut`) and the Java `MediaPlayer` music (MP3 decoded with Media Foundation).

## Status

| Area | State |
|---|---|
| Boot, loading, front end, tutorial run | Works (first launch and existing profiles) |
| Frame rate | 60 FPS after loading. Vsync is only used on 60/120/180 Hz displays; elsewhere (48, 75, 144 Hz…) a frame limiter paces the game at 60 Hz, since it steps its logic once per frame |
| Sound effects (OpenSL ES) and music (6 MP3 tracks) | Work; the music pauses when the window loses focus |
| Keyboard, mouse, XInput gamepad | Implemented; to be checked by hand on full runs |
| Window, full screen | The game renders at a fixed size (1280x720 by default) and is scaled to fill the window with its aspect ratio kept (black bars only if the window's shape differs). `F11` or `Alt+Enter` toggles borderless full screen; `--fullscreen` starts in it at the monitor's native resolution. The process is DPI aware, so display scaling (125 %…) no longer blurs the image |
| Losing the focus | The game pauses (as on Android when the app goes to the background) and resumes when its window is clicked again |
| Saves (`save\rsbra.sav`, `rsbrb.sav`) | Written and read back across sessions |
| Online services (PSN, Facebook, ads, store downloads) | Offline: no connection is reported, rewarded ads never pay out, store purchases are granted locally |
| Java-side text (`JNI.RenderText`) | Not implemented (the UI uses the game's own bitmap fonts; a warning is logged if the game ever asks for it) |

## Getting started

**Just want to play?** Download `sbr64_pc-…-windows-x64.zip` from the [Releases](https://github.com/KLM-corporation/sbr64_pc/releases), unzip it, double-click `importer_le_jeu.bat` (it asks for the game's APK and OBB folder), then `run_game.bat`. No toolchain needed.

The repository holds the port only. The game itself (ARM library, archives, fonts, music) comes from
your own copy of **Run Sackboy! Run!** for Android, v1.0.4: see [`data/README.md`](data/README.md).

```powershell
# 1. Toolchain (once)
winget install MartinStorsjo.LLVM-MinGW.UCRT Kitware.CMake Ninja-build.Ninja

# 2. Game files from your APK and OBB files (once)
powershell -NoProfile -ExecutionPolicy Bypass -File tools\import_game_data.ps1 -Apk <game.apk> -ObbDir <folder with the OBBs>

# 3. Build (sbr64_pc.exe is copied next to build.ps1)
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1

# 4. Play
.\run_game.bat
```

## Folder layout

| Path | Content |
|---|---|
| `src\` | The port: `core\` (JIT, memory, sync, ELF loader), `android\` (Bionic/NDK/JNI emulation, EGL/GLES bridge, runner), `audio\`, `platform\` (window, input) |
| `third_party\` | Dynarmic (with its externals), the Boost headers it uses, dlmalloc |
| `tests\` | C++ unit tests and the Python E2E suite (`tests\e2e`) |
| `tools\` | `import_game_data.ps1` (also launched by `importer_le_jeu.bat`), `bmp2png.py` (screenshots), `got_symbol.py` and `plt_calls.py` (reverse-engineering helpers for the game library) |
| `data\` | Game files, imported (not versioned) |
| `save\` | Player profile (not versioned) |
| `sbr64_pc.exe`, `build\` | Build output (not versioned) |

At run time every path defaults to the folder holding the executable, or its parent when it runs from `build\`.

## Running the Game

```powershell
.\run_game.bat
```

### Command-Line Options

| Option | Description | Default |
|---|---|---|
| `--so <path>` | Path to `libRSBR_arm32.so` | `data\lib\libRSBR_arm32.so` |
| `--assets <dir>` | Path to extracted APK `assets/` | `data\assets` |
| `--obb <dir>` | Folder containing the `main.25` & `patch.42` OBB files | `data\obb` |
| `--music <dir>` | Folder containing the background music MP3s | `data\music` |
| `--save <dir>` | Folder where the save files are kept | `save` |
| `--size <WxH>` | Resolution the game renders at (the window scales it) | `1280x720` |
| `--lang <ll-CC>` | Game language (`fr-FR`, `en-US`, `de-DE`, …) | Windows user language |
| `--fullscreen` | Start in borderless full screen (`F11` / `Alt+Enter` toggle it) | off |
| `--vsync <auto\|on\|off>` | Swap interval (see Status) | `auto` |
| `--max-seconds <n>` | Exit cleanly after `<n>` seconds | unlimited |
| `--frames <n>` | Exit once `<n>` frames have been presented (loading time does not count) | unlimited |
| `--headless` | Hide the window (automated runs) | off |
| `--test-mode` | Core self-test and exit | off |
| `--verbose` | Debug logging, including frame-rate statistics, HLE call rates and file opens | off |
| `--version`, `-v` / `--help`, `-h` | Version / usage | — |

Diagnostics (for development):

| Option | Description |
|---|---|
| `--screenshot <file.bmp> --screenshot-frame <n[,n…]>` | Save the back buffer at these presented frames (`file_<n>.bmp` when several); `python tools\bmp2png.py` converts it |
| `--script "<frame>:key=space*12;<frame>:click=640,360"` | Scripted input through the window's message queue (`*N` = frames held; keys: `space enter esc up down left right`). Also `fullscreen=toggle`, `size=1600x700`, `focus=lost` / `focus=gained`; `@<ms>:` instead of `<frame>:` triggers on time (frames stop while the game is paused) |
| `--trace-callers <imports>` | With `--verbose`: which game functions (heuristic call chain) call these imports, e.g. `glDrawArrays,cosf` |
| `--watch <symbols>` | With `--verbose`: print exported game variables every 2 s (`got:<offset>` reads through a GOT slot) |

## Controls

| Action | Keyboard | Mouse | XInput Gamepad (Xbox) |
|---|---|---|---|
| **Jump / Tap** | `Space` or `Enter` (hold for higher jump) | Left-click / hold | `A` button (hold for higher jump) |
| **Swipe Up (Jump)** | `Up Arrow` / `W` / `Z` | Drag upward | `D-Pad Up` |
| **Swipe Down (Roll)** | `Down Arrow` / `S` | Drag downward | `D-Pad Down` / `B` |
| **Swipe Right (Dash)** | `Right Arrow` / `D` | Drag right | `D-Pad Right` / `X` / `RB` |
| **Swipe Left** | `Left Arrow` / `A` / `Q` | Drag left | `D-Pad Left` / `LB` |
| **Back / Pause** | `Escape` / `Backspace` | — | `Start` / `Back` |
| **Full screen** | `F11` or `Alt+Enter` | — | — |

## Tests

```powershell
# Tiers 1-4: 276 contract and simulation tests (they check formats and specifications, not the running game)
python tests\e2e\run_tests.py

# Tier 5: live runs of the real game, headless (about 1 minute; longer on battery saver)
python tests\e2e\run_tests.py --tier 5
```

C++ unit tests (in `build\`): `test_mem_win32`, `test_sync_win32`, `test_cpu_standalone`, `test_cpu_adversarial`, `test_elf_loader <libRSBR_arm32.so>`, `test_bionic_hle`, `test_jni_lifecycle`, `test_music_player <data\music>`.

## Legal

Run Sackboy! Run! and its files are the property of Sony Interactive Entertainment. This repository contains
only the port (an execution environment for the original Android build) and never the game's files, which
you must take from your own copy.
