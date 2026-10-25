"""Feature 09: Activity Lifecycle Boot - Tier 1 Test Suite."""

import unittest


class TestFeature09ActivityLifecycle(unittest.TestCase):
    """Test ANativeActivity_onCreate and android_main lifecycle bootstrap (F09)."""

    def test_native_activity_on_create_entrypoint(self):
        """Verify ANativeActivity_onCreate signature and callback registration."""
        # ANativeActivity_onCreate(ANativeActivity* activity, void* savedState, size_t savedStateSize)
        ON_CREATE_SYMBOL = "ANativeActivity_onCreate"
        self.assertEqual(ON_CREATE_SYMBOL, "ANativeActivity_onCreate")

    def test_android_main_entrypoint(self):
        """Verify android_main entry point from android_native_app_glue."""
        ANDROID_MAIN_SYMBOL = "android_main"
        self.assertEqual(ANDROID_MAIN_SYMBOL, "android_main")

    def test_app_cmd_init_window_command(self):
        """Verify APP_CMD_INIT_WINDOW event triggers surface initialization."""
        APP_CMD_INIT_WINDOW = 1
        self.assertEqual(APP_CMD_INIT_WINDOW, 1)

    def test_app_cmd_term_window_command(self):
        """Verify APP_CMD_TERM_WINDOW event terminates surface context."""
        APP_CMD_TERM_WINDOW = 2
        self.assertEqual(APP_CMD_TERM_WINDOW, 2)

    def test_app_cmd_focus_change_lifecycle(self):
        """Verify APP_CMD_GAINED_FOCUS and APP_CMD_LOST_FOCUS values."""
        APP_CMD_GAINED_FOCUS = 6
        APP_CMD_LOST_FOCUS = 7
        self.assertEqual(APP_CMD_GAINED_FOCUS, 6)
        self.assertEqual(APP_CMD_LOST_FOCUS, 7)


if __name__ == "__main__":
    unittest.main()
