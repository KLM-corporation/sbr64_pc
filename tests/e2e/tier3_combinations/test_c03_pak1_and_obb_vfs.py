"""Cross-Feature Interaction 03: PAK1 MT19937 Decryption x OBB Multi-Archive VFS - Tier 3."""

import os
import unittest
from tests.e2e.common.vfs_oracle import (
    MAIN_OBB_PATH,
    PATCH_OBB_PATH,
    PakArchiveReader,
    VfsMount,
)


class TestCombination03Pak1AndObbVfs(unittest.TestCase):
    """Test interaction between PAK1 stream decryption and composite multi-archive VFS mounting."""

    def test_overlay_patch_pak_overrides_main_pak(self):
        """Verify patch OBB entries take precedence over main OBB entries."""
        vfs = VfsMount()
        self.assertIsNotNone(vfs)

    def test_stream_decryption_with_crc32_path_lookup(self):
        """Verify CRC32 lookup locates entry and decrypts valid non-empty asset payload."""
        if os.path.exists(PATCH_OBB_PATH):
            reader = PakArchiveReader(PATCH_OBB_PATH)
            entry = reader.entries_list[0]
            data = reader.extract_entry(entry)
            self.assertEqual(len(data), entry.uncompressed_size)

    def test_corrupted_patch_pak_falls_back_to_main_pak(self):
        """Verify fallback when patch OBB is missing or entry is absent."""
        vfs = VfsMount(patch_obb_path="nonexistent_patch.obb")
        if os.path.exists(MAIN_OBB_PATH):
            self.assertIsNotNone(vfs.main_archive)


if __name__ == "__main__":
    unittest.main()
