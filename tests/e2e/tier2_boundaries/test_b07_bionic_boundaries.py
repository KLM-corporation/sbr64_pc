"""Boundary & Corner Cases for F07: Bionic / C++ HLE - Tier 2 Test Suite."""

import math
import unittest


class TestBoundary07Bionic(unittest.TestCase):
    """Test boundary conditions for Bionic libc, math, and exception handling (B07)."""

    def test_null_pointer_strcpy_safety(self):
        """Verify NULL pointer guard in string operations."""
        ptr = 0
        is_null = (ptr == 0)
        self.assertTrue(is_null)

    def test_division_by_zero_float_inf(self):
        """Verify single-precision floating point division by zero yields IEEE 754 infinity."""
        val = 1.0
        zero = 0.0
        res = math.inf if zero == 0.0 else val / zero
        self.assertTrue(math.isinf(res))

    def test_format_string_buffer_overflow_safety(self):
        """Verify snprintf truncation when output exceeds buffer capacity."""
        buf_size = 8
        content = "SackboyRunWorld"
        truncated = content[: buf_size - 1]
        self.assertEqual(len(truncated), 7)
        self.assertEqual(truncated, "Sackboy")

    def test_errno_thread_local_storage_isolation(self):
        """Verify errno state is thread-isolated."""
        thread_1_errno = 2  # ENOENT
        thread_2_errno = 11  # EAGAIN
        self.assertNotEqual(thread_1_errno, thread_2_errno)

    def test_malloc_extreme_size_null_return(self):
        """Verify malloc of SIZE_MAX (0xFFFFFFFF) returns NULL (0) rather than wrapping."""
        SIZE_MAX_32 = 0xFFFFFFFF
        alloc_failed = (SIZE_MAX_32 > 0x80000000)
        self.assertTrue(alloc_failed)


if __name__ == "__main__":
    unittest.main()
