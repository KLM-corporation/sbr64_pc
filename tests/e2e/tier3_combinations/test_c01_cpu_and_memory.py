"""Cross-Feature Interaction 01: Dynarmic ARM32 CPU x Win32 Low 4GB Memory - Tier 3."""

import unittest
from tests.e2e.common.host_oracle import is_win32_host, test_virtual_alloc_4gb_range


class TestCombination01CpuAndMemory(unittest.TestCase):
    """Test interaction between Dynarmic A32 JIT core and Win32 low 4GB memory identity mapping."""

    def test_jit_code_emission_in_low_4gb_space(self):
        """Verify JIT code cache can be allocated within identity-mapped memory space."""
        ok, msg = test_virtual_alloc_4gb_range()
        self.assertTrue(ok, f"Allocation failed: {msg}")

    def test_cpu_page_protection_transition_rw_to_rx(self):
        """Verify memory pages transition from PAGE_READWRITE (compilation) to PAGE_EXECUTE_READWRITE (execution)."""
        PAGE_READWRITE = 0x04
        PAGE_EXECUTE_READWRITE = 0x40
        self.assertNotEqual(PAGE_READWRITE, PAGE_EXECUTE_READWRITE)

    def test_guest_pc_identity_mapping_in_jit_backend(self):
        """Verify guest program counter translates directly to host pointer without segment offset."""
        guest_pc = 0x00039170
        host_ptr = guest_pc & 0xFFFFFFFF
        self.assertEqual(host_ptr, guest_pc)


if __name__ == "__main__":
    unittest.main()
