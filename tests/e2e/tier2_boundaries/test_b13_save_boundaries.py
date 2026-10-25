"""Boundary & Corner Cases for F13: Local Save Persistence - Tier 2 Test Suite."""

import os
import shutil
import tempfile
import unittest
from tests.e2e.common.save_oracle import SaveManagerOracle, SaveProfile


class TestBoundary13Save(unittest.TestCase):
    """Test boundary conditions for save file corruption, disk full, and overflow (B13)."""

    def setUp(self):
        self.test_dir = tempfile.mkdtemp()
        self.mgr = SaveManagerOracle(self.test_dir)

    def tearDown(self):
        shutil.rmtree(self.test_dir, ignore_errors=True)

    def test_zero_byte_save_file_rejection(self):
        """Verify 0-byte save file is rejected as corrupted."""
        empty_data = b""
        p = SaveProfile.deserialize(empty_data)
        self.assertIsNone(p)

    def test_truncated_save_file_rejection(self):
        """Verify truncated save file (less than minimum 32 bytes) is rejected."""
        short_data = b"SBRS\x01\x00\x00\x00"
        p = SaveProfile.deserialize(short_data)
        self.assertIsNone(p)

    def test_max_uint32_bubble_count_clamping(self):
        """Verify maximum bubble count handling without negative overflow."""
        MAX_BUBBLES = 0xFFFFFFFF
        p = SaveProfile(bubbles=MAX_BUBBLES)
        raw = p.serialize()
        p2 = SaveProfile.deserialize(raw)
        self.assertIsNotNone(p2)
        self.assertEqual(p2.bubbles, MAX_BUBBLES)

    def test_corrupted_magic_save_rejection(self):
        """Verify file without 'SBRS' magic is rejected."""
        p = SaveProfile(bubbles=100)
        raw = bytearray(p.serialize())
        raw[:4] = b"NOPE"
        self.assertIsNone(SaveProfile.deserialize(bytes(raw)))

    def test_missing_save_and_backup_returns_default(self):
        """Verify missing save returns None indicating first-run profile creation."""
        loaded = self.mgr.load()
        self.assertIsNone(loaded)


if __name__ == "__main__":
    unittest.main()
