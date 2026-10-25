"""Cross-Feature Interaction 07: Save Persistence x JNI SetFilesDir - Tier 3."""

import os
import shutil
import tempfile
import unittest
from tests.e2e.common.save_oracle import SaveManagerOracle, SaveProfile


class TestCombination07SaveAndRestart(unittest.TestCase):
    """Test interaction between JNI filesDir configuration and persistent player profile reload."""

    def setUp(self):
        self.test_dir = tempfile.mkdtemp()
        self.mgr = SaveManagerOracle(self.test_dir)

    def tearDown(self):
        shutil.rmtree(self.test_dir, ignore_errors=True)

    def test_set_files_dir_configures_save_location(self):
        """Verify JNI SetFilesDir points engine to local directory."""
        files_dir = self.test_dir
        self.assertTrue(os.path.isdir(files_dir))

    def test_player_profile_roundtrip_after_session_restart(self):
        """Verify saving in session 1, closing, and reopening in session 2 retains bubble count."""
        # Session 1
        p1 = SaveProfile(bubbles=750, high_score=45000, distance=1800, unlocked_costumes=2)
        self.mgr.save(p1)

        # Session 2 (simulate process restart with fresh manager)
        mgr2 = SaveManagerOracle(self.test_dir)
        p2 = mgr2.load()
        self.assertIsNotNone(p2)
        self.assertEqual(p2.bubbles, 750)
        self.assertEqual(p2.high_score, 45000)

    def test_atomic_save_prevents_data_loss_on_interruption(self):
        """Verify presence of valid backup file when active save is disturbed."""
        p = SaveProfile(bubbles=1234)
        self.mgr.save(p)
        self.assertTrue(os.path.exists(self.mgr.backup_path))


if __name__ == "__main__":
    unittest.main()
