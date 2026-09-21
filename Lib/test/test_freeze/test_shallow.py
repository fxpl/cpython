"""Tests for shallow_freeze() and is_shallow_frozen()."""

import unittest
from immutable import (
    shallow_freeze, deep_freeze, is_shallow_frozen, is_deep_frozen,
    set_freezable, FREEZABLE_EXPLICIT, FREEZABLE_NO, FREEZABLE_YES,
)


def make_freezable_class():
    """Create a fresh class marked as freezable."""
    class C:
        def __init__(self, inner=None):
            self.inner = inner
    set_freezable(C, FREEZABLE_YES)
    return C


class TestShallowFreezeBasic(unittest.TestCase):

    def test_returns_first_argument(self):
        C = make_freezable_class()
        obj = C()
        self.assertIs(shallow_freeze(obj, [], 12), obj)

    def test_object_becomes_shallow_frozen(self):
        C = make_freezable_class()
        obj = C()
        shallow_freeze(obj)
        self.assertTrue(is_shallow_frozen(obj))

    def test_object_does_not_become_deep_frozen(self):
        C = make_freezable_class()
        obj = C(C())
        shallow_freeze(obj)
        self.assertTrue(is_shallow_frozen(obj))
        self.assertFalse(is_deep_frozen(obj))
        self.assertFalse(is_shallow_frozen(obj.inner))

    def test_own_state_is_locked(self):
        C = make_freezable_class()
        obj = C()
        shallow_freeze(obj)
        with self.assertRaises(TypeError):
            obj.inner = 1

    def test_referent_stays_mutable(self):
        C = make_freezable_class()
        inner = C()
        shallow_freeze(C(inner))
        self.assertFalse(is_shallow_frozen(inner))
        inner.inner = 1  # must not raise

    def test_type_stays_mutable(self):
        """Unlike deep_freeze, shallow_freeze does not reach the type."""
        C = make_freezable_class()
        shallow_freeze(C())
        self.assertFalse(is_shallow_frozen(C))

    def test_many_args(self):
        C = make_freezable_class()
        objs = [C() for _ in range(10)]
        self.assertIs(shallow_freeze(*objs), objs[0])
        for obj in objs:
            self.assertTrue(is_shallow_frozen(obj))

    def test_zero_args_raises(self):
        with self.assertRaises(TypeError):
            shallow_freeze()

    def test_idempotent(self):
        C = make_freezable_class()
        obj = C()
        shallow_freeze(obj)
        shallow_freeze(obj)
        self.assertTrue(is_shallow_frozen(obj))

    def test_already_deep_frozen(self):
        C = make_freezable_class()
        obj = C()
        deep_freeze(obj)
        shallow_freeze(obj)
        self.assertTrue(is_deep_frozen(obj))


class TestShallowFreezeFreezability(unittest.TestCase):

    def test_not_freezable_raises(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            shallow_freeze(obj)
        self.assertFalse(is_shallow_frozen(obj))

    def test_explicit_root_is_frozen(self):
        """Every shallow_freeze() argument is a root, so EXPLICIT succeeds."""
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_EXPLICIT)
        shallow_freeze(obj)
        self.assertTrue(is_shallow_frozen(obj))

    def test_explicit_referent_is_untouched(self):
        """An EXPLICIT referent is neither frozen nor an error: it isn't visited."""
        C = make_freezable_class()
        inner = C()
        set_freezable(inner, FREEZABLE_EXPLICIT)
        shallow_freeze(C(inner))
        self.assertFalse(is_shallow_frozen(inner))

    def test_one_bad_root_leaves_later_roots_unfrozen(self):
        C = make_freezable_class()
        good, bad, later = C(), C(), C()
        set_freezable(bad, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            shallow_freeze(good, bad, later)
        self.assertFalse(is_shallow_frozen(bad))
        self.assertFalse(is_shallow_frozen(later))


class TestShallowFreezePreFreezeHook(unittest.TestCase):

    def test_hook_runs(self):
        calls = []

        class C:
            def __pre_freeze__(self):
                calls.append(self)
        set_freezable(C, FREEZABLE_YES)

        obj = C()
        shallow_freeze(obj)
        self.assertEqual(calls, [obj])
        self.assertTrue(is_shallow_frozen(obj))

    def test_hook_failure_leaves_object_unfrozen(self):
        class C:
            def __pre_freeze__(self):
                raise ValueError("nope")
        set_freezable(C, FREEZABLE_YES)

        obj = C()
        with self.assertRaises(ValueError):
            shallow_freeze(obj)
        self.assertFalse(is_shallow_frozen(obj))


class TestIsShallowFrozen(unittest.TestCase):

    def test_mutable_object(self):
        C = make_freezable_class()
        self.assertFalse(is_shallow_frozen(C()))

    def test_immutable_by_construction(self):
        self.assertTrue(is_shallow_frozen(42))
        self.assertTrue(is_shallow_frozen("spam"))
        self.assertTrue(is_shallow_frozen((1, 2)))
        self.assertTrue(is_shallow_frozen(frozenset({1})))

    def test_tuple_of_mutable_is_shallow_but_not_deep(self):
        """A tuple's own state is immutable even when its members are not."""
        t = ([],)
        self.assertTrue(is_shallow_frozen(t))
        self.assertFalse(is_deep_frozen(t))

    def test_deep_frozen_implies_shallow_frozen(self):
        C = make_freezable_class()
        obj = C(C())
        deep_freeze(obj)
        self.assertTrue(is_shallow_frozen(obj))
        self.assertTrue(is_shallow_frozen(obj.inner))


if __name__ == "__main__":
    unittest.main()
