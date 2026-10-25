"""Locations of the original game files the oracles read.

The repository's own data folder (filled by tools/import_game_data.ps1) is preferred; the original
reverse-engineering workspace C:\\sbr64\\work is the fallback. Tests check that files exist before reading them.
"""

import os

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
DATA_DIR = os.path.join(PROJECT_ROOT, "data")
LEGACY_WORK_DIR = r"C:\sbr64\work"


def game_path(data_relative: str, legacy_relative: str) -> str:
    """Path under data/ when it exists there, otherwise under C:\\sbr64\\work."""
    local = os.path.join(DATA_DIR, *data_relative.split("/"))
    if os.path.exists(local):
        return local
    return os.path.join(LEGACY_WORK_DIR, *legacy_relative.split("/"))


LIB_RSBR_PATH = game_path("lib/libRSBR_arm32.so", "libRSBR_arm32.so")
MAIN_OBB_PATH = game_path("obb/main.25.com.playstation.runsackboyrun.obb",
                          "obb/main.25.com.playstation.runsackboyrun.obb")
PATCH_OBB_PATH = game_path("obb/patch.42.com.playstation.runsackboyrun.obb",
                           "obb/patch.42.com.playstation.runsackboyrun.obb")
# The APK itself is not copied into data/: point SBR64_APK at it to run the APK checks.
APK_PATH = os.environ.get("SBR64_APK", os.path.join(LEGACY_WORK_DIR, "original.apk"))
