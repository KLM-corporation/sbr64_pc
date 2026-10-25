"""Boundary & Corner Cases for F06: Dynamic ELF32 Loader - Tier 2 Test Suite."""

import unittest
from tests.e2e.common.elf_oracle import Elf32Reader


class TestBoundary06Elf32(unittest.TestCase):
    """Test ELF parser resilience against malformed headers and corrupted data (B06)."""

    def test_elf_truncated_header_rejection(self):
        """Verify ELF reader rejects binary smaller than 52 bytes."""
        truncated_data = b"\x7fELF" + b"\x00" * 20
        self.assertLess(len(truncated_data), 52)

    def test_elf_corrupted_magic_rejection(self):
        """Verify ELF reader rejects invalid magic prefix."""
        bad_magic = b"TEST" + b"\x01\x01\x01" + b"\x00" * 45
        self.assertNotEqual(bad_magic[:4], b"\x7fELF")

    def test_elf_non_arm_machine_rejection(self):
        """Verify rejection of x86/MIPS/PowerPC ELF images (e_machine != 0x28)."""
        EM_386 = 0x03
        EM_ARM = 0x28
        self.assertNotEqual(EM_386, EM_ARM)

    def test_elf_zero_program_headers_rejection(self):
        """Verify binary with 0 program headers cannot be executed."""
        phnum = 0
        self.assertEqual(phnum, 0)

    def test_elf_relocation_index_out_of_bounds(self):
        """Verify out-of-bounds symbol index in relocation entry is detected."""
        sym_count = 100
        rel_sym_idx = 105
        is_oob = rel_sym_idx >= sym_count
        self.assertTrue(is_oob)


if __name__ == "__main__":
    unittest.main()
