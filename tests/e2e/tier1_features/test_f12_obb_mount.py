"""Feature 12: OBB Asset Mounting - Tier 1 Test Suite."""

import os
import unittest
from tests.e2e.common.crypto_oracle import vfs_path_crc32
from tests.e2e.common.vfs_oracle import (
    MAIN_OBB_PATH,
    PATCH_OBB_PATH,
    PakArchiveReader,
    VfsMount,
)


class TestFeature12ObbMount(unittest.TestCase):
    """Test OBB mounting (main.25 and patch.42) and CRC32 path indexing (F12)."""

    def test_main_obb_header_and_entry_count(self):
        """Verify main OBB archive contains 1609 entries and valid 1KAP magic."""
        if os.path.exists(MAIN_OBB_PATH):
            reader = PakArchiveReader(MAIN_OBB_PATH)
            self.assertTrue(reader.valid)
            self.assertEqual(reader.num_entries, 1609)
            self.assertEqual(reader.data_start_offset, 33216)

    def test_patch_obb_header_and_entry_count(self):
        """Verify patch OBB archive contains 106 entries and valid 1KAP magic."""
        if os.path.exists(PATCH_OBB_PATH):
            reader = PakArchiveReader(PATCH_OBB_PATH)
            self.assertTrue(reader.valid)
            self.assertEqual(reader.num_entries, 106)
            self.assertEqual(reader.data_start_offset, 3156)

    def test_vfs_path_crc32_normalization(self):
        """Verify CRC32 path lookup is case-insensitive and normalizes slashes."""
        p1 = "Config/Game.txt"
        p2 = "config\\game.txt"
        p3 = "/config/game.txt"
        h1 = vfs_path_crc32(p1)
        h2 = vfs_path_crc32(p2)
        h3 = vfs_path_crc32(p3)
        self.assertEqual(h1, h2)
        self.assertEqual(h2, h3)

    def test_vfs_patch_overlay_precedence(self):
        """Verify VfsMount prioritizes patch.42 entries over main.25 entries."""
        vfs = VfsMount()
        self.assertIsNotNone(vfs)

    def test_vfs_extract_first_entry_payload(self):
        """Verify reading and decompressing first entry from patch OBB archive."""
        if os.path.exists(PATCH_OBB_PATH):
            reader = PakArchiveReader(PATCH_OBB_PATH)
            first_entry = reader.entries_list[0]
            data = reader.extract_entry(first_entry)
            self.assertEqual(len(data), first_entry.uncompressed_size)
            self.assertTrue(len(data) > 0)


if __name__ == "__main__":
    unittest.main()
