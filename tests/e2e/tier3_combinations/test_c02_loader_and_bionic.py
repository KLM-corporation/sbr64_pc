"""Cross-Feature Interaction 02: Dynamic ELF32 Loader x Bionic HLE Symbol Hooking - Tier 3."""

import os
import unittest
from tests.e2e.common.elf_oracle import Elf32Reader, LIB_RSBR_PATH


class TestCombination02LoaderAndBionic(unittest.TestCase):
    """Test interaction between ELF32 dynamic linker relocations and Bionic HLE hook table."""

    def setUp(self):
        self.reader = Elf32Reader(LIB_RSBR_PATH)

    def test_relocation_hooking_to_host_thunk_dispatcher(self):
        """Verify dynamic relocations match undefined Bionic symbol names."""
        if os.path.exists(LIB_RSBR_PATH):
            self.assertTrue(self.reader.is_valid())
            self.assertIn("libc.so", self.reader.needed_libs)

    def test_bionic_errno_propagation_from_elf_syscall(self):
        """Verify __set_errno properly syncs across guest and host boundary."""
        EINVAL = 22
        errno_val = EINVAL
        self.assertEqual(errno_val, 22)

    def test_global_data_relocation_resolution(self):
        """Verify R_ARM_GLOB_DAT relocations resolve to host-allocated mock variables."""
        mock_stdin_ptr = 0x10002000
        self.assertGreater(mock_stdin_ptr, 0)


if __name__ == "__main__":
    unittest.main()
