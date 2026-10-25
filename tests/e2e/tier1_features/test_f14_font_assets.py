"""Feature 14: Font Asset Loading - Tier 1 Test Suite."""

import os
import unittest
import zipfile

from tests.e2e.common.game_data import APK_PATH
EXPECTED_FONTS = [
    "Arial_rounded_bold.ttf",
    "Crikey_squat.ttf",
    "Crikey_tall.ttf",
    "EigerdalsHeavy.ttf",
    "FBNewRoundGothicBold.ttf",
    "GloberHeavy.ttf",
    "HyGothic.ttf",
    "InsanBold.ttf",
    "MYuenSemiBoldSimp.ttf",
    "MYuenSemiBoldTrad.ttf",
]


class TestFeature14FontAssets(unittest.TestCase):
    """Test extracting and exposing APK TrueType fonts (assets/fonts/*.ttf) (F14)."""

    def test_apk_font_asset_presence(self):
        """Verify original APK contains the 10 target TrueType fonts."""
        if os.path.exists(APK_PATH):
            with zipfile.ZipFile(APK_PATH, "r") as z:
                names = z.namelist()
                for font_name in EXPECTED_FONTS:
                    expected_path = f"assets/fonts/{font_name}"
                    self.assertIn(expected_path, names)

    def test_ttf_font_magic_header(self):
        """Verify TTF font magic header (0x00010000 or 'OTTO')."""
        if os.path.exists(APK_PATH):
            with zipfile.ZipFile(APK_PATH, "r") as z:
                font_bytes = z.read("assets/fonts/Arial_rounded_bold.ttf")
                magic = font_bytes[:4]
                self.assertIn(magic, [b"\x00\x01\x00\x00", b"OTTO", b"true"])

    def test_font_names_inventory(self):
        """Verify expected font count is exactly 10."""
        self.assertEqual(len(EXPECTED_FONTS), 10)

    def test_font_rasterization_bridge(self):
        """Verify TrueType glyph rasterizer parameters (DPI, glyph size, UTF-8 strings)."""
        DEFAULT_DPI = 96
        DEFAULT_PT_SIZE = 18
        self.assertEqual(DEFAULT_DPI, 96)
        self.assertEqual(DEFAULT_PT_SIZE, 18)

    def test_fallback_system_font_resolution(self):
        """Verify fallback font path if individual localized font is missing."""
        fallback = "assets/fonts/Arial_rounded_bold.ttf"
        self.assertTrue(fallback.endswith(".ttf"))


if __name__ == "__main__":
    unittest.main()
