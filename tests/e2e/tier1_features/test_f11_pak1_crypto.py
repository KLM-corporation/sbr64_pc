"""Feature 11: PAK1 Decryption Engine - Tier 1 Test Suite."""

import unittest
from tests.e2e.common.crypto_oracle import (
    PAK1_DEFAULT_SEED,
    PAK1_KEYSTREAM_LEN,
    PAK1_MAGIC,
    MT19937,
    pak1_decrypt_payload,
    pak1_keystream,
    zlib_compress_payload,
    zlib_decompress_payload,
)


class TestFeature11Pak1Crypto(unittest.TestCase):
    """Test PAK1 MT19937 PRNG cipher and zlib decompression (F11)."""

    def test_mt19937_seed_baff1ed_determinism(self):
        """Verify MT19937 PRNG generates repeatable sequence with seed 0x0BAFF1ED."""
        mt1 = MT19937(PAK1_DEFAULT_SEED)
        mt2 = MT19937(PAK1_DEFAULT_SEED)
        v1 = [mt1.extract_u32() for _ in range(10)]
        v2 = [mt2.extract_u32() for _ in range(10)]
        self.assertEqual(v1, v2)

    def test_pak1_magic_bytes(self):
        """Verify PAK1 file magic is b'1KAP' (0x50414B31 in little-endian)."""
        self.assertEqual(PAK1_MAGIC, b"1KAP")
        magic_int = int.from_bytes(PAK1_MAGIC, "little")
        self.assertEqual(magic_int, 0x50414B31)

    def test_pak1_keystream_length_773(self):
        """Verify generated PAK1 keystream buffer length is exactly 773 bytes."""
        key = pak1_keystream()
        self.assertEqual(len(key), PAK1_KEYSTREAM_LEN)

    def test_pak1_stream_xor_decrypt(self):
        """Verify stream XOR decryption is invertible: decrypt(encrypt(data)) == data."""
        original = b"GameConfigTextPayload12345!@#$%"
        key = pak1_keystream()
        offset = 0x81C0
        encrypted = pak1_decrypt_payload(original, offset, key)
        self.assertNotEqual(original, encrypted)
        decrypted = pak1_decrypt_payload(encrypted, offset, key)
        self.assertEqual(original, decrypted)

    def test_zlib_decompression_rfc1950(self):
        """Verify standard zlib deflate/inflate round-trip for asset payloads."""
        payload = b"A" * 1024 + b"B" * 1024
        compressed = zlib_compress_payload(payload)
        self.assertLess(len(compressed), len(payload))
        decompressed = zlib_decompress_payload(compressed)
        self.assertEqual(decompressed, payload)


if __name__ == "__main__":
    unittest.main()
