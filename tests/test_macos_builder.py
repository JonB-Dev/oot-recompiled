import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('builder', Path(__file__).parents[1] / 'tools/macos_builder.py')
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)

class BuilderTests(unittest.TestCase):
    def test_wrong_rom_is_rejected_before_build(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'wrong.z64'
            p.write_bytes(b'not a rom')
            with self.assertRaisesRegex(ValueError, 'USA.*1.0'):
                b.verify_rom(p)

    def test_missing_rom_is_explained(self):
        with self.assertRaisesRegex(ValueError, 'Choose'):
            b.verify_rom(Path('/nonexistent/rom.z64'))

    def test_rom_hash_is_streamed_and_checked(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'a ROM; $(echo unsafe).z64'
            p.write_bytes(b'fixture')
            with patch.object(b, 'ROM_SHA1', '51cff3c1f0bc59f6187e7040cc12a4e9b1eca7aa'):
                b.verify_rom(p)

    def test_build_plan_preserves_paths_as_arguments(self):
        root = Path('/a source tree')
        rom = Path('/a ROM;$(no).z64')
        plan = b.build_plan(root, rom, True)
        self.assertEqual(plan[1][1][-1], str(rom))
        self.assertIn('build_macos_rt_probe.sh', plan[-1][1][-1])
        self.assertEqual(len(b.build_plan(root, rom, False)), 2)

    def test_lock_prevents_duplicate_builds_and_releases_on_error(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            with b.build_lock(root):
                with self.assertRaisesRegex(ValueError, 'already'):
                    with b.build_lock(root):
                        pass
            with b.build_lock(root):
                pass

    def test_zip_source_is_isolated_from_downloaded_files(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            self.assertEqual(b.source_folder(root), root / 'build/source')
            (root / '.git').mkdir()
            self.assertEqual(b.source_folder(root), root)

    def test_partial_source_download_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / 'build/source').mkdir(parents=True)
            with self.assertRaisesRegex(ValueError, 'incomplete'):
                b.prepare_source(root)

    def test_source_from_unexpected_remote_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / 'build/source/.git').mkdir(parents=True)
            with patch.object(b.subprocess, 'check_output', return_value='https://untrusted.example/repo'):
                with self.assertRaisesRegex(ValueError, 'unexpected origin'):
                    b.prepare_source(root)

    def test_failed_stage_is_not_reported_as_success(self):
        with self.assertRaises(b.subprocess.CalledProcessError):
            b.run_command([b.sys.executable, '-c', 'raise SystemExit(9)'])

    def test_cancel_stops_active_child(self):
        child = unittest.mock.Mock()
        child.pid = 987654
        child.wait.side_effect = [KeyboardInterrupt(), 0]
        child.poll.return_value = None
        with patch.object(b.subprocess, 'Popen', return_value=child), patch.object(b.os, 'killpg') as kill:
            with self.assertRaises(KeyboardInterrupt):
                b.run_command(['fake-compiler'])
            kill.assert_called_once_with(child.pid, b.signal.SIGTERM)

if __name__ == '__main__':
    unittest.main()
