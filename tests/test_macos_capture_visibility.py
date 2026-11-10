"""A recorded black frame must not be accepted as gameplay evidence."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.smoke_macos_gameplay import is_visible_png


@unittest.skipUnless(shutil.which("ffmpeg"), "ffmpeg is needed for capture validation")
class CaptureVisibilityTest(unittest.TestCase):
    def test_black_frame_is_rejected_and_bright_frame_is_visible(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertFalse(is_visible_png(Path(directory) / "missing.png"))
            for color, expected in (("black", False), ("white", True)):
                frame = Path(directory) / f"{color}.png"
                subprocess.run(
                    ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                     "-f", "lavfi", "-i", f"color=c={color}:s=16x16",
                     "-frames:v", "1", str(frame)],
                    check=True,
                )
                self.assertEqual(is_visible_png(frame), expected, color)


if __name__ == "__main__":
    unittest.main()
