"""A locked macOS desktop must fail before a long gameplay capture starts."""

import subprocess
import unittest
from unittest.mock import patch

from tools.smoke_macos_gameplay import require_unlocked_capture_session


class CaptureSessionTest(unittest.TestCase):
    @patch("tools.smoke_macos_gameplay.subprocess.run")
    def test_unlocked_session_allows_capture(self, run):
        run.return_value = subprocess.CompletedProcess([], 0, "", "")
        require_unlocked_capture_session()
        self.assertEqual(run.call_args.args[0][0], "swift")

    @patch("tools.smoke_macos_gameplay.subprocess.run")
    def test_locked_session_rejects_capture(self, run):
        run.return_value = subprocess.CompletedProcess([], 2, "", "")
        with self.assertRaisesRegex(RuntimeError, "locked"):
            require_unlocked_capture_session()

    @patch("tools.smoke_macos_gameplay.subprocess.run")
    def test_probe_error_rejects_capture(self, run):
        run.return_value = subprocess.CompletedProcess([], 3, "", "probe failed")
        with self.assertRaisesRegex(RuntimeError, "could not verify"):
            require_unlocked_capture_session()


if __name__ == "__main__":
    unittest.main()
