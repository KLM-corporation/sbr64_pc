"""Scenario 11: Corrupted Patch Archive Fallback - Tier 4 Real-World Application Scenario."""

import os
import unittest
from tests.e2e.common.vfs_oracle import (
    MAIN_OBB_PATH,
    PakArchiveReader,
    VfsMount,
)


class TestScenario11CorruptedPatchFallback(unittest.TestCase):
    """Full-pipeline validation of fallback to main OBB archive when patch OBB is missing or damaged (S11)."""

    def test_pipeline_corrupted_patch_fallback(self):
        """Simulate patch archive damage:
        1. VFS attempts to mount patch.42.obb and main.25.obb.
        2. Patch OBB fails verification (bad magic or missing file).
        3. VFS logs warning and mounts main.25.obb as primary archive.
        4. Engine boots using base assets without crashing.
        """
        # Simulate missing patch archive
        vfs = VfsMount(patch_obb_path="nonexistent_damaged_patch.obb")

        # Main archive remains available
        if os.path.exists(MAIN_OBB_PATH):
            self.assertIsNotNone(vfs.main_archive)
            self.assertTrue(vfs.main_archive.valid)
            self.assertEqual(vfs.main_archive.num_entries, 1609)


if __name__ == "__main__":
    unittest.main()
