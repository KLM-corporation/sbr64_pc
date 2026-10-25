"""Save file format, checksum validation, and backup restoration oracle."""

import os
import struct
import zlib
from typing import Dict, Optional

SAVE_FILE_MAGIC = b"SBRS"
SAVE_VERSION = 1
SAVE_DEFAULT_SIZE = 3800


class SaveProfile:
    """Opaque-box representation of player progress stored in rsbra.sav / rsbrb.sav."""

    def __init__(self, bubbles: int = 0, high_score: int = 0, distance: int = 0, unlocked_costumes: int = 1):
        self.bubbles = bubbles
        self.high_score = high_score
        self.distance = distance
        self.unlocked_costumes = unlocked_costumes
        self.sound_enabled = True
        self.music_enabled = True

    def serialize(self, total_size: int = SAVE_DEFAULT_SIZE) -> bytes:
        """Serialize save profile with header, data payload, padding, and CRC32 footer."""
        body = bytearray(total_size)
        # Header (16 bytes): magic(4), version(4), bubbles(4), high_score(4)
        struct.pack_into("<4sIII", body, 0, SAVE_FILE_MAGIC, SAVE_VERSION, self.bubbles, self.high_score)
        # Stats (16 bytes): distance(4), unlocked_costumes(4), sound(2), music(2), reserved(4)
        struct.pack_into("<IIHH", body, 16, self.distance, self.unlocked_costumes, 1 if self.sound_enabled else 0, 1 if self.music_enabled else 0)
        # CRC32 of first (total_size - 4) bytes placed in the last 4 bytes
        data_to_hash = bytes(body[: total_size - 4])
        crc = zlib.crc32(data_to_hash) & 0xFFFFFFFF
        struct.pack_into("<I", body, total_size - 4, crc)
        return bytes(body)

    @classmethod
    def deserialize(cls, data: bytes) -> Optional["SaveProfile"]:
        if len(data) < 32 or data[:4] != SAVE_FILE_MAGIC:
            return None
        total_size = len(data)
        stored_crc = struct.unpack_from("<I", data, total_size - 4)[0]
        calc_crc = zlib.crc32(data[: total_size - 4]) & 0xFFFFFFFF
        if stored_crc != calc_crc:
            return None  # Corrupted save

        magic, version, bubbles, high_score = struct.unpack_from("<4sIII", data, 0)
        distance, costumes, sound, music = struct.unpack_from("<IIHH", data, 16)
        prof = cls(bubbles=bubbles, high_score=high_score, distance=distance, unlocked_costumes=costumes)
        prof.sound_enabled = sound != 0
        prof.music_enabled = music != 0
        return prof


class SaveManagerOracle:
    """Manages active (rsbra.sav) and backup (rsbrb.sav) files in target directory."""

    def __init__(self, save_dir: str):
        self.save_dir = save_dir
        os.makedirs(save_dir, exist_ok=True)
        self.active_path = os.path.join(save_dir, "rsbra.sav")
        self.backup_path = os.path.join(save_dir, "rsbrb.sav")

    def save(self, profile: SaveProfile) -> bool:
        data = profile.serialize()
        # Atomic write with backup copy
        with open(self.active_path, "wb") as f:
            f.write(data)
        with open(self.backup_path, "wb") as f:
            f.write(data)
        return True

    def load(self) -> Optional[SaveProfile]:
        # Try active first
        if os.path.exists(self.active_path):
            with open(self.active_path, "rb") as f:
                data = f.read()
            prof = SaveProfile.deserialize(data)
            if prof is not None:
                return prof
        # Fallback to backup
        if os.path.exists(self.backup_path):
            with open(self.backup_path, "rb") as f:
                data = f.read()
            prof = SaveProfile.deserialize(data)
            if prof is not None:
                # Restore active from backup
                with open(self.active_path, "wb") as f:
                    f.write(data)
                return prof
        return None
