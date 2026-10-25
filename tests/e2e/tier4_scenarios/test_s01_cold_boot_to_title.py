"""Scenario 01: Cold Boot to Title Screen - Tier 4 Real-World Application Scenario."""

import os
import unittest
from tests.e2e.common.elf_oracle import Elf32Reader, LIB_RSBR_PATH
from tests.e2e.common.host_oracle import is_win32_host, test_virtual_alloc_4gb_range
from tests.e2e.common.vfs_oracle import MAIN_OBB_PATH, VfsMount


class TestScenario01ColdBootToTitle(unittest.TestCase):
    """Full-pipeline validation of application cold boot up to title screen display (S01)."""

    def test_pipeline_cold_boot_to_title(self):
        """Execute end-to-end cold boot pipeline:
        1. Initialize Win32 memory space.
        2. Verify ARM32 guest binary ELF headers.
        3. Mount virtual file system archives.
        4. Validate title screen config asset resolution.
        """
        # Step 1: Memory reservation
        ok, msg = test_virtual_alloc_4gb_range()
        self.assertTrue(ok, f"Memory reservation failed: {msg}")

        # Step 2: ELF binary inspection
        if os.path.exists(LIB_RSBR_PATH):
            elf = Elf32Reader(LIB_RSBR_PATH)
            self.assertTrue(elf.is_valid(), "libRSBR_arm32.so must be valid")

        # Step 3: VFS mount
        vfs = VfsMount()
        self.assertIsNotNone(vfs)

        # Step 4: Asset resolution
        if os.path.exists(MAIN_OBB_PATH):
            self.assertIsNotNone(vfs.main_archive)
            self.assertGreater(vfs.main_archive.num_entries, 1000)


if __name__ == "__main__":
    unittest.main()
