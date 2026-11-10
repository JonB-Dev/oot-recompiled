"""A living game process is not a passing smoke test after a Metal fault."""

import subprocess
import unittest
from datetime import datetime
from unittest.mock import patch

from tools.smoke_macos_gameplay import metal_gpu_errors, shader_validation_environment


class MetalFaultDetectionTest(unittest.TestCase):
    def test_shader_validation_aborts_and_reports_faults(self):
        original = {"PATH": "/bin"}
        configured = shader_validation_environment(original)
        self.assertEqual(original, {"PATH": "/bin"})
        self.assertEqual(configured["MTL_SHADER_VALIDATION"], "1")
        self.assertEqual(configured["MTL_SHADER_VALIDATION_REPORT_TO_STDERR"], "1")
        self.assertEqual(configured["MTL_SHADER_VALIDATION_ABORT_ON_FAULT"], "1")

    @patch("tools.smoke_macos_gameplay.subprocess.run")
    def test_rejects_metal_command_buffer_errors(self, run):
        run.return_value = subprocess.CompletedProcess([], 0,
            "Timestamp Ty Process[PID:TID]\n"
            "2026-11-09 09:32:00 E OoTRecompiled[80602:1] "
            "kIOGPUCommandBufferCallbackErrorSubmissionsIgnored\n", "")
        self.assertTrue(metal_gpu_errors(80602, datetime(2026, 11, 9, 9, 31)))
        self.assertIn("processID == 80602", run.call_args.args[0][-1])

    @patch("tools.smoke_macos_gameplay.subprocess.run")
    def test_accepts_empty_driver_log(self, run):
        run.return_value = subprocess.CompletedProcess([], 0,
            "Timestamp Ty Process[PID:TID]\n", "")
        self.assertFalse(metal_gpu_errors(80602, datetime(2026, 11, 9, 9, 31)))

    @patch("tools.smoke_macos_gameplay.subprocess.run")
    def test_log_query_failure_is_not_a_pass(self, run):
        run.return_value = subprocess.CompletedProcess([], 1, "", "log unavailable")
        with self.assertRaises(RuntimeError):
            metal_gpu_errors(80602, datetime(2026, 11, 9, 9, 31))
