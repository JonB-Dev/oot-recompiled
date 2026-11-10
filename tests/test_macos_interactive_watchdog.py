import subprocess
import unittest
from datetime import datetime
from unittest.mock import Mock, patch

from tools.smoke_macos_gameplay import monitor_game


class InteractiveWatchdogTest(unittest.TestCase):
    def test_user_quit_is_success_only_for_interactive_play(self):
        game = Mock(pid=123)
        game.poll.return_value = 0
        self.assertEqual(monitor_game(game, 10, 1536, datetime.now(), interactive=True), 0)
        with self.assertRaisesRegex(RuntimeError, "app exited 0"):
            monitor_game(game, 10, 1536, datetime.now())

    def test_user_play_still_rejects_crashes(self):
        game = Mock(pid=123)
        game.poll.return_value = -11
        with self.assertRaisesRegex(RuntimeError, "app exited -11"):
            monitor_game(game, 10, 1536, datetime.now(), interactive=True)

    @patch("tools.smoke_macos_gameplay.time.sleep")
    @patch("tools.smoke_macos_gameplay.subprocess.run")
    @patch("tools.smoke_macos_gameplay.time.monotonic", return_value=0)
    def test_play_records_memory_and_accepts_clean_quit(self, clock, run, sleep):
        game = Mock(pid=123)
        game.poll.side_effect = [None, 0, 0]
        run.return_value = subprocess.CompletedProcess([], 0, "102400\n", "")
        self.assertEqual(monitor_game(game, 10, 1536, datetime.now(), interactive=True), 102400)

    @patch("tools.smoke_macos_gameplay.subprocess.run")
    @patch("tools.smoke_macos_gameplay.time.monotonic", return_value=0)
    def test_interactive_play_keeps_the_memory_limit(self, clock, run):
        game = Mock(pid=123)
        game.poll.return_value = None
        run.return_value = subprocess.CompletedProcess([], 0, "2097152\n", "")
        with self.assertRaisesRegex(RuntimeError, "RSS exceeded 1536"):
            monitor_game(game, 10, 1536, datetime.now(), interactive=True)

    @patch("tools.smoke_macos_gameplay.metal_gpu_errors", return_value=True)
    @patch("tools.smoke_macos_gameplay.time.monotonic", side_effect=[0, 0, 0, 4])
    def test_interactive_play_keeps_gpu_fault_detection(self, clock, errors):
        game = Mock(pid=123)
        game.poll.return_value = None
        with self.assertRaisesRegex(RuntimeError, "Metal GPU errors"):
            monitor_game(game, 10, 1536, datetime.now(), interactive=True)

    @patch("tools.smoke_macos_gameplay.time.monotonic", side_effect=[0, 0, 11])
    def test_interactive_session_has_a_deadline(self, clock):
        game = Mock(pid=123)
        game.poll.return_value = None
        self.assertEqual(monitor_game(game, 10, 1536, datetime.now(), interactive=True), 0)
        game.poll.assert_called_once()

    @patch("tools.smoke_macos_gameplay.subprocess.run")
    @patch("tools.smoke_macos_gameplay.time.monotonic", side_effect=[0, 0, 0, 0, 11])
    def test_missing_memory_measurement_is_not_accepted(self, clock, run):
        game = Mock(pid=123)
        game.poll.return_value = None
        run.return_value = subprocess.CompletedProcess([], 1, "", "unavailable")
        with self.assertRaisesRegex(RuntimeError, "inspect game RSS"):
            monitor_game(game, 10, 1536, datetime.now(), interactive=True)

    @patch("tools.smoke_macos_gameplay.time.monotonic", side_effect=[0, 0, 11])
    def test_crash_at_deadline_is_not_success(self, clock):
        game = Mock(pid=123)
        game.poll.return_value = -11
        with self.assertRaisesRegex(RuntimeError, "app exited -11"):
            monitor_game(game, 10, 1536, datetime.now())

    @patch("tools.smoke_macos_gameplay.datetime")
    @patch("tools.smoke_macos_gameplay.time.sleep")
    @patch("tools.smoke_macos_gameplay.subprocess.run")
    @patch("tools.smoke_macos_gameplay.metal_gpu_errors", return_value=False)
    @patch("tools.smoke_macos_gameplay.time.monotonic",
           side_effect=[0, 0, 4, 4, 4, 8, 8, 8, 11])
    def test_gpu_queries_use_overlapping_recent_windows(self, clock, errors, run, sleep, now):
        start = datetime(2026, 11, 9, 12, 0)
        now.now.return_value = datetime(2026, 11, 9, 12, 1)
        game = Mock(pid=123)
        game.poll.return_value = None
        run.return_value = subprocess.CompletedProcess([], 0, "1024\n", "")
        monitor_game(game, 10, 1536, start)
        self.assertEqual(errors.call_args_list[0].args, (123, start))
        self.assertEqual(errors.call_args_list[1].args,
                         (123, datetime(2026, 11, 9, 12, 0, 50)))
