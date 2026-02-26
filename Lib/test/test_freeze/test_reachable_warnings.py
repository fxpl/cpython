"""Tests for freeze warnings when tp_reachable is missing."""
import subprocess
import sys
import textwrap
import unittest


class TestReachableWarnings(unittest.TestCase):
    """Test that freeze logs warnings when tp_reachable is missing."""

    def _run_code(self, code):
        """Run code in a subprocess and return (stdout, stderr)."""
        result = subprocess.run(
            [sys.executable, "-c", textwrap.dedent(code)],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout, result.stderr

    def test_warn_tp_traverse_no_tp_reachable(self):
        """Warn once when a type has tp_traverse but no tp_reachable."""
        stdout, stderr = self._run_code("""\
            import _immutable

            class MyClass:
                def __init__(self):
                    self.x = 1

            _immutable.register_freezable(MyClass)
            _immutable._clear_tp_reachable(MyClass)

            obj = MyClass()
            _immutable.freeze(obj)
        """)
        self.assertIn(
            "freeze: type 'MyClass' has tp_traverse but no tp_reachable",
            stderr,
        )

    def test_warn_only_once_per_type(self):
        """A type should only produce the warning on the first freeze."""
        stdout, stderr = self._run_code("""\
            import _immutable

            class MyClass:
                def __init__(self):
                    self.x = 1

            _immutable.register_freezable(MyClass)
            _immutable._clear_tp_reachable(MyClass)

            _immutable.freeze(MyClass())
            _immutable.freeze(MyClass())
            _immutable.freeze(MyClass())
        """)
        count = stderr.count(
            "freeze: type 'MyClass' has tp_traverse but no tp_reachable"
        )
        self.assertEqual(count, 1, f"Expected 1 warning, got {count}:\n{stderr}")

    def test_warn_different_types_separately(self):
        """Different types should each produce their own warning."""
        stdout, stderr = self._run_code("""\
            import _immutable

            class ClassA:
                pass

            class ClassB:
                pass

            _immutable.register_freezable(ClassA)
            _immutable.register_freezable(ClassB)
            _immutable._clear_tp_reachable(ClassA)
            _immutable._clear_tp_reachable(ClassB)

            _immutable.freeze(ClassA())
            _immutable.freeze(ClassB())
        """)
        self.assertIn("type 'ClassA'", stderr)
        self.assertIn("type 'ClassB'", stderr)

    def test_no_warning_with_tp_reachable(self):
        """No warning should appear when tp_reachable is present."""
        stdout, stderr = self._run_code("""\
            import _immutable

            class MyClass:
                def __init__(self):
                    self.x = 1

            _immutable.register_freezable(MyClass)
            # Do NOT clear tp_reachable
            _immutable.freeze(MyClass())
        """)
        self.assertNotIn("tp_reachable", stderr)
        self.assertNotIn("tp_traverse", stderr)


if __name__ == "__main__":
    unittest.main()
