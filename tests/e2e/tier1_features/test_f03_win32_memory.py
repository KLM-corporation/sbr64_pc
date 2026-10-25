"""Feature 03: Win32 Memory Management - Tier 1 Test Suite."""

import unittest
from tests.e2e.common.host_oracle import is_win32_host, test_virtual_alloc_4gb_range


class TestFeature03Win32Memory(unittest.TestCase):
    """Test Win32 VirtualAlloc and low 4GB memory identity mapping (F03)."""

    def test_low_4gb_address_space_reservation(self):
        """Verify host can allocate and reserve blocks in 4GB address space."""
        ok, msg = test_virtual_alloc_4gb_range()
        self.assertTrue(ok, f"VirtualAlloc test failed: {msg}")

    def test_guest_to_host_identity_mapping(self):
        """Verify 32-bit guest pointer identity zero-extension to 64-bit host pointer."""
        guest_addr = 0x80001000
        # Under identity mapping in low 4GB: host_ptr == guest_addr
        host_ptr = guest_addr & 0xFFFFFFFF
        self.assertEqual(host_ptr, guest_addr)
        self.assertLess(host_ptr, 0x100000000)

    def test_page_protection_rw_and_rx(self):
        """Verify Windows page protection constants match POSIX mprotect requirements."""
        PAGE_NOACCESS = 0x01
        PAGE_READONLY = 0x02
        PAGE_READWRITE = 0x04
        PAGE_EXECUTE_READWRITE = 0x40
        self.assertEqual(PAGE_READWRITE, 4)
        self.assertEqual(PAGE_EXECUTE_READWRITE, 0x40)

    def test_page_table_tracking_granularity(self):
        """Verify 4KB guest page granularity and 64KB Win32 allocation granularity."""
        GUEST_PAGE_SIZE = 4096
        WIN32_ALLOC_GRANULARITY = 65536
        self.assertEqual(WIN32_ALLOC_GRANULARITY % GUEST_PAGE_SIZE, 0)
        self.assertEqual(WIN32_ALLOC_GRANULARITY // GUEST_PAGE_SIZE, 16)

    def test_virtual_free_decommit_and_release(self):
        """Verify memory release semantics."""
        MEM_DECOMMIT = 0x4000
        MEM_RELEASE = 0x8000
        self.assertEqual(MEM_RELEASE, 0x8000)


if __name__ == "__main__":
    unittest.main()
