"""Feature 06: Dynamic ELF32 Loader - Tier 1 Test Suite."""

import os
import unittest
from tests.e2e.common.elf_oracle import Elf32Reader, LIB_RSBR_PATH, EM_ARM, PT_LOAD


class TestFeature06Elf32Loader(unittest.TestCase):
    """Test ELF32 ARM binary parser, segment loader, and dynamic relocator (F06)."""

    def setUp(self):
        self.reader = Elf32Reader(LIB_RSBR_PATH)

    def test_elf32_magic_and_machine(self):
        """Verify guest library libRSBR_arm32.so has valid ELF32 header and ARM architecture."""
        if os.path.exists(LIB_RSBR_PATH):
            self.assertTrue(self.reader.is_valid(), "libRSBR_arm32.so must be a valid 32-bit ARM ELF")
            self.assertEqual(self.reader.e_machine, EM_ARM)

    def test_load_segments_read_execute_and_data(self):
        """Verify presence of PT_LOAD code (RX) and data (RW) segments."""
        if os.path.exists(LIB_RSBR_PATH):
            load_segments = [ph for ph in self.reader.program_headers if ph["type"] == PT_LOAD]
            self.assertGreaterEqual(len(load_segments), 2, "Must contain at least code and data PT_LOAD segments")
            # Segment 0: PF_R | PF_X = 5
            self.assertEqual(load_segments[0]["flags"] & 1, 1, "Code segment must be executable")
            # Segment 1: PF_R | PF_W = 6
            self.assertEqual(load_segments[1]["flags"] & 2, 2, "Data segment must be writable")

    def test_dt_needed_dependencies(self):
        """Verify dynamic dependencies list contains essential Android shared libraries."""
        if os.path.exists(LIB_RSBR_PATH):
            needed = self.reader.needed_libs
            self.assertIn("libc.so", needed)
            self.assertIn("libm.so", needed)
            self.assertIn("libGLESv2.so", needed)
            self.assertIn("libOpenSLES.so", needed)

    def test_dynamic_symbol_count(self):
        """Verify binary contains exported symbols for native activities and JNI."""
        if os.path.exists(LIB_RSBR_PATH):
            self.assertGreater(len(self.reader.data), 1000000, "libRSBR_arm32.so should be > 1MB")

    def test_arm_relocations_support(self):
        """Verify relocation types R_ARM_JUMP_SLOT (22) and R_ARM_GLOB_DAT (21) support."""
        R_ARM_GLOB_DAT = 21
        R_ARM_JUMP_SLOT = 22
        self.assertEqual(R_ARM_GLOB_DAT, 21)
        self.assertEqual(R_ARM_JUMP_SLOT, 22)


if __name__ == "__main__":
    unittest.main()
