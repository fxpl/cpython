import sys
import unittest
from immutable import freeze, is_frozen, freezable
from immutable import TracingRegion as Region

class TestTraceRefs(unittest.TestCase):
    def test_trace(self):
        @freezable
        class A:
            pass

        r = Region()
        r.a = A()
        r.b = A()
        r.c = A()

        _, base_refs = r.trace()

        a = r.a
        _, ref_count = r.trace()
        self.assertEqual(ref_count, base_refs + 1)

        b = r.b
        c = r.c
        _, ref_count = r.trace()
        self.assertEqual(ref_count, base_refs + 3)

class TestImplicitFreeze(unittest.TestCase):
    def test_implicit_freeze_func(self):
        @freezable
        def some_func():
            pass
        r = Region()

        r.obj = some_func
        self.assertFalse(is_frozen(r.obj))
        r.trace()
        self.assertTrue(is_frozen(r.obj))

    def test_implicit_freeze_type(self):
        @freezable
        class A:
            pass
        r = Region()

        r.obj = A
        self.assertFalse(is_frozen(r.obj))
        r.trace()
        self.assertTrue(is_frozen(r.obj))

    def test_implicit_freeze_module(self):
        import random;
        r = Region()

        r.obj = random
        self.assertFalse(is_frozen(r.obj))
        r.trace()
        self.assertTrue(is_frozen(r.obj))

        # Unimport module
        sys.modules.pop("random", None)
        sys.mut_modules.pop("random", None)

    def test_implicit_freeze_str(self):
        r = Region()

        r.obj = "Ducks are cool"
        r.trace()
        self.assertTrue(is_frozen(r.obj))

    def test_implicit_freeze_int(self):
        r = Region()

        r.obj = 17
        r.trace()
        self.assertTrue(is_frozen(r.obj))
