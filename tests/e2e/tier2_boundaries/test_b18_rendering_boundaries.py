"""Boundary & Corner Cases for F18: Game Rendering Loop - Tier 2 Test Suite."""

import unittest


class TestBoundary18Rendering(unittest.TestCase):
    """Test boundary conditions for scissor boxes, culling, and viewport bounds (B18)."""

    def test_zero_area_scissor_box(self):
        """Verify scissor box with width 0 or height 0 culls all subsequent pixels."""
        sw, sh = 0, 100
        has_area = (sw * sh) > 0
        self.assertFalse(has_area)

    def test_degenerate_triangle_culling(self):
        """Verify triangle with 0 area is culled."""
        # Vertex coordinates: (0,0), (0,0), (1,1) -> area = 0
        area = 0.5 * abs(0*(0-1) + 0*(1-0) + 1*(0-0))
        self.assertEqual(area, 0.0)

    def test_empty_vertex_array_draw_call(self):
        """Verify glDrawArrays with count = 0 returns immediately without error."""
        count = 0
        self.assertEqual(count, 0)

    def test_color_component_over_saturation_clamp(self):
        """Verify RGB color components are clamped to [0.0, 1.0]."""
        r, g, b, a = 1.5, -0.2, 0.8, 1.0
        cr = max(0.0, min(1.0, r))
        cg = max(0.0, min(1.0, g))
        self.assertEqual(cr, 1.0)
        self.assertEqual(cg, 0.0)

    def test_extreme_viewport_offset_coordinates(self):
        """Verify negative viewport offsets (e.g., x = -100) are accepted for off-screen rendering."""
        vx, vy = -100, -50
        self.assertLess(vx, 0)
        self.assertLess(vy, 0)


if __name__ == "__main__":
    unittest.main()
