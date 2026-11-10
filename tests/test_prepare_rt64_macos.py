"""The renderer series must be repeatable and never mutate its pinned source."""

import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.prepare_rt64_macos import prepare


class PrepareRt64Tests(unittest.TestCase):
    def setUp(self):
        # Keep this under the checkout: git apply behaves differently when a parent
        # repository exists, which is the production layout we need to exercise.
        test_root = Path(__file__).resolve().parent.parent / "build/prepare-rt64-tests"
        test_root.mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=test_root)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.destination = self.root / "build" / "rt64-src"
        self.series = self.root / "series"
        self.series.mkdir()
        subprocess.run(["git", "init", "-q", str(self.source)], check=True)
        (self.source / "CMakeLists.txt").write_text("base\n")
        subprocess.run(["git", "-C", str(self.source), "add", "."], check=True)
        subprocess.run(
            ["git", "-C", str(self.source), "-c", "user.name=Test",
             "-c", "user.email=test@example.invalid", "commit", "-qm", "base"], check=True
        )
        self.pin = subprocess.check_output(
            ["git", "-C", str(self.source), "rev-parse", "HEAD"], text=True
        ).strip()

    def write_patch(self, contents: str):
        target = self.source / "CMakeLists.txt"
        target.write_text(contents)
        diff = subprocess.check_output(
            ["git", "-C", str(self.source), "diff", "--", "CMakeLists.txt"], text=True
        )
        (self.series / "0001-change.patch").write_text(diff)
        target.write_text("base\n")

    def test_applies_series_repeatably_without_mutating_pin(self):
        self.write_patch("base\npatched\n")
        self.assertEqual(prepare(self.source, self.destination, self.series, self.pin), 1)
        first_stat = (self.destination / "CMakeLists.txt").stat().st_mtime_ns
        self.assertEqual(prepare(self.source, self.destination, self.series, self.pin), 1)
        self.assertEqual((self.destination / "CMakeLists.txt").stat().st_mtime_ns, first_stat)
        self.assertEqual((self.destination / "CMakeLists.txt").read_text(), "base\npatched\n")
        self.assertEqual((self.source / "CMakeLists.txt").read_text(), "base\n")
        self.assertFalse((self.destination / ".git").exists())
        (self.destination / "CMakeLists.txt").write_text("hand edit\n")
        self.assertEqual(prepare(self.source, self.destination, self.series, self.pin), 1)
        self.assertEqual((self.destination / "CMakeLists.txt").read_text(), "base\npatched\n")

    def test_macos_patches_follow_common_series_and_invalidate_cache(self):
        self.write_patch("base\npatched\n")
        macos = self.series / "macos"
        macos.mkdir()
        patch = macos / "0001-macos.patch"
        prefix = "--- a/CMakeLists.txt\n+++ b/CMakeLists.txt\n@@ -1,2 +1,3 @@\n base\n patched\n+"
        patch.write_text(prefix + "macos\n")
        self.assertEqual(prepare(self.source, self.destination, self.series, self.pin), 2)
        self.assertEqual((self.destination / "CMakeLists.txt").read_text(),
                         "base\npatched\nmacos\n")
        patch.write_text(prefix + "updated\n")
        self.assertEqual(prepare(self.source, self.destination, self.series, self.pin), 2)
        self.assertEqual((self.destination / "CMakeLists.txt").read_text(),
                         "base\npatched\nupdated\n")
        self.assertEqual((self.source / "CMakeLists.txt").read_text(), "base\n")

    def test_pin_mismatch_does_not_touch_existing_build(self):
        self.destination.mkdir(parents=True)
        (self.destination / "keep").write_text("keep")
        with self.assertRaisesRegex(RuntimeError, "RT64_PIN_MISMATCH"):
            prepare(self.source, self.destination, self.series, "0" * 40)
        self.assertEqual((self.destination / "keep").read_text(), "keep")

    def test_bad_patch_preserves_existing_build(self):
        self.destination.mkdir(parents=True)
        (self.destination / "keep").write_text("keep")
        (self.series / "0001-bad.patch").write_text("not a patch\n")
        with self.assertRaisesRegex(RuntimeError, "RT64_PATCH_FAILED"):
            prepare(self.source, self.destination, self.series, self.pin)
        self.assertEqual((self.destination / "keep").read_text(), "keep")

    def test_empty_series_preserves_existing_build(self):
        self.destination.mkdir(parents=True)
        (self.destination / "keep").write_text("keep")
        with self.assertRaisesRegex(RuntimeError, "RT64_SERIES_EMPTY"):
            prepare(self.source, self.destination, self.series, self.pin)
        self.assertEqual((self.destination / "keep").read_text(), "keep")

    def test_existing_backup_leaves_no_temporary_copy(self):
        self.write_patch("base\npatched\n")
        self.destination.parent.mkdir(parents=True)
        backup = self.destination.with_name(self.destination.name + ".previous")
        backup.mkdir()
        with self.assertRaisesRegex(RuntimeError, "backup needs inspection"):
            prepare(self.source, self.destination, self.series, self.pin)
        self.assertEqual(list(self.destination.parent.iterdir()), [backup])

    def test_destination_cannot_replace_source(self):
        self.write_patch("base\npatched\n")
        with self.assertRaisesRegex(RuntimeError, "RT64_PATH_OVERLAP"):
            prepare(self.source, self.source, self.series, self.pin)
        self.assertTrue((self.source / ".git").is_dir())


if __name__ == "__main__":
    unittest.main()
