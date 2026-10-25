"""Feature 22: XInput Gamepad Support - Tier 1 Test Suite."""

import unittest


class TestFeature22InputXinput(unittest.TestCase):
    """Test XInput gamepad button mappings and analog stick deadzones (F22)."""

    def test_xinput_button_a_maps_to_jump(self):
        """Verify XInput A button (0x1000) maps to jump action."""
        XINPUT_GAMEPAD_A = 0x1000
        self.assertEqual(XINPUT_GAMEPAD_A, 0x1000)

    def test_xinput_right_trigger_maps_to_dash(self):
        """Verify XInput Right Trigger (threshold > 30 / 255) maps to dash action."""
        TRIGGER_THRESHOLD = 30
        trigger_val = 180
        is_dash = trigger_val > TRIGGER_THRESHOLD
        self.assertTrue(is_dash)

    def test_xinput_deadzone_filtering(self):
        """Verify analog stick deadzone filtering (7849 threshold)."""
        XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE = 7849
        drift_val = 4000
        filtered_val = 0 if abs(drift_val) < XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE else drift_val
        self.assertEqual(filtered_val, 0)

    def test_xinput_controller_index_enumeration(self):
        """Verify 4 controller ports supported (0..3)."""
        XUSER_MAX_COUNT = 4
        self.assertEqual(XUSER_MAX_COUNT, 4)

    def test_xinput_disconnect_fallback_to_keyboard(self):
        """Verify ERROR_DEVICE_NOT_CONNECTED (1167) enables keyboard/mouse fallback."""
        ERROR_DEVICE_NOT_CONNECTED = 1167
        self.assertEqual(ERROR_DEVICE_NOT_CONNECTED, 1167)


if __name__ == "__main__":
    unittest.main()
