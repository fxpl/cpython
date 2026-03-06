"""Tests for freeze rollback: failed freeze leaves objects unfrozen."""

import gc
import unittest
import weakref
from immutable import (
    freeze, isfrozen, register_freezable, set_freezable,
    FREEZABLE_NO,
)


def make_freezable_class():
    """Create a fresh class registered as freezable."""
    class C:
        pass
    register_freezable(C)
    return C


class TestRollbackSingleRoot(unittest.TestCase):
    """When a single-root freeze fails, no objects are left frozen."""

    def test_parent_with_not_freezable_child(self):
        C = make_freezable_class()
        parent = C()
        child = C()
        parent.child = child
        set_freezable(child, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(parent)
        self.assertFalse(isfrozen(parent))
        self.assertFalse(isfrozen(child))

    def test_deep_chain_leaf_not_freezable(self):
        """a -> b -> c -> bad: all should be unfrozen."""
        C = make_freezable_class()
        a, b, c, bad = C(), C(), C(), C()
        a.child = b
        b.child = c
        c.child = bad
        set_freezable(bad, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(a)
        self.assertFalse(isfrozen(a))
        self.assertFalse(isfrozen(b))
        self.assertFalse(isfrozen(c))
        self.assertFalse(isfrozen(bad))

    def test_not_freezable_root(self):
        C = make_freezable_class()
        obj = C()
        set_freezable(obj, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(obj)
        self.assertFalse(isfrozen(obj))


class TestRollbackCycle(unittest.TestCase):
    """Cycles where one member's child is not freezable."""

    def test_cycle_with_not_freezable_child(self):
        """a <-> b, b -> bad: both a and b should be unfrozen."""
        C = make_freezable_class()
        a, b, bad = C(), C(), C()
        a.other = b
        b.other = a
        b.bad = bad
        set_freezable(bad, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(a)
        self.assertFalse(isfrozen(a))
        self.assertFalse(isfrozen(b))
        self.assertFalse(isfrozen(bad))

    def test_three_cycle_with_not_freezable_child(self):
        """a -> b -> c -> a, c -> bad: all unfrozen."""
        C = make_freezable_class()
        a, b, c, bad = C(), C(), C(), C()
        a.child = b
        b.child = c
        c.child = a
        c.bad = bad
        set_freezable(bad, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(a)
        self.assertFalse(isfrozen(a))
        self.assertFalse(isfrozen(b))
        self.assertFalse(isfrozen(c))
        self.assertFalse(isfrozen(bad))


class TestRollbackPreservesExisting(unittest.TestCase):
    """Previously-frozen objects stay frozen when a new freeze fails."""

    def test_previously_frozen_unaffected(self):
        C = make_freezable_class()
        already = C()
        freeze(already)
        self.assertTrue(isfrozen(already))

        parent = C()
        child = C()
        parent.child = child
        set_freezable(child, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(parent)
        self.assertFalse(isfrozen(parent))
        # The previously-frozen object should still be frozen.
        self.assertTrue(isfrozen(already))


class TestRollbackNormalFreezeStillWorks(unittest.TestCase):
    """Normal freeze still works after a rollback."""

    def test_freeze_after_rollback(self):
        C = make_freezable_class()
        bad = C()
        set_freezable(bad, FREEZABLE_NO)
        parent = C()
        parent.child = bad
        with self.assertRaises(TypeError):
            freeze(parent)
        self.assertFalse(isfrozen(parent))

        # Now freeze something else successfully
        good = C()
        freeze(good)
        self.assertTrue(isfrozen(good))

    def test_refreeze_after_rollback(self):
        """Object that was rolled back can be frozen after removing the blocker."""
        C = make_freezable_class()
        parent = C()
        child = C()
        parent.child = child
        set_freezable(child, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(parent)
        self.assertFalse(isfrozen(parent))

        # Remove the blocker and try again
        del parent.child
        freeze(parent)
        self.assertTrue(isfrozen(parent))


class TestRollbackRefcounts(unittest.TestCase):
    """After rollback, refcounts are correct and objects can be collected."""

    def test_single_object_collected_after_rollback(self):
        """A rolled-back single object is collected when unreferenced."""
        C = make_freezable_class()
        parent = C()
        child = C()
        parent.child = child
        wr = weakref.ref(parent)
        set_freezable(child, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(parent)
        del parent, child
        gc.collect()
        self.assertIsNone(wr())

    def test_chain_collected_after_rollback(self):
        """A rolled-back chain is collected when unreferenced."""
        C = make_freezable_class()
        a, b, c, bad = C(), C(), C(), C()
        a.child = b
        b.child = c
        c.child = bad
        wr_a = weakref.ref(a)
        wr_b = weakref.ref(b)
        wr_c = weakref.ref(c)
        set_freezable(bad, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(a)
        del a, b, c, bad
        gc.collect()
        self.assertIsNone(wr_a())
        self.assertIsNone(wr_b())
        self.assertIsNone(wr_c())

    def test_cycle_collected_after_rollback(self):
        """A rolled-back cycle is collected by the GC."""
        C = make_freezable_class()
        a, b, bad = C(), C(), C()
        a.other = b
        b.other = a
        b.bad = bad
        wr_a = weakref.ref(a)
        wr_b = weakref.ref(b)
        set_freezable(bad, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(a)
        del a, b, bad
        gc.collect()
        self.assertIsNone(wr_a())
        self.assertIsNone(wr_b())

    def test_three_cycle_collected_after_rollback(self):
        """A rolled-back 3-member cycle is collected by the GC."""
        C = make_freezable_class()
        a, b, c, bad = C(), C(), C(), C()
        a.child = b
        b.child = c
        c.child = a
        c.bad = bad
        wr_a = weakref.ref(a)
        wr_b = weakref.ref(b)
        wr_c = weakref.ref(c)
        set_freezable(bad, FREEZABLE_NO)
        with self.assertRaises(TypeError):
            freeze(a)
        del a, b, c, bad
        gc.collect()
        self.assertIsNone(wr_a())
        self.assertIsNone(wr_b())
        self.assertIsNone(wr_c())

    def test_weakref_in_graph_collected_after_rollback(self):
        """Objects linked via weakref are collected after rollback."""
        C = make_freezable_class()
        target = C()
        holder = C()
        holder.wr = weakref.ref(target)
        bad = C()
        holder.bad = bad
        set_freezable(bad, FREEZABLE_NO)
        wr_holder = weakref.ref(holder)
        wr_target = weakref.ref(target)
        with self.assertRaises(TypeError):
            freeze(holder)
        self.assertFalse(isfrozen(holder))
        self.assertFalse(isfrozen(target))
        del holder, target, bad
        gc.collect()
        self.assertIsNone(wr_holder())
        self.assertIsNone(wr_target())


if __name__ == '__main__':
    unittest.main()
