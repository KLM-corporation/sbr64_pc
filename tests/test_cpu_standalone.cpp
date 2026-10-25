#include <iostream>
#include <cassert>
#include <iomanip>
#include "core/mem_win32.h"
#include "core/cpu.h"

using namespace zb;

int main() {
    std::cout << "[TEST] Initializing Win32 memory subsystem...\n";
    mem::MemoryConfig mem_cfg;
    mem_cfg.enable_identity_map = true;
    mem::init(mem_cfg);

    std::cout << "[TEST] Initializing Dynarmic CPU JIT...\n";
    cpu::Config cfg;
    cfg.fastmem = true;
    cfg.fast_dispatch = true;
    cpu::init(cfg);

    // Allocate 64KB executable code page at 0x10000000
    cpu::gaddr code_base = 0x10000000;
    int err = 0;
    cpu::gaddr mapped = mem::guest_mmap(code_base, 0x10000, 7 /* PROT_READ|WRITE|EXEC */, 0x10 /* MAP_FIXED */, -1, 0, &err);
    assert(mapped == code_base);
    (void)mapped;

    // ==========================================
    // Test 1: ARM32 Arithmetic: (r0 + r1) * r2 - 5
    // ==========================================
    std::cout << "[TEST 1] ARM32 Arithmetic & Register Passing..." << std::endl;
    mem::wr<cpu::u32>(code_base + 0x00, 0xE0800001); // add r0, r0, r1
    mem::wr<cpu::u32>(code_base + 0x04, 0xE0030290); // mul r3, r0, r2
    mem::wr<cpu::u32>(code_base + 0x08, 0xE2430005); // sub r0, r3, #5
    mem::wr<cpu::u32>(code_base + 0x0C, 0xE12FFF1E); // bx lr

    cpu::u32 args1[] = { 10, 20, 3 };
    cpu::u64 ret1 = cpu::call(code_base, args1, 3);
    std::cout << "  (10 + 20) * 3 - 5 = " << (ret1 & 0xFFFFFFFFu) << std::endl;
    assert((ret1 & 0xFFFFFFFFu) == 85);
    std::cout << "  -> PASS" << std::endl;

    // ==========================================
    // Test 2: Thumb-2 Mode Execution: r0 = r0 + 42
    // ==========================================
    std::cout << "[TEST 2] Thumb-2 Execution..." << std::endl;
    cpu::gaddr thumb_code = code_base + 0x100;
    mem::wr<cpu::u16>(thumb_code + 0x00, 0x302A); // adds r0, #42
    mem::wr<cpu::u16>(thumb_code + 0x02, 0x4770); // bx lr

    cpu::u32 args2[] = { 100 };
    cpu::u64 ret2 = cpu::call(thumb_code | 1, args2, 1); // bit 0 set = Thumb
    std::cout << "  100 + 42 = " << (ret2 & 0xFFFFFFFFu) << std::endl;
    assert((ret2 & 0xFFFFFFFFu) == 142);
    std::cout << "  -> PASS" << std::endl;

    // ==========================================
    // Test 3: Host SVC Thunk Roundtrip
    // ==========================================
    std::cout << "[TEST 3] Guest -> Host SVC Thunk Roundtrip..." << std::endl;
    cpu::gaddr thunk_addr = cpu::register_thunk("test_multiply_add", [](JitCtx& j) {
        cpu::u32 a = j.arg(0);
        cpu::u32 b = j.arg(1);
        j.set_ret(a * 10 + b);
    });

    cpu::gaddr call_thunk_code = code_base + 0x200;
    // Guest pushes caller lr, calls thunk_addr, adds 1 to result, pops caller lr to pc
    mem::wr<cpu::u32>(call_thunk_code + 0x00, 0xE92D4000); // push {lr}
    mem::wr<cpu::u32>(call_thunk_code + 0x04, 0xE59F200C); // ldr r2, [pc, #12] -> thunk_addr at +0x18
    mem::wr<cpu::u32>(call_thunk_code + 0x08, 0xE1A0E00F); // mov lr, pc (+0x10)
    mem::wr<cpu::u32>(call_thunk_code + 0x0C, 0xE12FFF12); // bx r2
    mem::wr<cpu::u32>(call_thunk_code + 0x10, 0xE2800001); // add r0, r0, #1
    mem::wr<cpu::u32>(call_thunk_code + 0x14, 0xE8BD8000); // pop {pc}
    mem::wr<cpu::u32>(call_thunk_code + 0x18, thunk_addr); // .word thunk_addr

    cpu::u32 args3[] = { 7, 3 }; // 7 * 10 + 3 = 73; + 1 = 74
    cpu::u64 ret3 = cpu::call(call_thunk_code, args3, 2);
    std::cout << "  Thunk returned and incremented: " << (ret3 & 0xFFFFFFFFu) << std::endl;
    assert((ret3 & 0xFFFFFFFFu) == 74);
    std::cout << "  -> PASS" << std::endl;

    // ==========================================
    // Test 4: CP15 TLS Register Access
    // ==========================================
    std::cout << "[TEST 4] CP15 TPIDRURO Emulation..." << std::endl;
    cpu::gaddr cp15_code = code_base + 0x300;
    mem::wr<cpu::u32>(cp15_code + 0x00, 0xEE1D0F70); // mrc p15, 0, r0, c13, c0, 3
    mem::wr<cpu::u32>(cp15_code + 0x04, 0xE12FFF1E); // bx lr

    cpu::GuestThread* t = cpu::current_thread();
    t->tls = 0xAABBCCDD;
    for (auto& ctx_ptr : t->pool) {
        ctx_ptr->tpidruro = 0xAABBCCDD;
    }

    cpu::u64 ret4 = cpu::call(cp15_code, nullptr, 0);
    std::cout << "  CP15 TPIDRURO read: 0x" << std::hex << (ret4 & 0xFFFFFFFFu) << std::dec << std::endl;
    assert((ret4 & 0xFFFFFFFFu) == 0xAABBCCDD);
    std::cout << "  -> PASS" << std::endl;

    std::cout << "\n=====================================" << std::endl;
    std::cout << "ALL DYNARMIC CPU SELF-TESTS PASSED!" << std::endl;
    std::cout << "=====================================" << std::endl;
    return 0;
}
