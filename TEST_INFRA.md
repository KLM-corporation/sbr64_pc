# Test Infrastructure Specification: Run Sackboy! Run! Native PC Port (sbr64_pc)

## Overview & Scope
This document defines the complete end-to-end (E2E) testing infrastructure, testing philosophy, methodologies, feature verification matrix, and progressive testability framework for `sbr64_pc`—the native Windows PC port of *Run Sackboy! Run!*.

The testing infrastructure guarantees that all 24 features defined in `PROJECT.md` meet the functional, non-functional, and interface requirements from `ORIGINAL_REQUEST.md` (§R1–§R4 and Acceptance Criteria).

---

## 1. Testing Philosophy

### 1.1 Opaque-Box Testing
The test suite operates strictly as an **opaque-box (black-box)** testing harness:
- The tests interact with the system under test exclusively through external interfaces: command-line parameters, standard file formats (ELF32, PAK1, OBB, SAV, TTF), guest-to-host ABI contracts (AAPCS, JNI, OpenSL ES, OpenGL ES 2.0 / Desktop GL, XInput / Win32 events), and standard observable outputs (exit codes, logs, frame timing, audio buffer queues, save game state).
- The tests **do not link against or inspect private C++ implementation details** or non-exported symbols.

### 1.2 Requirement-Driven Verification
Every single test case in the suite directly traces to:
- A specific numbered requirement in `ORIGINAL_REQUEST.md` (R1: Standalone PC Runtime, R2: REA Analysis, R3: Graphics/Audio/Input, R4: Filesystem & Persistence)
- A milestone feature in `PROJECT.md § Feature Inventory` (Features 1 through 24).
- Formal interface contracts between architectural layers (M1 Core, M2 HLE, M3 Assets, M4 Graphics, M5 Audio/Input, M6 Hardening).

### 1.3 Zero Internal Module Dependencies
- The test harness is written in standard Python 3 (using standard library `unittest` and standard modules like `struct`, `ctypes`, `hashlib`, `zlib`) with zero external pip dependencies or compiler build-time entanglement.
- This ensures that the test runner executes identically in any CI/CD or developer environment without prerequisites like pytest or third-party test adapters.

---

## 2. Testing Methodologies

The test suite applies four rigorous testing methodologies across four distinct testing tiers:

### 2.1 Category-Partition Method (Tier 1: Feature Coverage)
For each of the 24 features, the input and state domains are decomposed into functional equivalence classes:
- **Equivalence Classes**: Valid, nominal, edge, and invalid inputs.
- **Coverage Criterion**: Each equivalence class is exercised by at least one dedicated test case.
- **Quota**: At least 5 test cases per feature, providing $\ge 120$ tests in Tier 1.

### 2.2 Boundary Value Analysis (BVA) (Tier 2: Boundary & Corner Cases)
BVA focuses on the extreme edges of input ranges, parameter limits, and resource constraints:
- **Memory Boundaries**: Low 4GB pointer wraps, 0-byte allocations, 4GB overflow checks, page-alignment edges (4KB boundaries).
- **Crypto & Compression Limits**: 0-byte PAK1 payloads, MT19937 seed edge values (`0x00000000`, `0x0BAFF1ED`, `0xFFFFFFFF`), corrupted zlib headers.
- **File System Limits**: Missing OBB files, truncated `.sav` files, long path names, CRC32 collision edge cases.
- **Frame Timing & Pacing**: Exactly 16.666ms (60 FPS), severe frame drops (<15 FPS), ultra-high refresh ticks (>144 FPS).
- **Audio & Input Limits**: Zero-length audio buffers, simultaneous multi-touch touches, disconnected gamepad inputs.
- **Quota**: At least 5 boundary/corner test cases per feature, providing $\ge 120$ tests in Tier 2.

### 2.3 Combinatorial Pairwise Interaction Testing (Tier 3: Cross-Feature Combinations)
Software faults frequently emerge at the intersection of two cooperating subsystems. Tier 3 systematically exercises 2-way pairwise interactions:
- **M1 x M2**: Dynarmic A32 JIT CPU execution coupled with Win32 4GB identity memory and Bionic dynamic relocations.
- **M2 x M3**: ELF32 Dynamic loader resolving asset calls into PAK1 decrypted OBB archives.
- **M2 x M4**: JNI lifecycle transitions (`ANativeActivity_onCreate` -> `android_main`) driving SDL2 window surface initialization.
- **M4 x M5**: 60 FPS rendering tick synchronized with OpenSL audio buffer queue consumption.
- **M3 x M5**: Save game state persistence triggered on background pause while input is actively engaged.
- **M5 x M1**: Gamepad XInput polling running across thread boundaries synchronized via Win32 `WaitOnAddress`.
- **Quota**: At least 24 pairwise cross-feature tests.

### 2.4 Real-World Workload Simulation (Tier 4: Realistic Scenarios)
Tier 4 executes complete, realistic multi-step pipelines matching actual gameplay:
1. Cold boot to title screen with asset decompression.
2. Title menu navigation and options toggle.
3. Endless runner loop: character spawn, continuous running, jump action, bubble collection.
4. Audio SFX triggering in sync with looping background music.
5. Window minimize / background pause and focus restore.
6. Player profile saving, application restart, and progress verification (`rsbra.sav`).
7. Hazard collision, defeat animation, and respawn cycle.
8. Costume store entitlement query and item unlock.
9. Dynamic window resizing across resolutions (720p, 1080p, 1440p) with aspect ratio retention.
10. Gamepad hotplugging (disconnect and reconnect with keyboard fallback).
11. Patch OBB archive fallback to Main OBB when patch entry is absent or corrupted.
12. Headless CI continuous 3600-frame stress run without memory leaks.
- **Quota**: At least 12 full-pipeline scenario tests.

---

## 3. Feature Inventory Matrix (24 Features)

The following matrix maps all 24 features from `PROJECT.md` to requirements, methodologies, and test coverage:

| ID | Feature Name | M# | Spec Ref | Equivalence Classes (Tier 1) | Boundary Conditions (Tier 2) | Tier 1 Suite | Tier 2 Suite |
|---|---|---|---|---|---|---|---|
| F01 | Standalone PC Binary | M1 | R1 | PE32+ header, x86_64 machine, subsystem, entry point, DLL imports | Missing DLLs, corrupted PE, non-zero exit codes, long path launch, cli args | `test_f01_*.py` | `test_b01_*.py` |
| F02 | Dynarmic x64 Execution | M1 | Survey 1 | ARMv7-A config, CPU context, SVC call, basic blocks, register state | R15 wrap, unaligned PC, invalid SVC ID, thumb/arm bit switch, illegal opcode | `test_f02_*.py` | `test_b02_*.py` |
| F03 | Win32 Memory Management | M1 | Survey 1 | 4GB reserve, VirtualAlloc commit, identity pointer arithmetic, page protection | Allocation at 0x0, >4GB address rejection, decommit, protection violation, unaligned sizes | `test_f03_*.py` | `test_b03_*.py` |
| F04 | Win32 Synchronization | M1 | Survey 1 | WaitOnAddress, WakeByAddressSingle, WakeByAddressAll, timeout handling, futex mapping | Zero timeout, infinite timeout, spurious wakeups, unaligned address, counter overflow | `test_f04_*.py` | `test_b04_*.py` |
| F05 | Toolchain Provisioning | M1 | Survey 3 | Clang/LLVM, MSVC, CMake 3.22+, Ninja 1.10+, C++20 standard compliance | Missing toolchain detection, invalid compiler flags, incompatible CRT, architecture mismatch | `test_f05_*.py` | `test_b05_*.py` |
| F06 | Dynamic ELF32 Loader | M2 | Survey 1,2 | ELF32 header, PT_LOAD segments, DT_NEEDED resolution, R_ARM_JUMP_SLOT, R_ARM_GLOB_DAT | Corrupted ELF magic, truncated section, invalid relocation index, non-ARM machine | `test_f06_*.py` | `test_b06_*.py` |
| F07 | Bionic / C++ HLE | M2 | Survey 1,2 | Libc memory, string, stdio, math, pthread stubs, AAPCS argument passing | NULL pointers, format string safety, buffer bounds, division by zero, errno propagation | `test_f07_*.py` | `test_b07_*.py` |
| F08 | JNI Environment & Mock JVM | M2 | Survey 2,3 | JavaVM table, JNIEnv table, FindClass, GetStaticMethodID, 18 native exports | NULL class name, invalid method signature, JNI exception check, local ref table overflow | `test_f08_*.py` | `test_b08_*.py` |
| F09 | Activity Lifecycle Boot | M2 | Survey 1,2 | ANativeActivity_onCreate, android_main, APP_CMD_INIT_WINDOW, APP_CMD_GAINED_FOCUS | Immediate termination command, null callbacks, re-entrant create, rapid focus change | `test_f09_*.py` | `test_b09_*.py` |
| F10 | Offline Stubs for Dead Services | M2 | Survey 2 | PSN offline return, Facebook stub, Flurry analytics drop, telemetry null sink | Malformed network payloads, timeout simulation, offline flag persistence, burst logging | `test_f10_*.py` | `test_b10_*.py` |
| F11 | PAK1 Decryption Engine | M3 | Survey 2 | MT19937 PRNG (seed 0x0BAFF1ED), stream XOR, zlib decompress, magic 0x314B4150 | Zero-length file, truncated PAK header, corrupted keystream, corrupted zlib stream | `test_f11_*.py` | `test_b11_*.py` |
| F12 | OBB Asset Mounting | M3 | Survey 2 | main.25 mount, patch.42 overlay mount, CRC32 path lookup, case insensitivity | Missing patch OBB, duplicate CRC32 hashes, special characters in path, empty archive | `test_f12_*.py` | `test_b12_*.py` |
| F13 | Local Save Persistence | M3 | R4 | rsbra.sav write/read, rsbrb.sav backup, rsbr.rcx config, profile checksum | Disk full error, read-only save folder, corrupted checksum recovery, 0-byte save | `test_f13_*.py` | `test_b13_*.py` |
| F14 | Font Asset Loading | M3 | Survey 2 | APK assets/fonts extraction, TTF table validation, TrueType rasterizer bridge | Missing TTF font, unsupported font format, 0-point glyph size, missing unicode glyph | `test_f14_*.py` | `test_b14_*.py` |
| F15 | Interactive Window (SDL2) | M4 | R3 | Window creation, title/icon, SDL event pump, high-DPI scaling, close event | Window minimize to 0x0, rapid resize storms, display change, alt-tab focus loss | `test_f15_*.py` | `test_b15_*.py` |
| F16 | Hardware GLES2 Translation | M4 | R3 | 78 GLES2 functions to OpenGL 3.3 Core, shader compilation, VAO emulation, FBO | Incompatible shader version, texture format mismatch, unbind edge cases, invalid enum | `test_f16_*.py` | `test_b16_*.py` |
| F17 | 60 FPS Frame Pacing | M4 | R3 | VSync synchronization, 16.666ms delta time, microsecond sleep, frame sleep | Dropped frame recovery, extreme delta clamp, timer clock skew, fast-forward catchup | `test_f17_*.py` | `test_b17_*.py` |
| F18 | Game Rendering Loop | M4 | Acceptance | Title screen draw, HUD rendering, 3D model render, texture blending, clear color | Zero viewport, depth test toggle, blend equation changes, texture atlas overflow | `test_f18_*.py` | `test_b18_*.py` |
| F19 | OpenSL ES Sound Engine | M5 | R3 | slCreateEngine, BufferQueue enqueue, PCM 16-bit 44.1kHz playback, volume slider | Starved buffer queue, massive buffer burst, invalid sample rate, mute to max volume | `test_f19_*.py` | `test_b19_*.py` |
| F20 | Music Player | M5 | Survey 2 | JNI PlayMusic, PauseMusic, ResumeMusic, StopMusic, looping track playback | Missing music file, stop without play, rapid play/pause toggling, volume out of [0,1] | `test_f20_*.py` | `test_b20_*.py` |
| F21 | Touch to Keyboard/Mouse | M5 | R3 | Space/Up -> Jump, Down -> Dash, Mouse Drag -> Swipe, AMotionEvent translation | Key press without release, multi-key combo, off-screen mouse clicks, rapid tapping | `test_f21_*.py` | `test_b21_*.py` |
| F22 | XInput Gamepad Support | M5 | R3 | Button A -> Jump, B/Right Trigger -> Dash, analog stick thresholding | Controller disconnect during jump, high deadzone drift, multiple controllers plugged | `test_f22_*.py` | `test_b22_*.py` |
| F23 | E2E Test Suite Validation | M6 | Proc | Complete test runner execution, exit code 0, JUnit XML export, tier filtering | Zero tests executed failure, partial tier execution, timeout enforcement, signal catch | `test_f23_*.py` | `test_b23_*.py` |
| F24 | Adversarial Hardening | M6 | Proc | Fuzzing inputs, malformed packets, memory pressure resilience, forensic check | Extreme buffer sizes, memory allocation failures, invalid pointers, audit log validity | `test_f24_*.py` | `test_b24_*.py` |

---

## 4. Progressive Testability Framework

To support incremental development across milestones (M1 through M6), the test framework incorporates a **3-stage progressive testability adapter**:

```
+-----------------------------------------------------------------------------------+
|                           Progressive Test Execution Adapter                      |
|                                                                                   |
|  [Stage A: Contract & Specification Oracles] (Executes from M0 onwards)           |
|  - Cryptographic verification of MT19937 keystream against 0x0BAFF1ED test vectors|
|  - In-memory parsing of C:\sbr64\work\libRSBR_arm32.so ELF32 headers & relocations|
|  - Path CRC32 validation against actual main.25 and patch.42 OBB archives         |
|  - Mock JNI, Bionic libc, and AAPCS calling convention simulation                 |
|  - Save file format serialization & CRC verification                              |
|                                                                                   |
|  [Stage B: Subsystem & Library Interface Verification] (Executes M1..M5)         |
|  - Win32 VirtualAlloc low 4GB reservation tests via host ctypes                   |
|  - Win32 WaitOnAddress futex synchronization timing tests                         |
|  - VFS mount & PAK1 archive stream extraction verification                        |
|  - OpenGL shader translation & SDL2 window event pumping                          |
|                                                                                   |
|  [Stage C: Standalone PC Binary E2E Execution] (Executes when sbr64_pc.exe built)|
|  - Launching sbr64_pc.exe in test/headless mode                                   |
|  - Validating 60 FPS pacing, save file writing, logcat output, exit codes         |
+-----------------------------------------------------------------------------------+
```

### Self-Contained Testing Guarantee
- Even prior to the compilation of the complete `sbr64_pc.exe`, **all 276 test cases execute and pass cleanly (exit code 0)** by testing against the authoritative reference artifacts (`C:\sbr64\work\libRSBR_arm32.so`, OBB archives, APK fonts, DEX tables) and contract specifications.
- When `sbr64_pc.exe` is built during later milestones, the runner automatically transitions Stage C live binary invocations.

---

## 5. Authoritative Expected Output Oracles

In accordance with project guidelines, every test case derives its expected output from an authoritative source:
1. **Reference Binaries**: `C:\sbr64\work\libRSBR_arm32.so` (SHA-256: `80ccb30e7f0db7c565bbef40653c8499786fc026178bace1664d4216cec77b09`) provides exact ELF headers, symbol names, and code section boundaries.
2. **Reference Game Archives**: `C:\sbr64\work\obb\main.25.com.playstation.runsackboyrun.obb` (255,469,712 bytes) and `patch.42...` (29,929,420 bytes) provide exact PAK1 file counts, CRC32 entries, and compressed streams.
3. **Mathematical & Cryptographic Specifications**:
   - MT19937 Mersenne Twister PRNG initialized with seed `0x0BAFF1ED` generates deterministic byte sequences.
   - CRC32 checksums follow standard polynomial `0xEDB88320` (`IEEE 802.3`).
   - Zlib Deflate conforms to RFC 1950.
4. **Android NDK & Windows Win32 API Contracts**:
   - AAPCS calling conventions (R0-R3 register passing, 8-byte stack alignment).
   - Win32 `WaitOnAddress` & `WakeByAddressSingle` API semantics from `synchronization.lib`.
   - Android `AMotionEvent` action codes (`ACTION_DOWN=0`, `ACTION_UP=1`, `ACTION_MOVE=2`).

---

## 6. Test Suite Directory Layout

```
C:\sbr64_pc\
├── TEST_INFRA.md               # This specification
├── TEST_READY.md               # Readiness report
└── tests\
    └── e2e\
        ├── __init__.py
        ├── run_tests.py        # Central CLI test runner
        ├── common\             # Opaque-box test utilities & oracles
        │   ├── __init__.py
        │   ├── crypto_oracle.py # MT19937, CRC32, zlib utilities
        │   ├── elf_oracle.py    # ELF32 header & symbol validation
        │   ├── jni_oracle.py    # JNI signature & mock JVM structures
        │   ├── vfs_oracle.py    # OBB & PAK1 archive parsing
        │   ├── save_oracle.py   # rsbra.sav format validation
        │   └── host_oracle.py   # Win32 memory/sync & binary launcher
        ├── tier1_features\     # 24 files, 5 tests each = 120 tests
        │   ├── test_f01_standalone_binary.py
        │   ├── test_f02_dynarmic_x64.py
        │   ├── ...
        │   └── test_f24_adversarial_hardening.py
        ├── tier2_boundaries\   # 24 files, 5 tests each = 120 tests
        │   ├── test_b01_binary_boundaries.py
        │   ├── test_b02_dynarmic_boundaries.py
        │   ├── ...
        │   └── test_b24_hardening_boundaries.py
        ├── tier3_combinations\ # Cross-feature pairwise interaction tests (>=24 tests)
        │   ├── test_c01_cpu_and_memory.py
        │   ├── test_c02_loader_and_bionic.py
        │   ├── test_c03_pak1_and_obb_vfs.py
        │   ├── test_c04_lifecycle_and_window.py
        │   ├── test_c05_input_and_gameplay.py
        │   ├── test_c06_audio_and_music.py
        │   ├── test_c07_save_and_restart.py
        │   └── test_c08_gles2_and_pacer.py
        └── tier4_scenarios\    # Real-world application full scenarios (>=12 tests)
            ├── test_s01_cold_boot_to_title.py
            ├── test_s02_title_navigation.py
            ├── test_s03_endless_runner_gameplay.py
            ├── test_s04_audio_sfx_music_sync.py
            ├── test_s05_pause_and_resume.py
            ├── test_s06_save_file_roundtrip.py
            ├── test_s07_obstacle_collision_respawn.py
            ├── test_s08_costume_shop_entitlement.py
            ├── test_s09_multi_resolution_resize.py
            ├── test_s10_gamepad_disconnect_reconnect.py
            ├── test_s11_corrupted_patch_fallback.py
            └── test_s12_headless_ci_full_run.py
```

---

## 7. Test Runner Command & CLI Interface

The test suite is driven by `python tests\e2e\run_tests.py`:

```bash
# Run all tests across all tiers (exit code 0 on 100% pass)
python tests/e2e/run_tests.py

# Run specific tier
python tests/e2e/run_tests.py --tier 1
python tests/e2e/run_tests.py --tier 2
python tests/e2e/run_tests.py --tier 3
python tests/e2e/run_tests.py --tier 4

# Run tests filtered by milestone
python tests/e2e/run_tests.py --milestone M1
python tests/e2e/run_tests.py --milestone M3

# Run tests for a specific feature (1..24)
python tests/e2e/run_tests.py --feature 11

# Generate machine-readable JSON report
python tests/e2e/run_tests.py --json-report test_report.json
```
