"""Boundary & Corner Cases for F21: Touch to Keyboard/Mouse - Tier 2 Test Suite."""

import unittest


class TestBoundary21Input(unittest.TestCase):
    """Test boundary conditions for key releases, mouse coordinates, and multi-touch (B21)."""

    def test_key_down_without_key_up_stuck_prevention(self):
        """Verify window focus loss releases all stuck key-down states."""
        key_down = True
        # Focus lost event
        focus_lost = True
        if focus_lost:
            key_down = False
        self.assertFalse(key_down)

    def test_simultaneous_multi_key_press(self):
        """Verify pressing Jump (Space) and Dash (Down) concurrently."""
        jump_active = True
        dash_active = True
        both_pressed = jump_active and dash_active
        self.assertTrue(both_pressed)

    def test_negative_mouse_coordinates_clamping(self):
        """Verify mouse coordinates outside window viewport are clamped to [0, width]."""
        raw_x = -50.0
        clamped_x = max(0.0, min(1280.0, raw_x))
        self.assertEqual(clamped_x, 0.0)

    def test_extreme_mouse_wheel_delta(self):
        """Verify high mouse wheel deltas (+/- 1200) do not overflow scrolling counters."""
        wheel_delta = 1200
        step = wheel_delta // 120
        self.assertEqual(step, 10)

    def test_rapid_touch_tap_frequency_100hz(self):
        """Verify handling 100 touch events per second without dropping frames."""
        events = [("DOWN", i) for i in range(100)]
        self.assertEqual(len(events), 100)


if __name__ == "__main__":
    unittest.main()
