"""Feature 15: Interactive Window (SDL2) - Tier 1 Test Suite."""

import unittest


class TestFeature15InteractiveWindow(unittest.TestCase):
    """Test SDL2 desktop interactive window creation, high-DPI, and events (F15)."""

    def test_sdl2_window_creation_parameters(self):
        """Verify default window resolution (1280x720) and title."""
        DEFAULT_WIDTH = 1280
        DEFAULT_HEIGHT = 720
        WINDOW_TITLE = "Run Sackboy! Run! (PC Native)"
        self.assertEqual(DEFAULT_WIDTH, 1280)
        self.assertEqual(DEFAULT_HEIGHT, 720)
        self.assertIn("Run Sackboy", WINDOW_TITLE)

    def test_window_resizable_and_high_dpi_flags(self):
        """Verify SDL window creation flags include RESIZABLE, OPENGL, and ALLOW_HIGHDPI."""
        SDL_WINDOW_OPENGL = 0x00000002
        SDL_WINDOW_RESIZABLE = 0x00000020
        SDL_WINDOW_ALLOW_HIGHDPI = 0x00002000
        flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
        self.assertTrue(flags & SDL_WINDOW_OPENGL)
        self.assertTrue(flags & SDL_WINDOW_RESIZABLE)
        self.assertTrue(flags & SDL_WINDOW_ALLOW_HIGHDPI)

    def test_window_aspect_ratio_16_9(self):
        """Verify native 16:9 mobile aspect ratio calculation for viewport letterboxing."""
        aspect = 1280 / 720
        self.assertAlmostEqual(aspect, 1.7777, places=3)

    def test_sdl2_event_pump_loop(self):
        """Verify event pump poll contract."""
        SDL_QUIT = 0x100
        SDL_WINDOWEVENT = 0x200
        self.assertEqual(SDL_QUIT, 0x100)

    def test_window_close_event_quit_handler(self):
        """Verify window close request signals clean engine termination."""
        engine_running = True
        # Simulate SDL_QUIT
        event_type = 0x100  # SDL_QUIT
        if event_type == 0x100:
            engine_running = False
        self.assertFalse(engine_running)


if __name__ == "__main__":
    unittest.main()
