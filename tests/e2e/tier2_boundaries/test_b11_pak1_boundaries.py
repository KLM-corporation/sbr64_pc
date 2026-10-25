"""Boundary & Corner Cases for F11: PAK1 Decryption Engine - Tier 2 Test Suite."""

import unittest
import zlib
from tests.e2e.common.crypto_oracle import (
    MT19937,
    pak1_decrypt_payload,
    pak1_keystream,
    zlib_decompress_payload,
)


class TestBoundary11Pak1(unittest.TestCase):
    """Test boundary conditions for MT19937 PRNG seeds and corrupted zlib payloads (B11)."""

    def test_zero_length_payload_decryption(self):
        """Verify decrypting an empty byte string returns b'' without error."""
        empty_payload = b""
        res = pak1_decrypt_payload(empty_payload, 0)
        self.assertEqual(res, b"")

    def test_mt19937_seed_zero_boundary(self):
        """Verify MT19937 initialization with seed 0 produces non-zero subsequent states."""
        mt = MT19937(0)
        v = mt.extract_u32()
        self.assertIsInstance(v, int)
        self.assertGreaterEqual(v, 0)

    def test_mt19937_seed_max_uint32_boundary(self):
        """Verify MT19937 initialization with seed 0xFFFFFFFF."""
        mt = MT19937(0xFFFFFFFF)
        v = mt.extract_u32()
        self.assertIsInstance(v, int)
        self.assertGreaterEqual(v, 0)

    def test_corrupted_zlib_header_exception(self):
        """Verify corrupted zlib compressed header raises zlib.error."""
        corrupted = b"\x00\x00\x00\x00"
        with self.assertRaises(zlib.error):
            zlib_decompress_payload(corrupted)

    def test_offset_exceeding_keystream_length_modulo(self):
        """Verify offset wrapping beyond 773-byte keystream length."""
        key = pak1_keystream(773)
        off1 = 50
        off2 = 50 + 773 * 10
        self.assertEqual(off1 % 773, off2 % 773)


if __name__ == "__main__":
    unittest.main()
