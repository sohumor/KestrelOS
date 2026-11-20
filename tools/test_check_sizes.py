#!/usr/bin/env python3
"""Tests for the built-artifact size gate."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


CHECKER = Path(__file__).with_name("check_sizes.py")


class SizeCheckTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.build = Path(self.temp.name)
        self.kernel = self.build / "kernel.bin"
        self.kernel.touch()
        self.file = self.build / "rootfs" / "bin" / "app"
        self.file.parent.mkdir(parents=True)
        self.file.touch()

    def resize(self, path, size):
        with path.open("r+b") as stream:
            stream.truncate(size)

    def run_check(self):
        return subprocess.run(
            [sys.executable, str(CHECKER), "--build-dir", str(self.build)],
            capture_output=True, text=True)

    def test_below_limits(self):
        self.resize(self.kernel, 921599)
        self.resize(self.file, 3879731)
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("3879731 bytes < 3.7 MiB", result.stdout)

    def test_kernel_at_limit(self):
        self.resize(self.kernel, 921600)
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL:", result.stdout)
        self.assertIn("kernel.bin", result.stdout)

    def test_rootfs_above_limit(self):
        self.resize(self.file, 3879732)
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL:", result.stdout)
        self.assertIn("bin/app", result.stdout)

    def test_empty_rootfs(self):
        self.file.unlink()
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("contains no files", result.stderr)

    def test_missing_kernel(self):
        self.kernel.unlink()
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("run the build first", result.stderr)

    def test_missing_rootfs(self):
        self.file.unlink()
        self.file.parent.rmdir()
        self.file.parent.parent.rmdir()
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("run the build first", result.stderr)


if __name__ == "__main__":
    unittest.main()
