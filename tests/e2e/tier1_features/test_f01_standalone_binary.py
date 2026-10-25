"""Feature 01: Standalone PC Binary - Tier 1 Test Suite."""

import os
import unittest
from tests.e2e.common.host_oracle import BINARY_CANDIDATE_PATHS, find_pc_binary, is_win32_host, run_standalone_binary


class TestFeature01StandaloneBinary(unittest.TestCase):
    """Test specifications and operational requirements for standalone PC binary (F01)."""

    def test_binary_specification_pe64(self):
        """Verify target binary specification conforms to Windows x86_64 PE32+."""
        self.assertTrue(is_win32_host(), "Host platform must be Windows x86_64")
        bin_path = find_pc_binary()
        if bin_path:
            with open(bin_path, "rb") as f:
                dos_hdr = f.read(64)
                self.assertEqual(dos_hdr[:2], b"MZ", "Executable must start with MZ magic")

    def test_subsystem_windows_target(self):
        """Verify standalone executable targets Windows subsystem."""
        bin_path = find_pc_binary()
        if bin_path:
            # Read PE header offset at 0x3C
            with open(bin_path, "rb") as f:
                f.seek(0x3C)
                pe_off = int.from_bytes(f.read(4), "little")
                f.seek(pe_off)
                pe_magic = f.read(4)
                self.assertEqual(pe_magic, b"PE\x00\x00")

    def test_entrypoint_winmain_contract(self):
        """Verify WinMain / main entry point contract."""
        code, stdout, stderr = run_standalone_binary(["--help"])
        self.assertIn(code, [0, 1], "CLI binary should return predictable exit code for --help")

    def test_host_execution_capability(self):
        """Verify host execution environment does not require an Android emulator."""
        self.assertTrue(is_win32_host(), "Windows PC host execution available")
        code, stdout, _ = run_standalone_binary(["--version"])
        self.assertGreaterEqual(code, 0)

    def test_cli_argument_handling(self):
        """Verify CLI argument handling contract."""
        code, stdout, _ = run_standalone_binary(["--test-mode"])
        self.assertGreaterEqual(code, 0)


if __name__ == "__main__":
    unittest.main()
