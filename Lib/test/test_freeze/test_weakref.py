import gc
import os
import sys
import unittest
import weakref

import immutable
from immutable import (
    deep_freeze, shallow_freeze, is_deep_frozen, is_shallow_frozen,
)
from test.support import import_helper


class A:
    pass


class Finalizable:
    def __del__(self):
        # Using a module field to escape immutability.
        sys.deallocated = True


class CallbackDetector:
    def __init__(self):
        self.called = False

    def callback(self, wr):
        self.called = True


def trigger_pending_calls():
    # Pending calls are checked, for example, when calling a function.
    pass


def dummy_callback(wr):
    pass


class TestRefcounts(unittest.TestCase):
    def test_weakref_to_frozen_object(self):
        baseline = A()
        a = A()
        deep_freeze(a)
        wr = weakref.ref(a)
        # The weakref should be frozen to ensure atomic refcounting.
        # FIXME(Immutable): Freezing a weakref currently makes it strong.
        # self.assertTrue(is_deep_frozen(wr))
        self.assertEqual(sys.getrefcount(wr), sys.getrefcount(baseline))

    def test_weakref_to_frozen_object_callback(self):
        baseline = A()
        a = A()
        deep_freeze(a)
        wr = weakref.ref(a, dummy_callback)
        self.assertFalse(is_deep_frozen(wr))

    def test_freeze_object_with_weakref(self):
        baseline = A()
        a = A()
        wr = weakref.ref(a)
        deep_freeze(a)
        # The weakref should be frozen to ensure atomic refcounting.
        # FIXME(Immutable): Freezing a weakref currently makes it strong.
        # self.assertTrue(is_deep_frozen(wr))
        self.assertEqual(sys.getrefcount(wr), sys.getrefcount(baseline))

    def test_freeze_object_with_weakref_callback(self):
        baseline = A()
        a = A()
        wr = weakref.ref(a, dummy_callback)
        deep_freeze(a)
        # The weakref should have had its refcount pre-emptively incremented.
        self.assertFalse(is_deep_frozen(wr))


class TestWeakrefList(unittest.TestCase):
    def test_remove_weakref(self):
        a = A()
        deep_freeze(a)
        wr = weakref.ref(a)
        wr = None
        # The reference should have been removed.
        self.assertTrue(weakref.getweakrefcount(a) == 0)

    @unittest.skip("Regions disable the weak-reference reuse optimization to selectively clear them")
    def test_reuse_weakref(self):
        a = A()
        deep_freeze(a)
        wr1 = weakref.ref(a)
        wr2 = weakref.ref(a)
        # The weakrefs should be the same, as they refer to the same object.
        self.assertTrue(wr1 is wr2)


class TestGetWeakrefs(unittest.TestCase):
    def test_mutable_weakref(self):
        a = A()
        wr = weakref.ref(a)
        self.assertEqual(weakref.getweakrefs(a), [wr])

    def test_shallow_frozen_weakref(self):
        a = A()
        wr = weakref.ref(a)
        shallow_freeze(wr)
        self.assertTrue(is_shallow_frozen(wr))
        self.assertFalse(is_deep_frozen(wr))
        self.assertEqual(weakref.getweakrefs(a), [wr])

    def test_deep_frozen_weakref(self):
        a = A()
        wr = weakref.ref(a)
        deep_freeze(wr)
        self.assertTrue(is_deep_frozen(wr))
        self.assertEqual(weakref.getweakrefs(a), [wr])

    def test_weakref_to_frozen_object(self):
        a = A()
        deep_freeze(a)
        wr = weakref.ref(a)
        self.assertEqual(weakref.getweakrefs(a), [wr])

class TestFreezeThroughWeakref(unittest.TestCase):
    """Referents are reached by the freeze, but as fresh SCC roots: they must
    end up deeply frozen without the weakref turning into a strong edge that
    keeps them alive."""

    def test_referent_is_deep_frozen(self):
        holder = A()
        target = A()
        holder.wr = weakref.ref(target)
        deep_freeze(holder)
        self.assertTrue(is_deep_frozen(target))

    @unittest.skipUnless(immutable._cross_interpreter_sharing,
                         "only SCC refcounting untracks frozen objects")
    def test_referent_is_untracked(self):
        holder = A()
        target = A()
        holder.wr = weakref.ref(target)
        deep_freeze(holder)
        self.assertFalse(gc.is_tracked(target))

    def test_referent_still_dies(self):
        holder = A()
        target = A()
        holder.wr = weakref.ref(target)
        deep_freeze(holder)
        del target
        gc.collect()
        self.assertIsNone(holder.wr())

    def test_frozen_weakref_root_referent_still_dies(self):
        target = A()
        wr = weakref.ref(target)
        deep_freeze(wr)
        del target
        gc.collect()
        self.assertIsNone(wr())

    def test_cycle_across_weakref(self):
        holder = A()
        target = A()
        holder.wr = weakref.ref(target)
        target.holder = holder
        deep_freeze(holder)
        self.assertTrue(is_deep_frozen(target))

    def test_referent_also_strongly_reachable(self):
        root = A()
        target = A()
        root.strong = target
        root.wr = weakref.ref(target)
        deep_freeze(root)
        self.assertTrue(is_deep_frozen(target))
        self.assertIs(root.wr(), target)

    def test_weakref_chain(self):
        a, b, c = A(), A(), A()
        a.wr = weakref.ref(b)
        b.wr = weakref.ref(c)
        deep_freeze(a)
        self.assertTrue(is_deep_frozen(b))
        self.assertTrue(is_deep_frozen(c))

    def test_dead_referent(self):
        target = A()
        wr = weakref.ref(target)
        del target
        gc.collect()
        deep_freeze(wr)
        self.assertTrue(is_deep_frozen(wr))

    def test_weakref_subclass(self):
        class Ref(weakref.ref):
            pass

        holder = A()
        target = A()
        holder.wr = Ref(target)
        holder.wr.extra = A()
        deep_freeze(holder)
        self.assertTrue(is_deep_frozen(target))
        self.assertTrue(is_deep_frozen(holder.wr.extra))
        self.assertIs(holder.wr(), target)
        del target
        gc.collect()
        self.assertIsNone(holder.wr())

    def test_weakref_subclass_with_slots(self):
        class Ref(weakref.ref):
            __slots__ = ("extra",)

        target = A()
        wr = Ref(target)
        wr.extra = A()
        deep_freeze(wr)
        self.assertTrue(is_deep_frozen(target))
        self.assertTrue(is_deep_frozen(wr.extra))

    @unittest.skipUnless(immutable._cross_interpreter_sharing,
                         "the assert it guards is SCC-only")
    def test_survives_gc_churn(self):
        """A deeply frozen but still tracked object trips an assert in
        gc_collect_increment(), which only the incremental collector reaches."""
        holder = A()
        target = A()
        holder.wr = weakref.ref(target)
        deep_freeze(holder)
        for _ in range(50):
            junk = [A() for _ in range(500)]
            for p, q in zip(junk, junk[1:]):
                p.next = q
            del junk
            gc.collect(1)


class TestCallbacks(unittest.TestCase):
    def setUp(self):
        sys.deallocated = False

    def test_callback_single(self):
        f = Finalizable()
        deep_freeze(f)
        detector = CallbackDetector()
        wr = weakref.ref(f, detector.callback)
        f = None
        trigger_pending_calls()
        self.assertTrue(detector.called)
        self.assertTrue(sys.deallocated)

    @unittest.skipUnless(immutable._cross_interpreter_sharing,
                         "without SCC refcounting the cycle needs a GC pass")
    def test_callback_scc(self):
        f = Finalizable()
        f.b = A()
        f.b.f = f
        deep_freeze(f)
        detector = CallbackDetector()
        wr = weakref.ref(f, detector.callback)
        f = None
        trigger_pending_calls()
        self.assertTrue(detector.called)
        self.assertTrue(sys.deallocated)


@unittest.skipUnless(immutable._cross_interpreter_sharing,
                     "frozen objects are not shared across interpreters")
class TestCrossInterpreterCallbacks(unittest.TestCase):
    """Callbacks run in the interpreter that registered them."""

    def setUp(self):
        self._interpreters = import_helper.import_module('_interpreters')
        self.interp = self._interpreters.create('legacy')
        self.addCleanup(self.destroy)
        self.r, w = os.pipe()
        self.addCleanup(os.close, self.r)
        self.addCleanup(os.close, w)
        os.set_blocking(self.r, False)
        f = A()
        deep_freeze(f)
        self.wr = weakref.ref(f)
        self._interpreters.run_string(self.interp, (
            "import os, sys, weakref\n"
            f"sys.wr = weakref.ref(f, lambda r: os.write({w}, b'called'))\n"
            "del f\n"
        ), shared={"f": f})
        del f
        gc.collect()

    def destroy(self):
        if self.interp is not None:
            self._interpreters.destroy(self.interp)
            self.interp = None

    def read_pipe(self):
        try:
            return os.read(self.r, 100)
        except BlockingIOError:
            return b""

    def test_callback_runs_in_registering_interpreter(self):
        self.assertEqual(self.read_pipe(), b"")
        self._interpreters.run_string(self.interp, "pass")
        self.assertEqual(self.read_pipe(), b"called")
        self.assertIsNone(self.wr())

    def test_callback_drained_when_interpreter_destroyed(self):
        self.assertEqual(self.read_pipe(), b"")
        self.destroy()
        self.assertEqual(self.read_pipe(), b"called")
        self.assertIsNone(self.wr())


class TestRemoveDeadWeakref(unittest.TestCase):
    def test_frozen_dict(self):
        import _weakref
        target = A()
        d = {"k": weakref.ref(target)}
        deep_freeze(d)
        del target
        gc.collect()
        with self.assertRaisesRegex(TypeError, "is immutable"):
            _weakref._remove_dead_weakref(d, "k")
        self.assertIn("k", d)


if __name__ == '__main__':
    unittest.main()
