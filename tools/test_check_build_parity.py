#!/usr/bin/env python3
"""Unit coverage for parity comparison and command capture boundaries."""

import contextlib
import io
import subprocess
import unittest

import check_build_parity as parity


class ParityTests(unittest.TestCase):
    def test_empty_commands(self):
        self.assertEqual(parity.native_commands([], "/output"), set())

    def test_normalization(self):
        commands = [["ld", "-L", "/output/lib", "-o", "/output/app", "a.o", "b.o"],
                    ["ar", "rcs", "/output/lib.a", "b.o", "a.o"],
                    ["nasm", "-f", "bin", "boot/stage1.asm"],
                    ["python3", "tools/mkfs.py"]]
        self.assertEqual(parity.native_commands(commands, "/output"), {
            "ld -Lbuild/lib -o build/app a.o b.o", "ar rcs build/lib.a a.o b.o"})

    def test_link_order_is_significant(self):
        left = parity.native_commands([["ld", "a.o", "b.o"]], "/output")
        right = parity.native_commands([["ld", "b.o", "a.o"]], "/output")
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertFalse(parity.compare("link order", left, right))
        self.assertIn("-ld a.o b.o", output.getvalue())
        self.assertIn("+ld b.o a.o", output.getvalue())

    def test_equal_sets(self):
        self.assertTrue(parity.compare("empty", set(), set()))

    def test_real_parser_choices(self):
        self.assertEqual(parity.python_actions(), set(parity.COMMANDS))

    def test_command_error(self):
        with self.assertRaises(subprocess.CalledProcessError):
            parity.command_output([parity.sys.executable, "-c", "raise SystemExit(3)"])


if __name__ == "__main__":
    unittest.main()
