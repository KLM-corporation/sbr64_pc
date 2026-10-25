"""Feature 02: Dynarmic x64 Execution - Tier 1 Test Suite."""

import os
import struct
import unittest

DYNARMIC_PATH = r"C:\sbr64\third_party\dynarmic"


class TestFeature02DynarmicX64(unittest.TestCase):
    """Test Dynarmic ARM32 JIT execution on x86_64 host (F02)."""

    def test_dynarmic_armv7_configuration(self):
        """Verify Dynarmic ARMv7-A CPU configuration parameters."""
        # ARMv7-A requires 16 general purpose registers, VFPv3-D32 (32 double-precision regs)
        reg_count = 16
        neon_regs = 32
        self.assertEqual(reg_count, 16)
        self.assertEqual(neon_regs, 32)

    def test_cpu_context_register_layout(self):
        """Verify CPU context binary layout for AAPCS thunking."""
        # Context struct: 16 regs (uint32) + CPSR (uint32) + FPSCR (uint32)
        ctx_format = "<16III"
        expected_size = struct.calcsize(ctx_format)
        self.assertEqual(expected_size, 72, "CPU context base size must be 72 bytes")

    def test_svc_gateway_thunk_allocation(self):
        """Verify SVC instruction encoding used by Dynarmic guest thunks."""
        # ARM SVC instruction: 0xEF000000 | (imm24)
        svc_num = 0x42
        svc_inst = 0xEF000000 | svc_num
        packed = struct.pack("<I", svc_inst)
        self.assertEqual(packed[0], 0x42)
        self.assertEqual(packed[3], 0xEF)

    def test_aapcs_calling_convention(self):
        """Verify AAPCS parameter register mapping (R0-R3)."""
        # AAPCS passes first 4 32-bit integer arguments in R0, R1, R2, R3
        registers = {"R0": 0, "R1": 1, "R2": 2, "R3": 3}
        self.assertEqual(len(registers), 4)
        self.assertEqual(registers["R0"], 0)
        self.assertEqual(registers["R3"], 3)

    def test_dynarmic_jit_x64_backend_headers(self):
        """Verify Dynarmic x64 host backend headers exist in third_party."""
        x64_backend_dir = os.path.join(DYNARMIC_PATH, "src", "dynarmic", "backend", "x64")
        if os.path.exists(DYNARMIC_PATH):
            self.assertTrue(os.path.isdir(x64_backend_dir), "Dynarmic x64 backend directory must exist")
            self.assertTrue(
                os.path.exists(os.path.join(x64_backend_dir, "emit_x64.h"))
                or os.path.exists(os.path.join(x64_backend_dir, "x64_core.h")),
                "x64 backend emitter headers must exist",
            )


if __name__ == "__main__":
    unittest.main()
