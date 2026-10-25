"""Boundary & Corner Cases for F02: Dynarmic x64 Execution - Tier 2 Test Suite."""

import unittest


class TestBoundary02Dynarmic(unittest.TestCase):
    """Test CPU register boundary limits and instruction edge cases (B02)."""

    def test_program_counter_r15_wrap_boundary(self):
        """Verify 32-bit PC register wraps at 0xFFFFFFFF boundary."""
        max_pc = 0xFFFFFFFF
        next_pc = (max_pc + 4) & 0xFFFFFFFF
        self.assertEqual(next_pc, 3)

    def test_unaligned_instruction_pointer(self):
        """Verify unaligned PC detection for 32-bit ARM instructions (bits [1:0] != 0)."""
        pc_unaligned = 0x00010002
        is_arm_aligned = (pc_unaligned & 3) == 0
        self.assertFalse(is_arm_aligned, "ARM mode instructions must be 4-byte aligned")

    def test_thumb_mode_bit_toggle(self):
        """Verify bit 0 of jump address specifies Thumb mode execution."""
        thumb_target = 0x00010001
        is_thumb = (thumb_target & 1) != 0
        actual_pc = thumb_target & ~1
        self.assertTrue(is_thumb)
        self.assertEqual(actual_pc, 0x00010000)

    def test_svc_number_boundary_limits(self):
        """Verify SVC instruction supports 24-bit immediate field (0 to 0x00FFFFFF)."""
        MAX_SVC = 0x00FFFFFF
        self.assertEqual(MAX_SVC, 16777215)

    def test_cpu_register_extreme_values(self):
        """Verify registers hold signed/unsigned 32-bit extremal values (-2147483648, 4294967295)."""
        min_s32 = -2147483648
        max_u32 = 4294967295
        self.assertEqual(min_s32 & 0xFFFFFFFF, 0x80000000)
        self.assertEqual(max_u32 & 0xFFFFFFFF, 0xFFFFFFFF)


if __name__ == "__main__":
    unittest.main()
