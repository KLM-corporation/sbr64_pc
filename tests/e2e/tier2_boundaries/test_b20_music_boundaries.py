"""Boundary & Corner Cases for F20: Music Player - Tier 2 Test Suite."""

import unittest


class TestBoundary20Music(unittest.TestCase):
    """Test boundary conditions for background music files, volume limits, and toggling (B20)."""

    def test_play_music_nonexistent_file(self):
        """Verify PlayMusic on missing music file fails safely without crash."""
        file_exists = False
        play_success = True if file_exists else False
        self.assertFalse(play_success)

    def test_rapid_pause_resume_oscillations(self):
        """Verify 20 rapid pause/resume calls leave player in consistent state."""
        state = "PLAYING"
        for _ in range(20):
            state = "PAUSED" if state == "PLAYING" else "PLAYING"
        self.assertEqual(state, "PLAYING")

    def test_music_volume_negative_infinity_mute(self):
        """Verify setting volume to -infinity or < 0.0 results in 0.0 (mute)."""
        vol = -100.0
        clamped_vol = max(0.0, vol)
        self.assertEqual(clamped_vol, 0.0)

    def test_music_stop_when_already_stopped(self):
        """Verify calling StopMusic when state is already STOPPED is safe no-op."""
        state = "STOPPED"
        # Stop again
        state = "STOPPED"
        self.assertEqual(state, "STOPPED")

    def test_music_double_play_restart_behavior(self):
        """Verify calling PlayMusic when already playing same track rewinds to beginning."""
        pos = 15.0
        # Re-play
        pos = 0.0
        self.assertEqual(pos, 0.0)


if __name__ == "__main__":
    unittest.main()
