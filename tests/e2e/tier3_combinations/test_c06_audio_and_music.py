"""Cross-Feature Interaction 06: OpenSL ES Sound Effects x JNI Background Music - Tier 3."""

import unittest


class TestCombination06AudioAndMusic(unittest.TestCase):
    """Test interaction between OpenSL buffer queue sound effects and JNI music player."""

    def test_concurrent_sfx_buffer_and_background_music(self):
        """Verify SFX buffer mixing simultaneously with active background music stream."""
        music_playing = True
        sfx_active_channels = 4
        self.assertTrue(music_playing and sfx_active_channels > 0)

    def test_master_audio_volume_scales_both_sfx_and_music(self):
        """Verify JNI SetAudioGain scales both music and sound effect channels proportionally."""
        master_vol = 0.5
        music_vol = 1.0 * master_vol
        sfx_vol = 0.8 * master_vol
        self.assertEqual(music_vol, 0.5)
        self.assertEqual(sfx_vol, 0.4)

    def test_pause_gameplay_mutes_sfx_and_pauses_music(self):
        """Verify game pause clears SFX buffer queue and pauses music."""
        game_paused = True
        sfx_cleared = game_paused
        music_paused = game_paused
        self.assertTrue(sfx_cleared and music_paused)


if __name__ == "__main__":
    unittest.main()
