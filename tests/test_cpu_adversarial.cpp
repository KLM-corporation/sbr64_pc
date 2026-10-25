#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cassert>
#include <cstring>
#include <cstdlib>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/mem_win32.h"
#include "core/cpu.h"
#include "core/common.h"

using namespace zb;

// Global test variables
static cpu::gaddr g_code_base = 0x12000000;
static cpu::gaddr g_data_base = 0x13000000;

// =============================================================================
// Helper: Child Process Subprocess Execution for Out-of-Bounds Fault Test
// =============================================================================
static bool run_fault_subprocess(const char* exe_path, std::string& output, int& exit_code) {
    std::string cmd = std::string("\"") + exe_path + "\" --fault-test";
    
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return false;
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;

    PROCESS_INFORMATION pi{};
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');

    BOOL ok = CreateProcessA(NULL, cmd_buf.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    CloseHandle(hWritePipe);

    if (!ok) {
        CloseHandle(hReadPipe);
        return false;
    }

    char buffer[512];
    DWORD bytesRead = 0;
    while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        output += buffer;
    }

    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    exit_code = static_cast<int>(code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);
    return true;
}

// =============================================================================
// Fault Test Child Mode
// =============================================================================
static void execute_fault_child() {
    mem::MemoryConfig mem_cfg;
    mem_cfg.enable_identity_map = true;
    mem::init(mem_cfg);

    cpu::Config cfg;
    cfg.fastmem = true;
    cfg.fast_dispatch = true;
    cpu::init(cfg);

    cpu::gaddr code = 0x14000000;
    int err = 0;
    mem::guest_mmap(code, 0x10000, 7 /* PROT_READ|WRITE|EXEC */, 0x10 /* MAP_FIXED */, -1, 0, &err);

    // Guest code: LDR R0, [R1] where R1 is unmapped 0x35000000, then BX LR
    mem::wr<cpu::u32>(code + 0x00, 0xE5910000); // ldr r0, [r1]
    mem::wr<cpu::u32>(code + 0x04, 0xE12FFF1E); // bx lr

    cpu::u32 args[] = { 0, 0x35000000 };
    cpu::call(code, args, 2);
    // Should NOT reach here
    std::exit(0);
}

// =============================================================================
// Suite 1: Deep Re-entrancy & Pool Stress
// =============================================================================
static cpu::gaddr g_reentrant_guest_addr = 0;
static cpu::gaddr g_reentrant_thunk_addr = 0;

static void reentrant_thunk_handler(JitCtx& j) {
    cpu::u32 current_depth = j.arg(0);
    cpu::u32 target_depth  = j.arg(1);
    cpu::u32 magic         = j.arg(2);

    GuestThread* t = cpu::current_thread();
    assert(t->depth == static_cast<int>(current_depth));
    (void)t;

    if (current_depth < target_depth) {
        cpu::u32 next_args[] = { current_depth + 1, target_depth, magic };
        cpu::u64 guest_ret = cpu::call(g_reentrant_guest_addr, next_args, 3);
        j.set_ret(static_cast<cpu::u32>(guest_ret) + 1);
    } else {
        j.set_ret(magic + 1);
    }
}

static void test_suite_1_reentrancy() {
    std::cout << "\n======================================================\n";
    std::cout << "[SUITE 1] Deep Guest <-> Host Re-entrancy & Pool Stress\n";
    std::cout << "======================================================\n" << std::flush;

    g_reentrant_thunk_addr = cpu::register_thunk("reentrant_thunk", reentrant_thunk_handler);

    // ARM guest function:
    // r0 = current_depth, r1 = target_depth, r2 = magic
    // Computes: magic = (magic * 3) ^ current_depth
    // Calls reentrant_thunk(current_depth, target_depth, magic)
    // Returns thunk_ret + 10
    g_reentrant_guest_addr = g_code_base + 0x1000;
    cpu::gaddr a = g_reentrant_guest_addr;

    // push {r4, lr}
    mem::wr<cpu::u32>(a + 0x00, 0xE92D4010);
    // mov r3, #3
    mem::wr<cpu::u32>(a + 0x04, 0xE3A03003);
    // mul r2, r2, r3           ; magic = magic * 3
    mem::wr<cpu::u32>(a + 0x08, 0xE0020392);
    // eor r2, r2, r0           ; magic = magic ^ current_depth
    mem::wr<cpu::u32>(a + 0x0C, 0xE0222000);
    // ldr r3, [pc, #16] -> +0x24 (thunk_addr)
    mem::wr<cpu::u32>(a + 0x10, 0xE59F3010);
    // blx r3                   ; call thunk with (r0, r1, r2)
    mem::wr<cpu::u32>(a + 0x14, 0xE12FFF33);
    // add r0, r0, #10          ; ret = thunk_ret + 10
    mem::wr<cpu::u32>(a + 0x18, 0xE280000A);
    // pop {r4, pc}
    mem::wr<cpu::u32>(a + 0x1C, 0xE8BD8010);
    // nop alignment
    mem::wr<cpu::u32>(a + 0x20, 0xE1A00000);
    // word: thunk_addr at a + 0x28 (pc at +0x10 is +0x18, +16 = +0x28)
    mem::wr<cpu::u32>(a + 0x28, g_reentrant_thunk_addr);

    // Subtest 1.1: Stepwise depth re-entrancy up to depth 15
    const int test_depths[] = { 1, 2, 4, 8, 12, 15 };
    for (int target : test_depths) {
        GuestThread* t = cpu::current_thread();
        assert(t->depth == 0);
        (void)t;

        cpu::u32 initial_magic = 0x1234;
        cpu::u32 args[] = { 1, static_cast<cpu::u32>(target), initial_magic };
        
        cpu::u64 result = cpu::call(g_reentrant_guest_addr, args, 3);
        assert(t->depth == 0); // Must completely unwind to 0
        assert(t->pool.size() >= static_cast<size_t>(target));

        std::cout << "  [PASS] Re-entrancy to depth " << target 
                  << " unwound cleanly. Final result: 0x" << std::hex << result << std::dec << "\n" << std::flush;
    }

    // Subtest 1.2: Pool Reuse and High-frequency Cycling (10,000 iterations at depth 6)
    std::cout << "  [RUN] Executing 10,000 re-entrant burst calls (depth 6)...\n" << std::flush;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < 10000; ++iter) {
        cpu::u32 args[] = { 1, 6, static_cast<cpu::u32>(iter) };
        cpu::u64 r = cpu::call(g_reentrant_guest_addr, args, 3);
        (void)r;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "  [PASS] 10,000 re-entrancy iterations completed in " << ms 
              << " ms (" << (10000.0 / (ms / 1000.0)) << " calls/sec).\n" << std::flush;
}

// =============================================================================
// Suite 2: Thumb-2 / ARM Interworking & High-Register Stress
// =============================================================================
static void test_suite_2_interworking_and_registers() {
    std::cout << "\n======================================================\n";
    std::cout << "[SUITE 2] Thumb-2 / ARM Interworking & High-Register Stress\n";
    std::cout << "======================================================\n" << std::flush;

    cpu::gaddr arm_fn        = g_code_base + 0x2000;
    cpu::gaddr thumb_fn      = g_code_base + 0x2100;
    cpu::gaddr arm_leaf      = g_code_base + 0x2200;

    // ARM leaf: r0 = r0 + 25; bx lr
    mem::wr<cpu::u32>(arm_leaf + 0x00, 0xE2800019); // add r0, r0, #25
    mem::wr<cpu::u32>(arm_leaf + 0x04, 0xE12FFF1E); // bx lr

    // Thumb function:
    // Takes r0 (val), r1 (arm_leaf address)
    // Saves r4, lr (Thumb push {r4, lr} = 0xB510)
    // Calls arm_leaf via blx r1 (0x4788)
    // Adds 50: adds r0, #50 (0x3032)
    // Restores r4, pc (Thumb pop {r4, pc} = 0xBD10)
    mem::wr<cpu::u16>(thumb_fn + 0x00, 0xB510); // push {r4, lr}
    mem::wr<cpu::u16>(thumb_fn + 0x02, 0x4788); // blx r1
    mem::wr<cpu::u16>(thumb_fn + 0x04, 0x3032); // adds r0, #50
    mem::wr<cpu::u16>(thumb_fn + 0x06, 0xBD10); // pop {r4, pc}

    // ARM entry function:
    // Takes r0 (val), r1 (thumb_fn address with bit 0 set), r2 (arm_leaf address)
    // push {r4, lr}
    // mov r4, r2
    // mov r2, r1
    // mov r1, r4 ; r1 = arm_leaf
    // blx r2     ; call thumb_fn (bit 0 selects Thumb)
    // add r0, r0, #100
    // pop {r4, pc}
    mem::wr<cpu::u32>(arm_fn + 0x00, 0xE92D4010); // push {r4, lr}
    mem::wr<cpu::u32>(arm_fn + 0x04, 0xE1A04002); // mov r4, r2
    mem::wr<cpu::u32>(arm_fn + 0x08, 0xE1A02001); // mov r2, r1
    mem::wr<cpu::u32>(arm_fn + 0x0C, 0xE1A01004); // mov r1, r4
    mem::wr<cpu::u32>(arm_fn + 0x10, 0xE12FFF32); // blx r2
    mem::wr<cpu::u32>(arm_fn + 0x14, 0xE2800064); // add r0, r0, #100
    mem::wr<cpu::u32>(arm_fn + 0x18, 0xE8BD8010); // pop {r4, pc}

    // Subtest 2.1: Interworking execution
    // (10 + 25) + 50 + 100 = 185
    cpu::u32 args1[] = { 10, thumb_fn | 1, arm_leaf };
    cpu::u64 ret1 = cpu::call(arm_fn, args1, 3);
    assert((ret1 & 0xFFFFFFFFu) == 185);
    std::cout << "  [PASS] ARM -> Thumb -> ARM -> Thumb -> ARM Interworking: result = " 
              << (ret1 & 0xFFFFFFFFu) << " (expected 185)\n" << std::flush;

    // Subtest 2.2: Thumb-16 High Register Operations (r8, r9, r10, r11, r12)
    cpu::gaddr high_reg_fn = g_code_base + 0x2300;
    // Thumb-16 code:
    // mov r8, r0  (0x4680)
    // mov r9, r1  (0x4689)
    // mov r10, r2 (0x4692)
    // mov r11, r3 (0x469B)
    // add r8, r9  (0x44C8)   ; r8 = r0 + r1
    // add r10, r11 (0x44DA)  ; r10 = r2 + r3
    // mov r0, r8  (0x4640)   ; r0 = r8
    // mov r1, r10 (0x4651)   ; r1 = r10
    // adds r0, r0, r1 (0x1840) ; r0 = r0 + r1
    // bx lr       (0x4770)
    mem::wr<cpu::u16>(high_reg_fn + 0x00, 0x4680);
    mem::wr<cpu::u16>(high_reg_fn + 0x02, 0x4689);
    mem::wr<cpu::u16>(high_reg_fn + 0x04, 0x4692);
    mem::wr<cpu::u16>(high_reg_fn + 0x06, 0x469B);
    mem::wr<cpu::u16>(high_reg_fn + 0x08, 0x44C8);
    mem::wr<cpu::u16>(high_reg_fn + 0x0A, 0x44DA);
    mem::wr<cpu::u16>(high_reg_fn + 0x0C, 0x4640);
    mem::wr<cpu::u16>(high_reg_fn + 0x0E, 0x4651);
    mem::wr<cpu::u16>(high_reg_fn + 0x10, 0x1840);
    mem::wr<cpu::u16>(high_reg_fn + 0x12, 0x4770);

    cpu::u32 args2[] = { 0x10000001u, 0x20000002u, 0x30000003u, 0x40000004u };
    cpu::u64 ret2 = cpu::call(high_reg_fn | 1, args2, 4);
    cpu::u32 exp2 = 0x10000001u + 0x20000002u + 0x30000003u + 0x40000004u;
    assert((ret2 & 0xFFFFFFFFu) == exp2);
    std::cout << "  [PASS] Thumb-16 High Register Operations (r8..r11): result = 0x" 
              << std::hex << (ret2 & 0xFFFFFFFFu) << " (expected 0x" << exp2 << std::dec << ")\n" << std::flush;

    // Subtest 2.3: Callee-Saved Register Preservation (r4-r11)
    cpu::gaddr canaries_fn = g_code_base + 0x2400;
    cpu::gaddr dummy_thunk = cpu::register_thunk("dummy_clobber_thunk", [](JitCtx& j) {
        volatile int x = 12345;
        (void)x;
        j.set_ret(99);
    });

    // ARM code:
    // push {r4-r11, lr}
    // mov r4, #0x44 ... mov r11, #0xBB
    // ldr r12, [pc, #36] -> dummy_thunk
    // blx r12 (call dummy_thunk)
    // eor r0, r4, r5; eor r0, r0, r6; ... eor r0, r0, r11
    // pop {r4-r11, pc}
    mem::wr<cpu::u32>(canaries_fn + 0x00, 0xE92D4FF0); // push {r4-r11, lr}
    mem::wr<cpu::u32>(canaries_fn + 0x04, 0xE3A04044); // mov r4, #0x44
    mem::wr<cpu::u32>(canaries_fn + 0x08, 0xE3A05055); // mov r5, #0x55
    mem::wr<cpu::u32>(canaries_fn + 0x0C, 0xE3A06066); // mov r6, #0x66
    mem::wr<cpu::u32>(canaries_fn + 0x10, 0xE3A07077); // mov r7, #0x77
    mem::wr<cpu::u32>(canaries_fn + 0x14, 0xE3A08088); // mov r8, #0x88
    mem::wr<cpu::u32>(canaries_fn + 0x18, 0xE3A09099); // mov r9, #0x99
    mem::wr<cpu::u32>(canaries_fn + 0x1C, 0xE3A0A0AA); // mov r10, #0xAA
    mem::wr<cpu::u32>(canaries_fn + 0x20, 0xE3A0B0BB); // mov r11, #0xBB
    mem::wr<cpu::u32>(canaries_fn + 0x24, 0xE59FC028); // ldr r12, [pc, #40] -> +0x54
    mem::wr<cpu::u32>(canaries_fn + 0x28, 0xE12FFF3C); // blx r12 (call dummy_thunk)
    mem::wr<cpu::u32>(canaries_fn + 0x2C, 0xE0240005); // eor r0, r4, r5
    mem::wr<cpu::u32>(canaries_fn + 0x30, 0xE0200006); // eor r0, r0, r6
    mem::wr<cpu::u32>(canaries_fn + 0x34, 0xE0200007); // eor r0, r0, r7
    mem::wr<cpu::u32>(canaries_fn + 0x38, 0xE0200008); // eor r0, r0, r8
    mem::wr<cpu::u32>(canaries_fn + 0x3C, 0xE0200009); // eor r0, r0, r9
    mem::wr<cpu::u32>(canaries_fn + 0x40, 0xE020000A); // eor r0, r0, r10
    mem::wr<cpu::u32>(canaries_fn + 0x44, 0xE020000B); // eor r0, r0, r11
    mem::wr<cpu::u32>(canaries_fn + 0x48, 0xE8BD8FF0); // pop {r4-r11, pc}
    mem::wr<cpu::u32>(canaries_fn + 0x54, dummy_thunk);

    cpu::u64 ret3 = cpu::call(canaries_fn, nullptr, 0);
    cpu::u32 exp3 = 0x44 ^ 0x55 ^ 0x66 ^ 0x77 ^ 0x88 ^ 0x99 ^ 0xAA ^ 0xBB;
    assert((ret3 & 0xFFFFFFFFu) == exp3);
    (void)exp3;
    std::cout << "  [PASS] Non-volatile callee-saved registers (r4-r11) preserved across thunk: 0x" 
              << std::hex << (ret3 & 0xFFFFFFFFu) << std::dec << "\n" << std::flush;
}

// =============================================================================
// Suite 3: Host SVC Callbacks, Max Arguments & 64-bit Return Values
// =============================================================================
static cpu::u32 g_thunk_args_received[10]{};
static cpu::u64 g_thunk_ret64_val = 0;

static void sum10_thunk_handler(JitCtx& j) {
    for (unsigned i = 0; i < 10; ++i) {
        g_thunk_args_received[i] = j.arg(i);
    }
    // Set 64-bit return
    j.set_ret64(g_thunk_ret64_val);
}

static void test_suite_3_host_svc_and_args() {
    std::cout << "\n======================================================\n";
    std::cout << "[SUITE 3] Host SVC Callbacks, Max Arguments & 64-bit Ret\n";
    std::cout << "======================================================\n" << std::flush;

    cpu::gaddr sum10_thunk_addr = cpu::register_thunk("sum10_thunk", sum10_thunk_handler);

    // Guest function caller passing 10 arguments:
    // r0..r3: arg0..arg3 (passed into caller)
    // pushes arg4..arg9 (6 words = 24 bytes) to stack
    cpu::gaddr caller_10args = g_code_base + 0x3000;
    cpu::gaddr a = caller_10args;

    // push {r4, lr}
    mem::wr<cpu::u32>(a + 0x00, 0xE92D4010);
    // sub sp, sp, #24
    mem::wr<cpu::u32>(a + 0x04, 0xE24DD018);
    // mov r4, #0x50; str r4, [sp, #0]
    mem::wr<cpu::u32>(a + 0x08, 0xE3A04050);
    mem::wr<cpu::u32>(a + 0x0C, 0xE58D4000);
    // mov r4, #0x60; str r4, [sp, #4]
    mem::wr<cpu::u32>(a + 0x10, 0xE3A04060);
    mem::wr<cpu::u32>(a + 0x14, 0xE58D4004);
    // mov r4, #0x70; str r4, [sp, #8]
    mem::wr<cpu::u32>(a + 0x18, 0xE3A04070);
    mem::wr<cpu::u32>(a + 0x1C, 0xE58D4008);
    // mov r4, #0x80; str r4, [sp, #12]
    mem::wr<cpu::u32>(a + 0x20, 0xE3A04080);
    mem::wr<cpu::u32>(a + 0x24, 0xE58D400C);
    // mov r4, #0x90; str r4, [sp, #16]
    mem::wr<cpu::u32>(a + 0x28, 0xE3A04090);
    mem::wr<cpu::u32>(a + 0x2C, 0xE58D4010);
    // mov r4, #0xA0; str r4, [sp, #20]
    mem::wr<cpu::u32>(a + 0x30, 0xE3A040A0);
    mem::wr<cpu::u32>(a + 0x34, 0xE58D4014);
    // ldr r12, [pc, #8] -> +0x44 (sum10_thunk_addr)
    mem::wr<cpu::u32>(a + 0x38, 0xE59FC008);
    // blx r12
    mem::wr<cpu::u32>(a + 0x3C, 0xE12FFF3C);
    // add sp, sp, #24
    mem::wr<cpu::u32>(a + 0x40, 0xE28DD018);
    // pop {r4, pc}
    mem::wr<cpu::u32>(a + 0x44, 0xE8BD8010);
    // word: sum10_thunk_addr at a + 0x48 (pc at +0x38 is +0x40, +8 = +0x48)
    mem::wr<cpu::u32>(a + 0x48, sum10_thunk_addr);

    // Subtest 3.1: 10 arguments and 64-bit return values across extreme patterns
    const cpu::u64 ret_tests[] = {
        0x0000000000000000ULL,
        0x00000000FFFFFFFFULL,
        0xFFFFFFFF00000000ULL,
        0x123456789ABCDEF0ULL,
        0xCAFEBABEDEADBEEFULL,
        0x8000000000000000ULL,
        0xFFFFFFFFFFFFFFFFULL
    };

    for (cpu::u64 expected_ret64 : ret_tests) {
        g_thunk_ret64_val = expected_ret64;
        
        cpu::u32 reg_args[] = { 0x10, 0x20, 0x30, 0x40 };
        cpu::u64 actual_ret64 = cpu::call(caller_10args, reg_args, 4);

        // Verify all 10 arguments received by host
        assert(g_thunk_args_received[0] == 0x10);
        assert(g_thunk_args_received[1] == 0x20);
        assert(g_thunk_args_received[2] == 0x30);
        assert(g_thunk_args_received[3] == 0x40);
        assert(g_thunk_args_received[4] == 0x50);
        assert(g_thunk_args_received[5] == 0x60);
        assert(g_thunk_args_received[6] == 0x70);
        assert(g_thunk_args_received[7] == 0x80);
        assert(g_thunk_args_received[8] == 0x90);
        assert(g_thunk_args_received[9] == 0xA0);

        // Verify 64-bit return value roundtrip
        assert(actual_ret64 == expected_ret64);
        std::cout << "  [PASS] 10 Args + 64-bit Ret 0x" << std::hex << actual_ret64 << std::dec << "\n" << std::flush;
    }

    // Subtest 3.2: 100,000 High-Frequency SVC Thunks
    std::cout << "  [RUN] Executing 100,000 SVC thunk calls in guest loop...\n" << std::flush;
    cpu::gaddr loop_thunk = cpu::register_thunk("loop_inc_thunk", [](JitCtx& j) {
        j.set_ret(j.arg(0) + 1);
    });

    cpu::gaddr loop_code = g_code_base + 0x3200;
    // ARM loop:
    // r0 = count (0)
    // r2 = remaining iterations (100,000)
    // ldr r1, [pc, #12] -> +0x18
    // Loop top at +0x04:
    // blx r1
    // subs r2, r2, #1
    // bne -4 words (-16 bytes) -> +0x04
    // pop {pc}
    mem::wr<cpu::u32>(loop_code + 0x00, 0xE92D4000); // push {lr}
    mem::wr<cpu::u32>(loop_code + 0x04, 0xE59F100C); // ldr r1, [pc, #12] -> +0x18
    // Loop top at +0x08:
    mem::wr<cpu::u32>(loop_code + 0x08, 0xE12FFF31); // blx r1
    mem::wr<cpu::u32>(loop_code + 0x0C, 0xE2522001); // subs r2, r2, #1
    mem::wr<cpu::u32>(loop_code + 0x10, 0x1AFFFFFC); // bne -4 words (-16 bytes) -> +0x08
    mem::wr<cpu::u32>(loop_code + 0x14, 0xE8BD8000); // pop {pc}
    mem::wr<cpu::u32>(loop_code + 0x18, loop_thunk);

    cpu::u32 loop_args[] = { 0, 0, 100000 };
    auto t0 = std::chrono::high_resolution_clock::now();
    cpu::u64 final_val = cpu::call(loop_code, loop_args, 3);
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    assert((final_val & 0xFFFFFFFFu) == 100000);
    (void)final_val;
    std::cout << "  [PASS] 100,000 SVC thunk roundtrips completed in " << ms 
              << " ms (" << (100000.0 / (ms / 1000.0)) << " thunks/sec).\n" << std::flush;
}

// =============================================================================
// Suite 4: Memory Faults, Out-of-Bounds Access, SEH & Fastmem Recovery
// =============================================================================
static void test_suite_4_memory_faults_and_seh(const char* argv0) {
    std::cout << "\n======================================================\n";
    std::cout << "[SUITE 4] Out-of-Bounds Guest Memory & Fastmem Recovery\n";
    std::cout << "======================================================\n" << std::flush;

    // Subtest 4.1: Direct Fastmem Read / Write Correctness
    cpu::gaddr test_page = g_data_base + 0x10000;
    int err = 0;
    mem::guest_mmap(test_page, 0x10000, 3 /* PROT_READ|WRITE */, 0x10 /* MAP_FIXED */, -1, 0, &err);

    cpu::gaddr rw_code = g_code_base + 0x4000;
    // Writes 8, 16, 32, 64-bit values to test_page
    // Reads them back and sums them
    mem::wr<cpu::u32>(rw_code + 0x00, 0xE5C10000); // strb r0, [r1, #0]
    mem::wr<cpu::u32>(rw_code + 0x04, 0xE1C100B2); // strh r0, [r1, #2]
    mem::wr<cpu::u32>(rw_code + 0x08, 0xE5810004); // str r0, [r1, #4]
    mem::wr<cpu::u32>(rw_code + 0x0C, 0xE5910004); // ldr r0, [r1, #4]
    mem::wr<cpu::u32>(rw_code + 0x10, 0xE12FFF1E); // bx lr

    cpu::u32 args1[] = { 0x55AA1234, test_page };
    cpu::u64 r1 = cpu::call(rw_code, args1, 2);
    assert((r1 & 0xFFFFFFFFu) == 0x55AA1234);
    std::cout << "  [PASS] Direct Fastmem Read/Write byte/half/word: 0x" 
              << std::hex << (r1 & 0xFFFFFFFFu) << std::dec << "\n" << std::flush;

    // Subtest 4.2: Guard page protection check
    GuestThread* t = cpu::current_thread();
    assert(mem::is_mapped(t->stack_lo) == false); // PROT_NONE guard page is unmapped in pt
    std::cout << "  [PASS] Guest stack guard page 0x" << std::hex << t->stack_lo 
              << " verified unmapped (PROT_NONE).\n" << std::dec << std::flush;

    // Subtest 4.3: Empirical Subprocess Out-of-Bounds Memory Fault & SEH Trapping
    std::cout << "  [RUN] Spawning child process to execute unmapped guest read (0x35000000)...\n" << std::flush;
    std::string child_output;
    int child_exit = 0;
    bool spawned = run_fault_subprocess(argv0, child_output, child_exit);
    assert(spawned);
    (void)spawned;

    std::cout << "  -> Subprocess exit code: " << child_exit << "\n" << std::flush;
    std::cout << "  -> Captured stderr/stdout snippet:\n" << std::flush;
    std::stringstream ss(child_output);
    std::string line;
    bool found_fault_log = false;
    bool found_state_dump = false;
    bool found_fatal = false;
    while (std::getline(ss, line)) {
        if (line.find("guest read fault") != std::string::npos) {
            std::cout << "     " << line << "\n" << std::flush;
            found_fault_log = true;
        }
        if (line.find("guest state (memory fault)") != std::string::npos) {
            std::cout << "     " << line << "\n" << std::flush;
            found_state_dump = true;
        }
        if (line.find("[FATAL] guest memory fault at 0x35000000") != std::string::npos) {
            std::cout << "     " << line << "\n" << std::flush;
            found_fatal = true;
        }
    }

    assert(found_fault_log);
    assert(found_state_dump);
    assert(found_fatal);
    (void)found_fault_log;
    (void)found_state_dump;
    (void)found_fatal;
    std::cout << "  [PASS] Windows SEH exception handling / fastmem fault recovery verified!\n"
              << "         Hardware 0xC0000005 trapped -> Dynarmic handler invoked -> MemoryRead32 called -> fatal logged cleanly.\n" << std::flush;
}

// =============================================================================
// Suite 5: Multi-Threaded Concurrency & Exclusive Monitor Stress
// =============================================================================
static void test_suite_5_multithreading_and_atomics() {
    std::cout << "\n======================================================\n";
    std::cout << "[SUITE 5] Multi-Threaded Concurrency & Exclusive Monitor\n";
    std::cout << "======================================================\n" << std::flush;

    // Shared counter page in guest memory
    cpu::gaddr atomic_page = g_data_base + 0x20000;
    int err = 0;
    mem::guest_mmap(atomic_page, 0x10000, 3 /* PROT_READ|WRITE */, 0x10 /* MAP_FIXED */, -1, 0, &err);
    mem::wr<cpu::u32>(atomic_page, 0);

    // ARM LDREX / STREX Atomic Increment Loop:
    // r0 = atomic address
    // r1 = loop count
    // Loop top at +0x00:
    // 0x00: ldrex r2, [r0]    ; 0xE1902F9F
    // 0x04: add r2, r2, #1    ; 0xE2822001
    // 0x08: strex r3, r2, [r0]; 0xE1803F92
    // 0x0C: cmp r3, #0        ; 0xE3530000
    // 0x10: bne -6 words      ; 0x1AFFFFFA -> 0x00
    // 0x14: subs r1, r1, #1   ; 0xE2511001
    // 0x18: bne -8 words      ; 0x1AFFFFF8 -> 0x00
    // 0x1C: bx lr             ; 0xE12FFF1E
    cpu::gaddr atomic_code = g_code_base + 0x5000;
    cpu::gaddr a = atomic_code;
    mem::wr<cpu::u32>(a + 0x00, 0xE1902F9F);
    mem::wr<cpu::u32>(a + 0x04, 0xE2822001);
    mem::wr<cpu::u32>(a + 0x08, 0xE1803F92);
    mem::wr<cpu::u32>(a + 0x0C, 0xE3530000);
    mem::wr<cpu::u32>(a + 0x10, 0x1AFFFFFA);
    mem::wr<cpu::u32>(a + 0x14, 0xE2511001);
    mem::wr<cpu::u32>(a + 0x18, 0xE2511001 == 0 ? 0 : 0x1AFFFFF8);
    mem::wr<cpu::u32>(a + 0x18, 0x1AFFFFF8);
    mem::wr<cpu::u32>(a + 0x1C, 0xE12FFF1E);

    constexpr int kNumThreads = 4;
    constexpr int kIncrementsPerThread = 25000;
    constexpr cpu::u32 kExpectedTotal = kNumThreads * kIncrementsPerThread;

    std::cout << "  [RUN] Spawning " << kNumThreads << " host threads contending on LDREX/STREX ("
              << kIncrementsPerThread << " increments each, total " << kExpectedTotal << ")...\n" << std::flush;

    std::vector<std::thread> workers;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int t = 0; t < kNumThreads; ++t) {
        workers.emplace_back([=]() {
            GuestThread* gt = cpu::current_thread();
            assert(gt != nullptr);
            (void)gt;
            cpu::u32 args[] = { atomic_page, kIncrementsPerThread };
            cpu::call(atomic_code, args, 2);
        });
    }

    for (auto& w : workers) {
        w.join();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    cpu::u32 final_count = mem::rd<cpu::u32>(atomic_page);
    std::cout << "  [PASS] Concurrent LDREX/STREX: final count = " << final_count 
              << " / " << kExpectedTotal << " in " << ms << " ms (" 
              << (kExpectedTotal / (ms / 1000.0)) << " atomic ops/sec).\n" << std::flush;
    assert(final_count == kExpectedTotal);
}

// =============================================================================
// Main Entrypoint
// =============================================================================
int main(int argc, char** argv) {
    std::setvbuf(stdout, NULL, _IONBF, 0);

    if (argc > 1 && std::strcmp(argv[1], "--fault-test") == 0) {
        execute_fault_child();
        return 0;
    }

    std::cout << "=================================================================\n";
    std::cout << " RUN SACKBOY! RUN! PC PORT - ADVERSARIAL CPU JIT TEST HARNESS\n";
    std::cout << "=================================================================\n" << std::flush;

    // 1. Memory subsystem init
    mem::MemoryConfig mem_cfg;
    mem_cfg.enable_identity_map = true;
    mem::init(mem_cfg);

    // 2. CPU subsystem init with fastmem
    cpu::Config cpu_cfg;
    cpu_cfg.fastmem = true;
    cpu_cfg.fast_dispatch = true;
    cpu::init(cpu_cfg);

    // 3. Allocate shared test pages
    int err = 0;
    mem::guest_mmap(g_code_base, 0x100000, 7 /* PROT_READ|WRITE|EXEC */, 0x10 /* MAP_FIXED */, -1, 0, &err);
    mem::guest_mmap(g_data_base, 0x100000, 3 /* PROT_READ|WRITE */, 0x10 /* MAP_FIXED */, -1, 0, &err);

    // Run test suites
    test_suite_1_reentrancy();
    test_suite_2_interworking_and_registers();
    test_suite_3_host_svc_and_args();
    test_suite_4_memory_faults_and_seh(argv[0]);
    test_suite_5_multithreading_and_atomics();

    std::cout << "\n=================================================================\n";
    std::cout << " ALL 5 ADVERSARIAL CPU TEST SUITES PASSED FLAWLESSLY!\n";
    std::cout << "=================================================================\n" << std::flush;
    return 0;
}
