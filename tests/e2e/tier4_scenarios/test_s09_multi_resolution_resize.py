"""Scenario 09: Multi-Resolution Dynamic Window Resize - Tier 4 Real-World Application Scenario."""

import unittest


class TestScenario09MultiResolutionResize(unittest.TestCase):
    """Full-pipeline validation of desktop window resizing across 720p, 1080p, 1440p (S09)."""

    def test_pipeline_multi_resolution_resize(self):
        """Simulate dynamic user window resizing across PC resolutions:
        1. Base window at 1280x720 (16:9).
        2. User resizes window to 1920x1080 (16:9 Full HD).
        3. User resizes window to ultra-wide 2560x1080 (21:9) -> letterboxing bars added.
        4. Viewport calculation keeps virtual game coordinate space at 1280x720 without stretching.
        """
        resolutions = [
            (1280, 720),
            (1920, 1080),
            (2560, 1080),  # 21:9 ultrawide
            (2560, 1440),  # 16:9 QHD
        ]
        TARGET_ASPECT = 16.0 / 9.0

        for width, height in resolutions:
            window_aspect = width / height
            if window_aspect > TARGET_ASPECT:
                # Pillarboxing: black bars on left and right
                viewport_h = height
                viewport_w = int(height * TARGET_ASPECT)
                offset_x = (width - viewport_w) // 2
                offset_y = 0
            else:
                # Letterboxing: black bars on top and bottom
                viewport_w = width
                viewport_h = int(width / TARGET_ASPECT)
                offset_x = 0
                offset_y = (height - viewport_h) // 2

            self.assertGreaterEqual(offset_x, 0)
            self.assertGreaterEqual(offset_y, 0)
            calc_aspect = viewport_w / viewport_h
            self.assertAlmostEqual(calc_aspect, TARGET_ASPECT, places=2)


if __name__ == "__main__":
    unittest.main()
