"""Unit tests for warning counting and ceiling behavior."""

import contextlib
import io
import unittest

from check_warnings import check_count, warning_lines


class WarningTests(unittest.TestCase):
    def test_unique_deterministic_lines(self):
        first = "b.c:2: warning: b\na.c:1: warning: a\nb.c:2: warning: b\nnote: x"
        self.assertEqual(warning_lines(first), warning_lines("\n".join(reversed(first.splitlines()))))
        self.assertEqual(len(warning_lines(first)), 2)

    def test_empty(self):
        self.assertEqual(warning_lines(""), [])
        self.assertEqual(warning_lines("note: no warnings\n"), [])

    def check_output(self, count, baseline):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            status = check_count(count, baseline)
        return status, output.getvalue()

    def test_equal(self):
        status, output = self.check_output(16, 16)
        self.assertEqual(status, 0)
        self.assertIn("PASS", output)

    def test_increase(self):
        status, output = self.check_output(17, 16)
        self.assertEqual(status, 1)
        self.assertIn("FAIL", output)

    def test_decrease(self):
        status, output = self.check_output(0, 16)
        self.assertEqual(status, 0)
        self.assertIn("Lower tools/warning-baseline.txt to 0", output)


if __name__ == "__main__":
    unittest.main()
