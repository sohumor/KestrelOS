"""Host-only tests for profiler parsing, ranges, and reports."""

import unittest
from unittest.mock import patch
from types import SimpleNamespace

import profile as profiler


class ProfileTests(unittest.TestCase):
    def test_registers_all_cpus(self):
        output = ("CPU#0\nRIP=ffffffff80101000 HLT=0\nCS =0008\n"
                  "CPU#1\nRIP=0000000000400010 HLT=1\nCS =001b\n")
        self.assertEqual(profiler.registers(output),
                         [(0xffffffff80101000, 0, False), (0x400010, 3, True)])

    def test_registers_invalid(self):
        for output in ("", "unknown command", "RIP=123 CS=8"):
            with self.subTest(output=output), self.assertRaises(ValueError):
                profiler.registers(output)

    def test_boot_registers(self):
        self.assertEqual(profiler.registers("EIP=0000fff0 HLT=0\nCS =f000"),
                         [(0xfff0, 0, False)])

    def test_unsized_assembly_symbols(self):
        with patch.object(profiler.subprocess, "run", return_value=SimpleNamespace(
                stdout="1000 T syscall\n1010 0008 r constants\n")):
            symbols = profiler.Symbols("assembly")
        self.assertEqual(symbols.lookup(0x100f), "syscall")
        self.assertIsNone(symbols.lookup(0x1010))

    def test_symbol_ranges(self):
        with patch.object(profiler.subprocess, "run", return_value=SimpleNamespace(
                stdout="1000 0010 T first\n1020 0004 t second\n1030 0008 D data\n")):
            symbols = profiler.Symbols("example")
        self.assertEqual(symbols.lookup(0x100f), "first")
        self.assertEqual(symbols.lookup(0x1020), "second")
        for address in (0, 0x1010, 0x1024, 0x1030):
            self.assertIsNone(symbols.lookup(address))

    def test_empty_symbols(self):
        with patch.object(profiler.subprocess, "run", return_value=SimpleNamespace(stdout="")):
            self.assertIsNone(profiler.Symbols("empty").lookup(123))

    def test_symbol_error_propagates(self):
        with patch.object(profiler.subprocess, "run", side_effect=FileNotFoundError):
            with self.assertRaises(FileNotFoundError):
                profiler.Symbols("missing")

    def test_empty_report(self):
        output = profiler.render({"command": "true", "window": "cmd",
                                  "requested_hz": 200, "phases": [],
                                  "total_samples": 0, "rounds": 0,
                                  "counts": {}, "functions": {"user": []}})
        self.assertIn("Samples: 0 CPU states in 0 QMP rounds", output)
        self.assertIn("(no samples)", output)


if __name__ == "__main__":
    unittest.main()
