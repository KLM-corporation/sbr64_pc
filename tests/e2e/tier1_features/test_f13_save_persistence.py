"""Feature 13: Local Save Persistence - Tier 1 Test Suite."""

import os
import shutil
import tempfile
import unittest
from tests.e2e.common.save_oracle import SaveManagerOracle, SaveProfile


class TestFeature13SavePersistence(unittest.TestCase):
    """Test local save persistence (rsbra.sav, rsbrb.sav) and checksum validation (F13)."""

    def setUp(self):
        self.test_dir = tempfile.mkdtemp()
        self.mgr = SaveManagerOracle(self.test_dir)

    def tearDown(self):
        shutil.rmtree(self.test_dir, ignore_errors=True)

    def test_save_profile_serialization_and_deserialization(self):
        """Verify SaveProfile serializes to binary and reconstructs accurately."""
        p = SaveProfile(bubbles=1500, high_score=98000, distance=3420, unlocked_costumes=5)
        raw = p.serialize()
        p2 = SaveProfile.deserialize(raw)
        self.assertIsNotNone(p2)
        self.assertEqual(p2.bubbles, 1500)
        self.assertEqual(p2.high_score, 98000)
        self.assertEqual(p2.distance, 3420)
        self.assertEqual(p2.unlocked_costumes, 5)

    def test_save_profile_crc32_checksum_validation(self):
        """Verify CRC32 footer detects binary tampering."""
        p = SaveProfile(bubbles=500)
        raw = bytearray(p.serialize())
        # Tamper with byte
        raw[8] ^= 0xFF
        tampered = bytes(raw)
        p2 = SaveProfile.deserialize(tampered)
        self.assertIsNone(p2, "Corrupted save data must be rejected")

    def test_save_file_atomic_write_and_backup_creation(self):
        """Verify saving writes both active and backup save files."""
        p = SaveProfile(bubbles=250)
        self.mgr.save(p)
        self.assertTrue(os.path.exists(self.mgr.active_path))
        self.assertTrue(os.path.exists(self.mgr.backup_path))

    def test_save_manager_fallback_to_backup_on_corruption(self):
        """Verify SaveManager falls back to rsbrb.sav when rsbra.sav is corrupted."""
        p = SaveProfile(bubbles=9999)
        self.mgr.save(p)
        # Corrupt active save
        with open(self.mgr.active_path, "wb") as f:
            f.write(b"CORRUPTED_TRUNCATED_DATA")
        loaded = self.mgr.load()
        self.assertIsNotNone(loaded)
        self.assertEqual(loaded.bubbles, 9999)

    def test_save_directory_creation_and_path_redirect(self):
        """Verify save directory is created if missing."""
        sub_dir = os.path.join(self.test_dir, "nested", "save")
        mgr2 = SaveManagerOracle(sub_dir)
        self.assertTrue(os.path.isdir(sub_dir))


if __name__ == "__main__":
    unittest.main()
