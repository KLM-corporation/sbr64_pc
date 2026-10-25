"""Scenario 06: Save File Persistence Roundtrip - Tier 4 Real-World Application Scenario."""

import os
import shutil
import tempfile
import unittest
from tests.e2e.common.save_oracle import SaveManagerOracle, SaveProfile


class TestScenario06SaveFileRoundtrip(unittest.TestCase):
    """Full-pipeline validation of saving player progress and reloading across sessions (S06)."""

    def setUp(self):
        self.test_dir = tempfile.mkdtemp()
        self.mgr = SaveManagerOracle(self.test_dir)

    def tearDown(self):
        shutil.rmtree(self.test_dir, ignore_errors=True)

    def test_pipeline_save_file_roundtrip(self):
        """Simulate real gameplay session save and reload:
        1. Player starts with 0 bubbles, plays run, collects 340 bubbles, scores 12,500 pts.
        2. Game triggers SaveProfile serialization to rsbra.sav with valid CRC.
        3. Application closes completely.
        4. Application launches next session, parses rsbra.sav.
        5. Verify verified bubble total and high score accurately restored.
        """
        # Session 1: Play run
        session1_profile = SaveProfile(bubbles=340, high_score=12500, distance=890, unlocked_costumes=1)
        ok = self.mgr.save(session1_profile)
        self.assertTrue(ok)
        self.assertTrue(os.path.exists(self.mgr.active_path))

        # Session 2: Fresh launch
        fresh_mgr = SaveManagerOracle(self.test_dir)
        session2_profile = fresh_mgr.load()

        self.assertIsNotNone(session2_profile)
        self.assertEqual(session2_profile.bubbles, 340)
        self.assertEqual(session2_profile.high_score, 12500)
        self.assertEqual(session2_profile.distance, 890)


if __name__ == "__main__":
    unittest.main()
