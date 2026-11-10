import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class NativePlayLauncherTest(unittest.TestCase):
    def invoke(self, *, framerate=0, running=False):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "test repo"
            (root / "tools").mkdir(parents=True)
            profile = root / "data/native-rt-profile"
            profile.mkdir(parents=True)
            (profile / "settings.txt").write_text(
                f"window = 1\nresolution = 2\nframerate = {framerate}\ntexturepack = 0\n")
            (profile / "lighting.txt").write_text("occlusionquality = 3\n")
            original = Path(__file__).resolve().parent.parent / "tools/play_macos_native_rt.sh"
            launcher = root / "tools/play.sh"
            shutil.copyfile(original, launcher)
            output = root / "args.json"
            python = root / "fake-python"
            python.write_text(f"#!{sys.executable}\nimport json, os, sys\n"
                              "if sys.argv[1] != '-m':\n"
                              " open(os.environ['PLAY_TEST_ARGS'], 'w').write(json.dumps(sys.argv[1:]))\n")
            python.chmod(0o755)
            pgrep = root / "pgrep"
            pgrep.write_text(f"#!/bin/sh\nexit {0 if running else 1}\n")
            pgrep.chmod(0o755)
            environment = os.environ | {"PYTHON_BIN": str(python), "PLAY_TEST_ARGS": str(output),
                                        "PATH": str(root) + os.pathsep + os.environ["PATH"]}
            result = subprocess.run(["bash", str(launcher)], cwd=temp, env=environment,
                                    capture_output=True, text=True)
            return result, json.loads(output.read_text()) if output.exists() else None

    def test_native_preset_uses_real_controls_and_bounded_watchdog(self):
        result, args = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--interactive", args)
        self.assertNotIn("--enter-save", args)
        self.assertEqual(args[args.index("--rt-level") + 1], "2")
        self.assertEqual(args[args.index("--max-rss-mib") + 1], "1536")
        self.assertEqual(args[args.index("--seconds") + 1], "1800")

    def test_untested_high_rate_preset_is_rejected_before_launch(self):
        result, args = self.invoke(framerate=3)
        self.assertNotEqual(result.returncode, 0)
        self.assertIsNone(args)
        self.assertIn("tested preset", result.stderr)

    def test_second_game_instance_is_rejected(self):
        result, args = self.invoke(running=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIsNone(args)
        self.assertIn("already running", result.stderr)
