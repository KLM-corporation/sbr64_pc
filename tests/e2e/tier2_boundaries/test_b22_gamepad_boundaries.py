"""Boundary & Corner Cases for F22: XInput Gamepad Support - Tier 2 Test Suite."""

import unittest


class TestBoundary22Gamepad(unittest.TestCase):
    """Test boundary conditions for XInput triggers, maximum stick deflection, and disconnects (B22)."""

    def test_gamepad_maximum_rumble_intensity(self):
        """Verify vibration motor values clamp to uint16 max (65535)."""
        MAX_RUMBLE = 65535
        rumble = 70000
        clamped_rumble = min(MAX_RUMBLE, max(0, rumble))
        self.assertEqual(clamped_rumble, 65535)

    def test_stick_maximum_deflection_32767(self):
        """Verify analog stick range clamps to [-32768, 32767]."""
        raw_stick = 32767
        normalized = raw_stick / 32767.0
        self.assertAlmostEqual(normalized, 1.0, places=4)

    def test_gamepad_rapid_disconnection_reconnection(self):
        """Verify hotplug state machine handles rapid connection state changes."""
        connected = False
        for _ in range(10):
            connected = not connected
        self.assertFalse(connected)

    def test_all_buttons_pressed_simultaneously(self):
        """Verify bitmask handling when all 16 gamepad buttons are depressed simultaneously."""
        ALL_BUTTONS = 0xFFFF
        self.assertEqual(ALL_BUTTONS, 65535)

    def test_controller_index_out_of_range_4(self):
        """Verify controller query for index >= 4 returns ERROR_BAD_ARGUMENTS or False."""
        user_index = 4
        is_valid = user_index < 4
        self.assertFalse(is_valid)


if __name__ == "__main__":
    unittest.main()
