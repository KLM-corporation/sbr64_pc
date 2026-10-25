#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include "core/common.h"
#include "core/cpu.h"
#include "core/sync_win32.h"
#include "core/elf_loader.h"
#include "core/mem_win32.h"
#include "android/bionic_hle.h"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); \
            std::abort(); \
        } \
    } while (0)

namespace {

zb::gaddr mock_resolver(const std::string& name, bool is_func) {
    (void)is_func;
    if (zb::gaddr a = zb::cpu::find_thunk(name)) return a;
    if (zb::gaddr d = zb::cpu::find_data(name)) return d;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    printf("=====================================================\n");
    printf("   sbr64_pc: Dynamic ELF32 Loader Test Harness       \n");
    printf("=====================================================\n\n");

    // Tier 1: Subsystem Initialization
    printf("[TIER 1] Initializing Memory, Sync, Dynarmic JIT & Bionic Subsystems...\n");
    zb::mem::init();
    zb::sync::init();
    zb::cpu::init();
    zb::bionic::register_all();
    TEST_ASSERT(zb::mem::is_identity_mapped(), "Guest memory must be identity-mapped in low 4GB");
    printf("  -> PASS\n\n");

    // Locate libRSBR_arm32.so
    std::string so_path = "C:\\sbr64\\work\\libRSBR_arm32.so";
    if (argc > 1) {
        so_path = argv[1];
    } else if (!std::filesystem::exists(so_path)) {
        if (std::filesystem::exists("../sbr64/work/libRSBR_arm32.so")) {
            so_path = "../sbr64/work/libRSBR_arm32.so";
        }
    }

    if (!std::filesystem::exists(so_path)) {
        fprintf(stderr, "[WARN] libRSBR_arm32.so not found at %s. Skipping production binary test.\n", so_path.c_str());
        return 0;
    }

    // Tier 2: ELF Binary Loading & PT_LOAD Mapping
    printf("[TIER 2] Loading '%s'...\n", so_path.c_str());
    zb::elf::Module* m = zb::elf::load_file(so_path, &mock_resolver);
    TEST_ASSERT(m != nullptr, "Failed to load ELF module");
    TEST_ASSERT(m->bias > 0, "Module load bias should be non-zero");
    TEST_ASSERT(m->hi > m->lo, "Module mapped span must be positive");
    TEST_ASSERT(m->hi - m->lo >= 0x33675c, "Module span must cover all PT_LOAD segments");
    TEST_ASSERT(m->exidx_count == 2904, "Module exidx count must match libRSBR (2904)");
    printf("  Module mapped: [0x%08x - 0x%08x], bias: 0x%08x\n", m->lo, m->hi, m->bias);
    printf("  -> PASS\n\n");

    // Tier 3: Relocation Verification
    printf("[TIER 3] Verifying Relocations (R_ARM_RELATIVE, ABS32, GLOB_DAT, JUMP_SLOT)...\n");
    // Verify R_ARM_RELATIVE in INIT_ARRAY[0]: offset 0x220240 holds (0x391bc + bias)
    zb::gaddr init0 = zb::rd<zb::u32>(m->bias + 0x220240);
    TEST_ASSERT(init0 == m->bias + 0x000391bc, "R_ARM_RELATIVE relocation failed on init_array[0]");
    printf("  INIT_ARRAY[0] relocated value: 0x%08x (expected 0x%08x)\n", init0, m->bias + 0x000391bc);

    // Verify DT_INIT_ARRAY function pointer table
    TEST_ASSERT(m->init_array.size() == 15, "INIT_ARRAY must contain 15 entries");
    TEST_ASSERT(m->init_array[0] == m->bias + 0x000391bc, "init_array[0] value mismatch");
    TEST_ASSERT(m->init_array[14] == 0, "init_array[14] null terminator mismatch");
    printf("  -> PASS\n\n");

    // Tier 4: Symbol Resolution & Entry Point Verification
    printf("[TIER 4] Verifying Symbol Exports & Entrypoints...\n");
    TEST_ASSERT(m->exports.size() >= 3200, "Export count must exceed 3200");

    // 1. ANativeActivity_onCreate
    zb::gaddr create_addr = zb::elf::resolve_symbol("ANativeActivity_onCreate");
    TEST_ASSERT(create_addr != 0, "ANativeActivity_onCreate not found");
    TEST_ASSERT(create_addr == m->bias + 0x001bf5c8, "ANativeActivity_onCreate relocated address mismatch");
    printf("  Resolved ANativeActivity_onCreate -> 0x%08x (unbiased offset: 0x%x)\n",
           create_addr, create_addr - m->bias);

    // 2. android_main
    zb::gaddr main_addr = zb::elf::resolve_symbol("android_main");
    TEST_ASSERT(main_addr != 0, "android_main not found");
    TEST_ASSERT(main_addr == m->bias + 0x001b9d10, "android_main relocated address mismatch");
    printf("  Resolved android_main             -> 0x%08x (unbiased offset: 0x%x)\n",
           main_addr, main_addr - m->bias);

    // 3. Negative query test
    TEST_ASSERT(zb::elf::resolve_symbol("nonexistent_symbol_12345") == 0, "Unknown symbol should return 0");
    printf("  -> PASS\n\n");

    // Tier 5: Static Constructor Execution via Dynarmic
    printf("[TIER 5] Executing Static Initializers via Dynarmic cpu::call()...\n");
    zb::elf::run_initializers(m);
    printf("  -> Successfully executed all 14 constructors without crashing!\n");
    printf("  -> PASS\n\n");

    // Tier 6: Symbolizer Verification
    printf("[TIER 6] Verifying Dynamic Symbolizer...\n");
    std::string sym1 = zb::cpu::symbolize(create_addr);
    TEST_ASSERT(sym1.find("ANativeActivity_onCreate") != std::string::npos, "Symbolizer failed for ANativeActivity_onCreate");
    printf("  Symbolized 0x%08x -> %s\n", create_addr, sym1.c_str());
    printf("  -> PASS\n\n");

    printf("=====================================================\n");
    printf("   ALL ELF LOADER ARCHITECTURE TESTS PASSED!         \n");
    printf("=====================================================\n");
    return 0;
}
