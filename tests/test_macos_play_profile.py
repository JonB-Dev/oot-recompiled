"""A local play profile must preserve the normal save."""

import tempfile
import unittest
from pathlib import Path

from tests.test_macos_playable_save import PlayableSaveTest
from tools.init_macos_play import initialize_profile


class PlayProfileTest(unittest.TestCase):
    def test_native_tracing_profile_keeps_original_rate_and_bounds_quality(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "source.bin"
            profile = Path(temp) / "native"
            source_bytes = bytes(PlayableSaveTest().make_sram())
            source.write_bytes(source_bytes)
            initialize_profile(source, profile, native_rt=True)
            self.assertEqual(source.read_bytes(), source_bytes)
            settings = (profile / "settings.txt").read_text()
            for line in ("window = 1", "resolution = 2", "framerate = 0", "texturepack = 0"):
                self.assertIn(line, settings)
            self.assertIn("occlusionquality = 3", (profile / "lighting.txt").read_text())
            (profile / "settings.txt").write_text("later settings")
            with self.assertRaises(FileExistsError):
                initialize_profile(source, profile, native_rt=True)
            self.assertEqual((profile / "settings.txt").read_text(), "later settings")

    def test_creates_safe_persistent_profile_without_changing_source(self):
        source_bytes = bytes(PlayableSaveTest().make_sram())
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "source.bin"
            profile = Path(temp) / "profile"
            source.write_bytes(source_bytes)
            initialize_profile(source, profile)
            self.assertEqual(source.read_bytes(), source_bytes)
            self.assertEqual(int.from_bytes(
                (profile / "saves/oot.n64.us.1.0.bin").read_bytes()[0x20:0x24], "big"), 0xBB)
            settings = (profile / "settings.txt").read_text()
            for line in ("window = 0", "resolution = 5", "framerate = 0",
                         "texturepack = 0"):
                self.assertIn(line, settings)

    def test_refuses_existing_profile_and_preserves_progress(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "source.bin"
            profile = Path(temp) / "profile"
            source.write_bytes(PlayableSaveTest().make_sram())
            initialize_profile(source, profile)
            save = profile / "saves/oot.n64.us.1.0.bin"
            save.write_bytes(b"later gameplay")
            with self.assertRaises(FileExistsError):
                initialize_profile(source, profile)
            self.assertEqual(save.read_bytes(), b"later gameplay")

    def test_invalid_source_leaves_no_profile(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "source.bin"
            profile = Path(temp) / "profile"
            source.write_bytes(b"bad")
            with self.assertRaises(ValueError):
                initialize_profile(source, profile)
            self.assertFalse(profile.exists())
