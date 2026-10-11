"""Bulk increments (e.g. tuple/list repeat) on deeply immutable objects."""
import unittest
from immutable import deep_freeze


class N:
    __slots__ = ("name",)

    def __init__(self, name):
        self.name = name


class TestBulkIncref(unittest.TestCase):
    def test_tuple_repeat(self):
        x = N("x")
        deep_freeze(x)
        t = (x,) * 1000
        del t  # must not drive x's refcount negative
        self.assertEqual(x.name, "x")

    def test_list_repeat(self):
        x = N("x")
        deep_freeze(x)
        lst = [x] * 500
        del lst
        self.assertEqual(x.name, "x")

    def test_str_repeat(self):
        s = "immutable-element"
        deep_freeze(s)
        t = (s,) * 2000
        del t
        self.assertEqual(s, "immutable-element")

    def test_interleaved_repeat(self):
        x = N("x")
        deep_freeze(x)
        a = (x,) * 1000
        b = (x,) * 1000
        del a
        # x is still referenced by b; must stay alive and usable
        self.assertEqual(x.name, "x")
        del b
        self.assertEqual(x.name, "x")


if __name__ == "__main__":
    unittest.main()
