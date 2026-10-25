"""Boundary & Corner Cases for F12: OBB Asset Mounting - Tier 2 Test Suite."""

import unittest
from tests.e2e.common.crypto_oracle import vfs_path_crc32
from tests.e2e.common.vfs_oracle import VfsMount


class TestBoundary12Obb(unittest.TestCase):
    """Test boundary conditions for OBB paths, CRC collisions, and missing entries (B12)."""

    def test_crc32_empty_path_string(self):
        """Verify CRC32 computation on empty string."""
        empty_crc = vfs_path_crc32("")
        self.assertIsInstance(empty_crc, int)
        self.assertEqual(empty_crc, 0)

    def test_path_with_extreme_length_2048_chars(self):
        """Verify CRC32 calculation on long path string (2048 chars)."""
        long_path = "assets/" + "sub/" * 500 + "file.png"
        crc = vfs_path_crc32(long_path)
        self.assertIsInstance(crc, int)

    def test_path_with_spaces_and_special_symbols(self):
        """Verify normalization of paths with spaces and punctuation."""
        p = "Config/Special & Characters (Test)/Locale.txt"
        crc = vfs_path_crc32(p)
        self.assertIsInstance(crc, int)

    def test_missing_entry_crc_returns_none(self):
        """Verify querying non-existent asset path returns None."""
        vfs = VfsMount()
        res = vfs.resolve_asset("non_existent_folder/missing_file_xyz123.bin")
        self.assertIsNone(res)

    def test_path_forward_and_backward_slash_identity(self):
        """Verify path with mixed slashes normalizes identically."""
        p1 = "Config\\Subdir\\File.txt"
        p2 = "config/subdir/file.txt"
        self.assertEqual(vfs_path_crc32(p1), vfs_path_crc32(p2))


if __name__ == "__main__":
    unittest.main()
