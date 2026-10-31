"""Debug builds report where a rejected object was frozen."""
import unittest
from test import support
from immutable import deep_freeze, shallow_freeze


class C:
    pass


def freeze_here(obj):
    deep_freeze(obj)


def shallow_freeze_here(obj):
    shallow_freeze(obj)


@unittest.skipUnless(support.Py_DEBUG, "freeze locations are debug-only")
class TestFreezeLocation(unittest.TestCase):
    def assert_frozen_at(self, obj, func_name):
        with self.assertRaises(TypeError) as cm:
            obj.x = 1
        msg = str(cm.exception)
        self.assertIn("frozen at:", msg)
        self.assertIn(func_name, msg)

    def test_deep_freeze_root(self):
        c = C()
        freeze_here(c)
        self.assert_frozen_at(c, "freeze_here")

    def test_deep_freeze_reachable(self):
        c = C()
        c.child = C()
        freeze_here(c)
        self.assert_frozen_at(c.child, "freeze_here")

    def test_shallow_freeze(self):
        c = C()
        shallow_freeze_here(c)
        self.assert_frozen_at(c, "shallow_freeze_here")


@unittest.skipIf(support.Py_DEBUG, "release build message")
class TestNoFreezeLocation(unittest.TestCase):
    def test_plain_message(self):
        c = C()
        freeze_here(c)
        with self.assertRaises(TypeError) as cm:
            c.x = 1
        self.assertNotIn("frozen at", str(cm.exception))


if __name__ == "__main__":
    unittest.main()
