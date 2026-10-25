"""Cryptographic, compression, and hashing oracles for sbr64_pc test suite."""

import struct
import zlib


class MT19937:
    """Standard 32-bit MT19937 Mersenne Twister PRNG matching C++ implementation."""

    W, N, M, R = 32, 624, 397, 31
    A = 0x9908B0DF
    U, D = 11, 0xFFFFFFFF
    S, B = 7, 0x9D2C5680
    T, C = 15, 0xEFC60000
    L = 18
    F = 1812433253

    LOWER_MASK = (1 << R) - 1
    UPPER_MASK = (~LOWER_MASK) & 0xFFFFFFFF

    def __init__(self, seed: int = 0x0BAFF1ED):
        self.mt = [0] * self.N
        self.index = self.N
        self.seed(seed)

    def seed(self, seed: int):
        self.mt[0] = seed & 0xFFFFFFFF
        for i in range(1, self.N):
            self.mt[i] = (self.F * (self.mt[i - 1] ^ (self.mt[i - 1] >> (self.W - 2))) + i) & 0xFFFFFFFF
        self.index = self.N

    def extract_u32(self) -> int:
        if self.index >= self.N:
            self._twist()
        y = self.mt[self.index]
        self.index += 1
        y ^= (y >> self.U) & self.D
        y ^= (y << self.S) & self.B
        y ^= (y << self.T) & self.C
        y ^= (y >> self.L)
        return y & 0xFFFFFFFF

    def _twist(self):
        for i in range(self.N):
            x = (self.mt[i] & self.UPPER_MASK) + (self.mt[(i + 1) % self.N] & self.LOWER_MASK)
            xA = x >> 1
            if x % 2 != 0:
                xA ^= self.A
            self.mt[i] = self.mt[(i + self.M) % self.N] ^ xA
        self.index = 0

    def generate_keystream(self, length: int) -> bytes:
        """Generate keystream matching Pak::Archive: 1 byte per Mersenne Twister call."""
        out = bytearray(length)
        for i in range(length):
            out[i] = self.extract_u32() & 0xFF
        return bytes(out)


PAK1_DEFAULT_SEED = 0x0BAFF1ED
PAK1_MAGIC = b"1KAP"  # 0x314b4150
PAK1_KEYSTREAM_LEN = 773


def pak1_keystream(length: int = PAK1_KEYSTREAM_LEN, seed: int = PAK1_DEFAULT_SEED) -> bytes:
    """Generate deterministic PAK1 keystream using MT19937."""
    mt = MT19937(seed)
    return mt.generate_keystream(length)


def pak1_decrypt_payload(raw: bytes, file_offset: int, key: bytes = None) -> bytes:
    """Decrypt PAK1 payload given absolute file offset in archive."""
    if key is None:
        key = pak1_keystream()
    klen = len(key)
    dec = bytearray(len(raw))
    for i, b in enumerate(raw):
        dec[i] = b ^ key[(file_offset + i) % klen]
    return bytes(dec)


def xor_crypt(data: bytes, key: bytes) -> bytes:
    """XOR data with repeated key."""
    if not key or not data:
        return data
    klen = len(key)
    res = bytearray(len(data))
    for i, b in enumerate(data):
        res[i] = b ^ key[i % klen]
    return bytes(res)


def normalize_vfs_path(path: str) -> str:
    """Normalize VFS path to lowercase forward-slash representation."""
    return path.replace("\\", "/").strip("/").lower()


def vfs_path_crc32(path: str) -> int:
    """Calculate CRC32 of normalized VFS path string."""
    norm = normalize_vfs_path(path).encode("utf-8")
    return zlib.crc32(norm) & 0xFFFFFFFF


def zlib_decompress_payload(compressed: bytes) -> bytes:
    """Decompress zlib payload."""
    return zlib.decompress(compressed)


def zlib_compress_payload(raw: bytes, level: int = 6) -> bytes:
    """Compress payload using zlib deflate."""
    return zlib.compress(raw, level=level)
