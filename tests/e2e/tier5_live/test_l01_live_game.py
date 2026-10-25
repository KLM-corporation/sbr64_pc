"""Tier 5 - Live runs of the real sbr64_pc.exe (unlike tiers 1-4, which check contracts and simulations).

Each class launches the game headless with a throw-away save folder and checks what it actually did:
exit code, imports, frame rate, music, a rendered (non-blank) frame and the save files.
Run with:  python tests/e2e/run_tests.py --tier 5    (about 1 minute; up to 4 on a throttled laptop)
"""

import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

from tests.e2e.common.host_oracle import find_pc_binary

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
MUSIC_DIR = os.path.join(PROJECT_ROOT, "data", "music")
GAME_LIB = os.path.join(PROJECT_ROOT, "data", "lib", "libRSBR_arm32.so")
SHOT_FRAME = 900


def run_game(save_dir, frames, extra=()):
    """Runs until `frames` frames were presented (120 s at most: loading is slow on a throttled laptop)."""
    exe = find_pc_binary()
    args = [exe, "--headless", "--verbose", "--frames", str(frames), "--max-seconds", "120", "--save", save_dir,
            *extra]
    p = subprocess.run(args, capture_output=True, text=True, errors="replace", timeout=210)
    return p.returncode, p.stdout, p.stderr


def distinct_colours(bmp_path):
    d = open(bmp_path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    stride = (w * 3 + 3) & ~3
    return len({d[off + y * stride + x: off + y * stride + x + 3] for y in range(0, h, 8) for x in range(0, w * 3, 24)})


@unittest.skipUnless(find_pc_binary(), "sbr64_pc.exe is not built")
@unittest.skipUnless(os.path.exists(GAME_LIB), "game files not imported (tools/import_game_data.ps1)")
class TestL01FreshProfileRun(unittest.TestCase):
    """First launch (no save): boots, renders the front end at 60 FPS with music, writes a profile once."""

    @classmethod
    def setUpClass(cls):
        cls.save_dir = tempfile.mkdtemp(prefix="sbr64_live_")
        cls.shot = os.path.join(cls.save_dir, "shot.bmp")
        cls.code, cls.out, cls.err = run_game(
            cls.save_dir, 1100, ["--screenshot", cls.shot, "--screenshot-frame", str(SHOT_FRAME)])
        # Second session on the same save folder: the profile must be read back.
        cls.code2, cls.out2, cls.err2 = run_game(cls.save_dir, 120)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.save_dir, ignore_errors=True)

    def test_exits_cleanly(self):
        self.assertEqual(self.code, 0, self.err[-2000:])
        self.assertIn("game started", self.out)
        self.assertNotIn("[FATAL]", self.err)
        self.assertNotIn("guest exception", self.err)

    def test_all_imports_resolved(self):
        self.assertIn("0 imports without an implementation", self.out)

    def test_steady_60_fps_after_loading(self):
        rates = [float(m.group(2)) for m in re.finditer(r"frames (\d+): ([\d.]+) fps average", self.out)
                 if int(m.group(1)) >= 600]
        self.assertTrue(rates, "no frame statistics after frame 600")
        self.assertGreaterEqual(sum(rates) / len(rates), 55.0, rates)

    @unittest.skipUnless(os.path.isdir(MUSIC_DIR), "data/music not imported (tools/import_game_data.ps1)")
    def test_front_end_music_is_mixed(self):
        self.assertIn("music: playing", self.out)
        self.assertIn("music: first audio block mixed", self.out)

    def test_rendered_frame_is_not_blank(self):
        self.assertTrue(os.path.exists(self.shot), "screenshot not written")
        self.assertGreater(distinct_colours(self.shot), 100, "frame %d is (nearly) uniform" % SHOT_FRAME)

    def test_profile_written_without_save_loop(self):
        writes = len(re.findall(r"rsbra\.sav, wb\)", self.out))
        self.assertGreaterEqual(writes, 1)
        self.assertLessEqual(writes, 10, "profile rewritten %d times in 1100 frames" % writes)
        self.assertEqual(os.path.getsize(os.path.join(self.save_dir, "rsbra.sav")), 3800)

    def test_profile_read_back_next_session(self):
        self.assertEqual(self.code2, 0, self.err2[-2000:])
        # The game reads rsbrb.sav (written together with rsbra.sav) first and falls back to rsbra.sav.
        read_ok = re.search(r"rsbr[ab]\.sav, rb\) -> (?!0{16})[0-9A-Fa-f]{16}", self.out2)
        self.assertIsNotNone(read_ok, "the second session did not open the saved profile")


@unittest.skipUnless(os.path.exists(os.path.join(PROJECT_ROOT, "build", "test_music_player.exe")),
                     "test_music_player.exe is not built")
@unittest.skipUnless(os.path.isdir(MUSIC_DIR), "data/music not extracted")
class TestL02MusicDecoder(unittest.TestCase):
    def test_decoder_unit_test_passes(self):
        exe = os.path.join(PROJECT_ROOT, "build", "test_music_player.exe")
        p = subprocess.run([exe, MUSIC_DIR], capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stdout)


if __name__ == "__main__":
    unittest.main()
