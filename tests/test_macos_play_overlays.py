"""Play must not draw decorative window borders or opt-in diagnostic plates."""

import re
import unittest
from pathlib import Path

from tools.init_macos_play import NATIVE_RT_SETTINGS, SETTINGS


ROOT = Path(__file__).resolve().parents[1]


class PlayOverlaysTest(unittest.TestCase):
    def test_game_frame_border_is_hidden_only_on_macos(self):
        document = (ROOT / "assets/ui/frame.rml").read_text()
        shell = (ROOT / "src/ui/ui_shell.cpp").read_text()
        self.assertIn('id="frame-edge"', document)
        self.assertIsNotNone(re.search(
            r"#if defined\(__APPLE__\)\s+const bool edge = false;\s+#else\s+"
            r"const bool edge = !fullscreen;\s+#endif", shell))
        self.assertIn("const bool up = header || edge;", shell)
        self.assertIn('id="win-close"', document)

    def test_play_profiles_start_without_fps_overlay(self):
        for settings in (SETTINGS, NATIVE_RT_SETTINGS):
            with self.subTest(settings=settings):
                self.assertIn("counter = 0\n", settings)
                self.assertNotIn("counter = 1\n", settings)
