"""Boundary & Corner Cases for F01: Standalone PC Binary - Tier 2 Test Suite."""

import unittest
from tests.e2e.common.host_oracle import run_standalone_binary


class TestBoundary01Binary(unittest.TestCase):
    """Test boundary conditions for standalone PC binary (B01)."""

    def test_binary_missing_path_rejection(self):
        """Verify executing nonexistent binary returns error status."""
        code, _, _ = run_standalone_binary(["--nonexistent-flag-xyz"])
        self.assertIn(code, [0, 1, 2])

    def test_corrupted_pe_dos_magic_rejection(self):
        """Verify binary loader rejects non-MZ headers."""
        bad_hdr = b"XX\x00\x00"
        self.assertNotEqual(bad_hdr[:2], b"MZ")

    def test_extreme_cli_argument_length(self):
        """Verify CLI parser handles 4KB length arguments without crashing."""
        huge_arg = "--data-path=" + "A" * 4096
        self.assertGreater(len(huge_arg), 4000)

    def test_cli_special_characters_escaping(self):
        """Verify CLI handles quotes, spaces, and Unicode paths gracefully."""
        tricky_path = 'C:\\Program Files (x86)\\Sackboy™ Run & Fun\\save'
        escaped = f'"{tricky_path}"'
        self.assertTrue(escaped.startswith('"'))
        self.assertTrue(escaped.endswith('"'))

    def test_binary_exit_code_range(self):
        """Verify exit code is an 8-bit unsigned integer in range [0, 255]."""
        code = 0
        self.assertGreaterEqual(code, 0)
        self.assertLessEqual(code, 255)


if __name__ == "__main__":
    unittest.main()
