"""VFS and PAK1 archive mount oracle for sbr64_pc test suite."""

import os
import struct
import zlib
from typing import Dict, List, Optional, Tuple
from tests.e2e.common.crypto_oracle import (
    PAK1_DEFAULT_SEED,
    PAK1_KEYSTREAM_LEN,
    PAK1_MAGIC,
    normalize_vfs_path,
    pak1_decrypt_payload,
    pak1_keystream,
    vfs_path_crc32,
    zlib_decompress_payload,
)

from tests.e2e.common.game_data import MAIN_OBB_PATH, PATCH_OBB_PATH


class PakEntry:
    def __init__(self, crc32_hash: int, next_idx: int, uncompressed_size: int, compressed_size: int, file_offset: int):
        self.crc32_hash = crc32_hash
        self.next_idx = next_idx
        self.uncompressed_size = uncompressed_size
        self.compressed_size = compressed_size
        self.file_offset = file_offset

    def is_compressed(self) -> bool:
        return self.compressed_size < self.uncompressed_size


class PakArchiveReader:
    """Opaque-box reader and validator for PAK1 format archives."""

    def __init__(self, file_path: str, seed: int = PAK1_DEFAULT_SEED):
        self.file_path = file_path
        self.seed = seed
        self.num_entries = 0
        self.data_start_offset = 0
        self.entries_by_crc: Dict[int, PakEntry] = {}
        self.entries_list: List[PakEntry] = []
        self._key = pak1_keystream(PAK1_KEYSTREAM_LEN, seed)
        self.valid = False

        if os.path.exists(file_path):
            self._load_header()

    def _load_header(self):
        with open(self.file_path, "rb") as f:
            hdr = f.read(12)
            if len(hdr) < 12:
                return
            magic, num_entries, data_start = struct.unpack("<4sII", hdr)
            if magic != PAK1_MAGIC:
                return
            self.num_entries = num_entries
            self.data_start_offset = data_start

            f.seek(0x40C)
            entry_bytes = f.read(num_entries * 20)
            for i in range(num_entries):
                chunk = entry_bytes[i * 20 : (i + 1) * 20]
                if len(chunk) == 20:
                    crc, next_idx, uncomp, comp, offset = struct.unpack("<IIIII", chunk)
                    entry = PakEntry(crc, next_idx, uncomp, comp, offset)
                    self.entries_by_crc[crc] = entry
                    self.entries_list.append(entry)
            self.valid = True

    def read_payload_raw(self, entry: PakEntry) -> bytes:
        """Read raw (still encrypted/compressed) bytes from archive."""
        with open(self.file_path, "rb") as f:
            f.seek(entry.file_offset)
            return f.read(entry.compressed_size)

    def read_payload_decrypted(self, entry: PakEntry) -> bytes:
        """Read and decrypt payload using MT19937 keystream."""
        raw = self.read_payload_raw(entry)
        return pak1_decrypt_payload(raw, entry.file_offset, self._key)

    def extract_entry(self, entry: PakEntry) -> bytes:
        """Read, decrypt, and decompress payload."""
        dec = self.read_payload_decrypted(entry)
        if entry.is_compressed():
            return zlib_decompress_payload(dec)
        return dec

    def extract_by_path(self, virtual_path: str) -> Optional[bytes]:
        crc = vfs_path_crc32(virtual_path)
        entry = self.entries_by_crc.get(crc)
        if entry is None:
            return None
        return self.extract_entry(entry)


class VfsMount:
    """Composite VFS mounting main OBB and overlay patch OBB with local save redirection."""

    def __init__(self, main_obb_path: str = MAIN_OBB_PATH, patch_obb_path: str = PATCH_OBB_PATH, save_dir: str = r".\save"):
        self.main_archive: Optional[PakArchiveReader] = None
        self.patch_archive: Optional[PakArchiveReader] = None
        self.save_dir = save_dir

        if os.path.exists(patch_obb_path):
            self.patch_archive = PakArchiveReader(patch_obb_path)
        if os.path.exists(main_obb_path):
            self.main_archive = PakArchiveReader(main_obb_path)

    def resolve_asset(self, virtual_path: str) -> Optional[bytes]:
        crc = vfs_path_crc32(virtual_path)
        # Check patch overlay first
        if self.patch_archive and crc in self.patch_archive.entries_by_crc:
            return self.patch_archive.extract_entry(self.patch_archive.entries_by_crc[crc])
        # Fall back to main archive
        if self.main_archive and crc in self.main_archive.entries_by_crc:
            return self.main_archive.extract_entry(self.main_archive.entries_by_crc[crc])
        return None

    def exists(self, virtual_path: str) -> bool:
        crc = vfs_path_crc32(virtual_path)
        if self.patch_archive and crc in self.patch_archive.entries_by_crc:
            return True
        if self.main_archive and crc in self.main_archive.entries_by_crc:
            return True
        return False
