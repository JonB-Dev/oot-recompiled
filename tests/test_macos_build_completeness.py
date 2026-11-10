"""Regression: fresh app builds must produce every registered test executable."""

import json
import subprocess
import unittest
from pathlib import Path


class MacosBuildCompletenessTest(unittest.TestCase):
    def test_fresh_builds_produce_all_registered_test_executables(self):
        root = Path(__file__).resolve().parents[1]
        for relative in ("build-cmake", "build/rt-compile-probe"):
            with self.subTest(build=relative):
                result = subprocess.run(
                    ["ctest", "--test-dir", str(root / relative), "--show-only=json-v1"],
                    capture_output=True, text=True, check=True)
                tests = json.loads(result.stdout)["tests"]
                self.assertTrue(tests, "no native tests were registered")
                for test in tests:
                    command = test.get("command", [])
                    self.assertTrue(command and Path(command[0]).is_file(),
                                    f"{relative}: {test['name']} was registered but not built")
