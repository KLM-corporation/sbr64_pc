cmake_minimum_required(VERSION 3.20)
project(sbr64_pc VERSION 1.0.0 LANGUAGES C CXX)

# ==============================================================================
# Global C++20 Standards and Options
# ==============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_CONSOLE "Build as console application for CLI/testing" ON)
option(BUILD_STATIC "Link C++ runtime statically for standalone distribution" ON)

# ==============================================================================
# Compiler Warnings and Flags
# ==============================================================================
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /bigobj /EHsc)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter -Wa,-mbig-obj)
endif()

add_compile_definitions(
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

# ==============================================================================
# Third-Party: Dynarmic ARM32 JIT Engine
# ==============================================================================
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dynarmic/CMakeLists.txt")
    set(DYNARMIC_TESTS OFF CACHE BOOL "Disable dynarmic test suite" FORCE)
    set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "Use bundled externals" FORCE)
    set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "Disable -Werror" FORCE)
    set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "Disable PCH to avoid toolchain mismatches" FORCE)
    set(DYNARMIC_FRONTENDS "A32" CACHE STRING "Compile ARM32 frontend only" FORCE)
    set(Boost_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost" CACHE PATH "Path to bundled Boost" FORCE)

    add_subdirectory(third_party/dynarmic EXCLUDE_FROM_ALL)
endif()

# ==============================================================================
# Source Files Discovery
# ==============================================================================
set(SBR64_SOURCES
    src/main.cpp
    src/core/mem_win32.cpp
    src/core/sync_win32.cpp
    src/core/cpu.cpp
    src/core/dlmalloc_glue.c
    src/core/elf_loader.cpp
    src/platform/host_io.cpp
    src/platform/win_window.cpp
    src/android/bionic_hle.cpp
    src/android/jni_env.cpp
    src/android/native_activity.cpp
    src/android/ndk_glue.cpp
    src/android/gles_bridge.cpp
    src/android/runner.cpp
    src/audio/opensl_win32.cpp
    src/audio/music_player.cpp
)

# ==============================================================================
# Executable Target: sbr64_pc
# ==============================================================================
add_executable(sbr64_pc ${SBR64_SOURCES})

set_target_properties(sbr64_pc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_include_directories(sbr64_pc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# Media Foundation decodes the MP3 background music.
set(SBR64_MF_LIBS mfplat mfreadwrite mfuuid ole32)

target_link_libraries(sbr64_pc PRIVATE
    kernel32
    user32
    gdi32
    opengl32
    winmm
    synchronization
    advapi32
    shell32
    ${SBR64_MF_LIBS}
)

if(TARGET dynarmic)
    target_link_libraries(sbr64_pc PRIVATE dynarmic)
endif()

if(MINGW)
    if(BUILD_STATIC)
        target_link_options(sbr64_pc PRIVATE -static)
    endif()
    if(BUILD_CONSOLE)
        target_link_options(sbr64_pc PRIVATE -mconsole)
    else()
        target_link_options(sbr64_pc PRIVATE -mwindows)
    endif()
endif()

# ==============================================================================
# Standalone Unit Test Targets
# ==============================================================================

# 1. Test Win32 Memory
add_executable(test_mem_win32
    tests/test_mem_win32.cpp
    src/core/mem_win32.cpp
    src/core/dlmalloc_glue.c
)
set_target_properties(test_mem_win32 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
target_include_directories(test_mem_win32 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_mem_win32 PRIVATE
    kernel32
    synchronization
)
if(MINGW)
    target_link_options(test_mem_win32 PRIVATE -static -mconsole)
endif()

# 2. Test Win32 Synchronization
add_executable(test_sync_win32
    tests/test_sync_win32.cpp
    src/core/sync_win32.cpp
    src/core/mem_win32.cpp
    src/core/dlmalloc_glue.c
)
set_target_properties(test_sync_win32 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
target_include_directories(test_sync_win32 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_sync_win32 PRIVATE
    kernel32
    synchronization
)
if(MINGW)
    target_link_options(test_sync_win32 PRIVATE -static -mconsole)
endif()

# 3. Test Dynarmic CPU Standalone
if(TARGET dynarmic)
    add_executable(test_cpu_standalone
        tests/test_cpu_standalone.cpp
        src/core/cpu.cpp
        src/core/mem_win32.cpp
        src/core/sync_win32.cpp
        src/core/dlmalloc_glue.c
    )
    set_target_properties(test_cpu_standalone PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    target_include_directories(test_cpu_standalone PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(test_cpu_standalone PRIVATE
        dynarmic
        kernel32
        synchronization
    )
    if(MINGW)
        target_link_options(test_cpu_standalone PRIVATE -static -mconsole)
    endif()

    # 4. Adversarial Dynarmic CPU Stress Test Target
    add_executable(test_cpu_adversarial
        tests/test_cpu_adversarial.cpp
        src/core/cpu.cpp
        src/core/mem_win32.cpp
        src/core/sync_win32.cpp
        src/core/dlmalloc_glue.c
    )
    set_target_properties(test_cpu_adversarial PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    target_include_directories(test_cpu_adversarial PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(test_cpu_adversarial PRIVATE
        dynarmic
        kernel32
        synchronization
    )
    if(MINGW)
        target_link_options(test_cpu_adversarial PRIVATE -static -mconsole)
    endif()

    # 5. Test Dynamic ELF32 Loader
    add_executable(test_elf_loader
        tests/test_elf_loader.cpp
        src/core/elf_loader.cpp
        src/android/bionic_hle.cpp
        src/core/cpu.cpp
        src/core/mem_win32.cpp
        src/core/sync_win32.cpp
        src/core/dlmalloc_glue.c
    )
    set_target_properties(test_elf_loader PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    target_include_directories(test_elf_loader PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(test_elf_loader PRIVATE
        dynarmic
        kernel32
        synchronization
    )
    if(MINGW)
        target_link_options(test_elf_loader PRIVATE -static -mconsole)
    endif()

    # 6. Test Bionic HLE
    add_executable(test_bionic_hle
        tests/test_bionic_hle.cpp
        src/android/bionic_hle.cpp
        src/core/cpu.cpp
        src/core/mem_win32.cpp
        src/core/sync_win32.cpp
        src/core/dlmalloc_glue.c
    )
    set_target_properties(test_bionic_hle PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    target_include_directories(test_bionic_hle PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(test_bionic_hle PRIVATE
        dynarmic
        kernel32
        synchronization
    )
    if(MINGW)
        target_link_options(test_bionic_hle PRIVATE -static -mconsole)
    endif()

    # 7. Test JNI & Lifecycle
    add_executable(test_jni_lifecycle
        tests/test_jni_lifecycle.cpp
        src/android/jni_env.cpp
        src/android/native_activity.cpp
        src/audio/music_player.cpp
        src/core/cpu.cpp
        src/core/mem_win32.cpp
        src/core/sync_win32.cpp
        src/core/dlmalloc_glue.c
    )
    set_target_properties(test_jni_lifecycle PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    target_include_directories(test_jni_lifecycle PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_link_libraries(test_jni_lifecycle PRIVATE
        dynarmic
        kernel32
        synchronization
        ${SBR64_MF_LIBS}
    )
    if(MINGW)
        target_link_options(test_jni_lifecycle PRIVATE -static -mconsole)
    endif()
endif()

# 8. Test background music decoder (Media Foundation)
add_executable(test_music_player
    tests/test_music_player.cpp
    src/audio/music_player.cpp
)
set_target_properties(test_music_player PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
target_include_directories(test_music_player PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_music_player PRIVATE
    kernel32
    ${SBR64_MF_LIBS}
)
if(MINGW)
    target_link_options(test_music_player PRIVATE -static -mconsole)
endif()


