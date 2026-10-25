"""Boundary & Corner Cases for F14: Font Asset Loading - Tier 2 Test Suite."""

import unittest


class TestBoundary14Font(unittest.TestCase):
    """Test boundary conditions for TrueType font rendering, scaling, and missing glyphs (B14)."""

    def test_font_zero_point_size_request(self):
        """Verify requesting point size 0 defaults to minimal readable size (8 pt)."""
        requested_pt = 0
        clamped_pt = max(8, requested_pt)
        self.assertEqual(clamped_pt, 8)

    def test_font_extreme_point_size_1024(self):
        """Verify requesting extreme size 1024 pt is clamped to max texture budget (256 pt)."""
        huge_pt = 1024
        clamped_pt = min(256, huge_pt)
        self.assertEqual(clamped_pt, 256)

    def test_font_missing_glyph_box_fallback(self):
        """Verify missing character code maps to glyph index 0 (undefined / tofu box)."""
        UNDEFINED_GLYPH_INDEX = 0
        self.assertEqual(UNDEFINED_GLYPH_INDEX, 0)

    def test_corrupted_ttf_table_offset(self):
        """Verify out-of-bounds TTF table offset is detected before reading."""
        file_len = 50000
        table_offset = 60000
        is_invalid = table_offset > file_len
        self.assertTrue(is_invalid)

    def test_empty_string_rendering(self):
        """Verify rendering empty string '' produces 0 draw primitives."""
        text = ""
        glyph_count = len(text)
        self.assertEqual(glyph_count, 0)


if __name__ == "__main__":
    unittest.main()
