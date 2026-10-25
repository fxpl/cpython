import gc
import re
import sys
import threading
import time
import unittest
import weakref
from immutable import deep_freeze, is_deep_frozen, freezable
from immutable import TracingRegion as Region
from immutable import Mutex, InterpreterLocal
import immutable
from test import support
from test.support import import_helper, os_helper, script_helper, threading_helper

def sort_region_error(msg):
    """Normalize a 'region could not be closed' message by masking the object
    addresses and sorting its per-object lines. Useful for deterministic test
    assertions, since the addresses differ per run and the object order comes
    from hashtable iteration and isn't stable."""
    header, *lines = re.sub(r"0x[0-9a-fA-F]+", "0x...", msg).splitlines()
    return [header, *sorted(lines)]

class TestTracing(unittest.TestCase):
    def test_release_error(self):
        x = [1]
        y = [2]

        r = Region()
        r.x = x
        r.y = y

        with self.assertRaises(RuntimeError) as cm:
            Region.close(r)

        self.assertEqual(
            sort_region_error(str(cm.exception)),
            [
                "The region could not be closed due to:",
                "- 1 incoming reference to list '[1]'",
                "- 1 incoming reference to list '[2]'"
            ])

    def test_release_error_capped_output(self):
        # The object order in the error message is based on the address
        # and therefore fairly random. All elements look the same of
        # make testing stable.
        l = [[1], [1], [1], [1], [1], [1], [1], [1]]

        r = Region()
        r.x = []

        for i in range(len(l)):
            r.x.append(l[i])

        with self.assertRaises(RuntimeError) as cm:
            Region.close(r)

        self.assertEqual(
            sort_region_error(str(cm.exception)),
            [
                "The region could not be closed due to:",
                "- 1 incoming reference to list '[1]'",
                "- 1 incoming reference to list '[1]'",
                "- 1 incoming reference to list '[1]'",
                "- 1 incoming reference to list '[1]'",
                "- 1 incoming reference to list '[1]'",
                "- 3 references to other objects",
            ])

        # The region should now close
        l = None
        Region.close(r)

    def test_release_error_in_subregion(self):
        x = [1]

        r = Region()
        child = Region()
        child.x = x
        r.child = child

        with self.assertRaises(RuntimeError) as cm:
            Region.close(r)

        self.assertEqual(
            sort_region_error(str(cm.exception)),
            [
                "The region could not be closed due to:",
                "- 1 incoming reference to list '[1]'",
            ])

    def test_failed_multi_parent_region_close(self):
        r1 = Region()
        r2 = Region()
        r3 = Region()
        r2.sub = r3
        r1.lst = [r3, r2, r3]
        del r2
        del r3

        with self.assertRaises(RuntimeError) as cm:
            Region.close(r1)

        self.assertEqual(
            sort_region_error(str(cm.exception)),
            [
                "The region could not be closed due to:",
                "- 3 incoming references to TracingRegion '<TracingRegion closed>'",
            ])


    def test_failed_cyclic_region_close(self):
        r1 = Region()
        r2 = Region()
        r3 = Region()

        r1.r2 = r2
        r2.r3 = r3
        r3.r1 = r1

        del r2
        del r3

        with self.assertRaises(RuntimeError) as cm:
            Region.close(r1)

        self.assertEqual(
            sort_region_error(str(cm.exception)),
            [
                "the region 0x... can not be closed as it attempts to reference one of its parent regions 0x...",
            ])

    def test_weak_ref_in_region(self):
        @freezable
        class A:
            pass

        m = Mutex(Region())
        m.value.obj = A()
        m.value.wref1 = weakref.ref(m.value.obj)
        wref2 = weakref.ref(m.value.obj)

        # Closing should pause all external weak references
        m.release()
        self.assertIsNone(wref2())

        # Acquireing should reenable all weak references
        m.acquire()
        self.assertEqual(wref2(), m.value.obj)
        self.assertEqual(m.value.wref1(), m.value.obj)

    def test_weak_ref_to_bridge(self):
        """
        The closing code and mutexes currently assume that bridges can't have weak references.
        This tests asserts this. We can add support for weak refs, but that would require some
        engineering and the question is if this is even needed.
        """

        r1 = Region()
        with self.assertRaises(TypeError) as err:
            weakref.ref(r1)

        self.assertEqual(str(err.exception), "cannot create weak reference to 'TracingRegion' object")

    def test_weak_ref_to_outside_object(self):
        @freezable
        class A:
            pass

        outside = A()
        for make_ref in (weakref.ref,
                         lambda o: weakref.ref(o, lambda w: None),
                         weakref.proxy):
            with self.subTest(make_ref=make_ref):
                r = Region()
                r.wref = make_ref(outside)
                # The referent is pulled into the region, but it is also
                # referenced from the outside
                with self.assertRaises(RuntimeError) as cm:
                    Region.close(r)
                self.assertEqual(
                    str(cm.exception),
                    "The region could not be closed due to:\n"
                    f"- 1 incoming reference to A '{outside!r}'\n")
                self.assertFalse(Region.is_closed(r))

    def test_dead_weak_ref_in_region(self):
        @freezable
        class A:
            pass

        r = Region()
        tmp = A()
        r.wref = weakref.ref(tmp)
        del tmp
        Region.close(r)
        self.assertTrue(Region.is_closed(r))
        self.assertIsNone(r.wref())


    def test_release_error_with_failing_repr(self):
        @freezable
        class BadRepr:
            def __repr__(self):
                raise ZeroDivisionError("repr failed")
            __str__ = __repr__

        bad = BadRepr()
        r = Region()
        r.bad = bad

        # The error from formatting the report replaces the close error
        with self.assertRaisesRegex(ZeroDivisionError, "repr failed"):
            Region.close(r)
        self.assertFalse(Region.is_closed(r))

        del bad
        Region.close(r)
        self.assertTrue(Region.is_closed(r))

    def test_bridge_self_reference(self):
        c = Mutex(Region())
        c.value.me = c.value
        c.release()
        c.acquire()
        self.assertTrue(Region.is_closed(c.value))
        self.assertIs(c.value.me, c.value)


class TestRegionCloseAPI(unittest.TestCase):
    """`TracingRegion.close`/`is_closed` are static methods taking a region."""

    def test_is_closed_reflects_state(self):
        r = Region()
        self.assertFalse(Region.is_closed(r))
        Region.close(r)
        self.assertTrue(Region.is_closed(r))

    def test_close_is_idempotent(self):
        r = Region()
        Region.close(r)
        Region.close(r)
        self.assertTrue(Region.is_closed(r))

    def test_reject_non_region(self):
        for bad in (42, [1], {}, None, object()):
            with self.assertRaises(TypeError):
                Region.close(bad)
            with self.assertRaises(TypeError):
                Region.is_closed(bad)


class TestRegionAPI(unittest.TestCase):
    def test_init_takes_no_arguments(self):
        with self.assertRaises(TypeError):
            Region(1)
        with self.assertRaises(TypeError):
            Region(a=1)

    def test_dict(self):
        r = Region()
        self.assertEqual(r.__dict__, {})

        r.__dict__ = {"x": 1}
        self.assertEqual(r.x, 1)

        with self.assertRaisesRegex(TypeError, "cannot delete __dict__"):
            del r.__dict__
        with self.assertRaisesRegex(TypeError, "__dict__ must be set to a dictionary"):
            r.__dict__ = 1
        self.assertEqual(r.x, 1)

    def test_dict_access_opens_region(self):
        r = Region()
        Region.close(r)
        r.__dict__
        self.assertFalse(Region.is_closed(r))

    def test_repr(self):
        r = Region()
        self.assertEqual(repr(r), "<TracingRegion open>")
        Region.close(r)
        self.assertEqual(repr(r), "<TracingRegion closed>")
        self.assertTrue(Region.is_closed(r))

    def test_mutex_in_region(self):
        r = Region()
        m = Mutex()
        m.release()
        r.m = m
        del m

        Region.close(r)
        self.assertTrue(Region.is_closed(r))
        self.assertFalse(is_deep_frozen(r.m))

    def test_open_cyclic_region_is_collected(self):
        @freezable
        class A:
            pass

        r = Region()
        r.me = r
        r.a = A()
        wref = weakref.ref(r.a)
        del r
        gc.collect()
        self.assertIsNone(wref())


class TestMovability(unittest.TestCase):
    """Which objects a close may move into a region."""

    @unittest.skipUnless(support.Py_GIL_DISABLED,
                         "per-thread refcounting only exists on free threading")
    def test_per_thread_refcounted_dict_is_not_movable(self):
        # Module dicts enable per-thread refcounting (see
        # _PyDict_EnablePerThreadRefcounting). The trace can't read a sound
        # refcount off such a dict, so closing a region holding one must fail.
        import csv
        r = Region()
        r.d = csv.__dict__
        with self.assertRaises(RuntimeError) as cm:
            Region.close(r)
        self.assertIn("not movable", str(cm.exception))
        self.assertIs(cm.exception.source, r.__dict__)
        self.assertIs(cm.exception.target, csv.__dict__)

    def assert_not_movable(self, value, type_name):
        r = Region()
        r.x = value
        with self.assertRaises(RuntimeError) as cm:
            Region.close(r)
        self.assertEqual(str(cm.exception),
                         f"Instances of type '{type_name}' are not movable")
        self.assertIs(cm.exception.source, r.__dict__)
        self.assertIs(cm.exception.target, value)
        self.assertFalse(Region.is_closed(r))

    def test_frame_is_not_movable(self):
        self.assert_not_movable(sys._getframe(), "frame")

    def test_generator_is_not_movable(self):
        self.assert_not_movable((i for i in ()), "generator")

    def test_coroutine_is_not_movable(self):
        async def coro():
            pass
        c = coro()
        try:
            self.assert_not_movable(c, "coroutine")
        finally:
            c.close()

    def test_exception_is_not_movable(self):
        self.assert_not_movable(ValueError(), "ValueError")

    def test_plain_dict_is_movable(self):
        r = Region()
        r.d = {"a": 1}
        Region.close(r)
        self.assertTrue(Region.is_closed(r))


class TestRegionOpening(unittest.TestCase):
    def test_open_after_acquire(self):
        c = Mutex(Region())
        c.value.x = []
        self.assertFalse(Region.is_closed(c.value))

        c.release()
        c.acquire()

        self.assertTrue(Region.is_closed(c.value))
        c.value.x = None
        self.assertFalse(Region.is_closed(c.value))

    def test_reclose_after_open(self):
        @freezable
        class A:
            pass

        c = Mutex(Region())
        c.value.a = A()
        c.release()
        c.acquire()
        rc = sys.getrefcount(c.value.a)

        # Each close must drop the references it took while tracing
        for _ in range(3):
            c.value.b = 1
            c.release()
            c.acquire()
            self.assertEqual(sys.getrefcount(c.value.a), rc)

    def test_release_closed_region(self):
        c = Mutex(Region())
        c.value.x = []
        self.assertFalse(Region.is_closed(c.value))

        c.release()
        c.acquire()

        self.assertTrue(Region.is_closed(c.value))

        c.release()

    def test_bridge_refs_keep_region_closed(self):
        c = Mutex(Region())
        c.release()
        c.acquire()
        self.assertTrue(Region.is_closed(c.value))

        # Adding new references to the bridge object should keep it closed.
        # only attribute accesses should open it.
        r1 = c.value
        r2 = c.value
        self.assertTrue(Region.is_closed(c.value))

        # However, these references should prevent the mutex from being released
        with self.assertRaises(RuntimeError) as cm:
            c.release()

        self.assertEqual(
            str(cm.exception),
            "the region couldn't be detached, due to incoming references to the bridge")

        # The release should succeed once all refs have been killed
        del r1
        del r2
        c.release()

    def test_sub_region_closing(self):
        @freezable
        class A:
            pass
        r = Region()
        r.a = A()
        r.a.child = Region()
        r.a.child.b = A()

        Region.close(r)

        r2 = r.a.child
        self.assertTrue(Region.is_closed(r2))

    def test_sub_region_multiple_refs(self):
        @freezable
        class A:
            pass
        r = Region()
        r.a = A()
        sub = Region()
        r.a.child_a = sub
        r.a.child_b = sub
        # A reference to the bridge of a sub-region counts as an incoming
        # reference into the parent region, see
        # test_ref_to_sub_region_bridge_keeps_parent_open.
        del sub

        Region.close(r)

        r2 = r.a.child_a
        self.assertTrue(Region.is_closed(r2))

    def test_ref_to_sub_region_bridge_keeps_parent_open(self):
        c1 = Mutex(Region())
        c2 = Mutex(Region())
        c1.value.child = c2.value

        self.assertFalse(Region.is_closed(c2.value))

        with self.assertRaises(RuntimeError) as cm:
            c1.release()

        # Attempting to close the region c1 should have closed c2 and then
        # failed due to the incoming reference to the bridge stored in c2
        self.assertTrue(Region.is_closed(c2.value))

        self.assertEqual(
            sort_region_error(str(cm.exception)),
            [
                "The region could not be closed due to:",
                "- 1 incoming reference to TracingRegion '<TracingRegion closed>'",
            ])



class TestImplicitFreeze(unittest.TestCase):
    def test_implicit_freeze_func(self):
        @freezable
        def some_func():
            pass
        r = Region()

        r.obj = some_func
        self.assertFalse(is_deep_frozen(r.obj))
        Region.close(r)
        self.assertTrue(is_deep_frozen(r.obj))

    def test_implicit_freeze_type(self):
        @freezable
        class A:
            pass
        r = Region()

        r.obj = A
        self.assertFalse(is_deep_frozen(r.obj))
        Region.close(r)
        self.assertTrue(is_deep_frozen(r.obj))

    def test_implicit_freeze_module(self):
        import csv;
        r = Region()

        r.obj = csv
        self.assertFalse(is_deep_frozen(r.obj))
        Region.close(r)
        self.assertTrue(is_deep_frozen(r.obj))

        # Unimport module
        sys.modules.pop("random", None)
        sys.mut_modules.pop("random", None)

    def test_implicit_freeze_str(self):
        r = Region()

        r.obj = "Ducks are cool"
        Region.close(r)
        self.assertTrue(is_deep_frozen(r.obj))

    def test_implicit_freeze_int(self):
        r = Region()

        r.obj = 17
        Region.close(r)
        self.assertTrue(is_deep_frozen(r.obj))

    def test_implicit_freeze_builtins(self):
        r = Region()

        r.builtin_function = len
        r.wrapper_descriptor = int.__add__
        r.method_wrapper = (1).__add__
        Region.close(r)
        self.assertTrue(is_deep_frozen(r.builtin_function))
        self.assertTrue(is_deep_frozen(r.wrapper_descriptor))
        self.assertTrue(is_deep_frozen(r.method_wrapper))


class TestClosedRegionTeardown(unittest.TestCase):
    """A closed region disposes of its own contents.

    Closing proves nothing outside the region references its members, so the
    death of the bridge object makes all of them garbage. The region finalizes
    and clears them itself rather than handing them to the GC.
    """

    @unittest.skipUnless(immutable._cross_interpreter_sharing,
        "FIXME(regions): On free-threading we currently just open the region")
    def test_cycles_reclaimed_without_the_collector(self):
        """Check that cycles in closed regions are reclaimed without the collector"""

        @freezable
        class Node:
            pass

        def _live_nodes():
            """The number of nodes the collector can see."""
            return sum(1 for o in gc.get_objects() if type(o) is Node)

        gc.disable()
        try:
            before = _live_nodes()

            # Create a cycle
            a = Node()
            b = Node()
            a.b = b
            b.a = a

            # Move the cycle into a mutex
            c = Mutex(Region())
            c.value.cycle = a
            del a
            del b
            mid = _live_nodes()

            # Close the region
            c.release()
            del c

            leaked = _live_nodes() - before
        finally:
            gc.enable()

        self.assertEqual(mid, 2, "the cycle wasn't detected while the region is open")
        self.assertEqual(leaked, 0, "closed region contents were not reclaimed")

    def test_finalizers_run(self):
        local = InterpreterLocal(0)

        @freezable
        class Recorder:
            def __del__(self, local=local):
                local.set(local.get() + 1)

        c = Mutex(Region())
        for i in range(5):
            setattr(c.value, "r%d" % i, Recorder())
        c.release()

        self.assertEqual(local.get(), 0)
        del c
        self.assertEqual(local.get(), 5)


    def test_finalizer_can_modify_the_bridge(self):
        local = InterpreterLocal(False)

        @freezable
        class Reenter:
            def __del__(self, local=local):
                # This will open the region and also prove that the finalizer ran
                expected_id = local.get()
                this_id = hex(id(self))
                local.set(expected_id == this_id)

        # Create a cycle, and allow Reenter to modify the bridge
        c = Mutex(Region())
        c.value.reenter = Reenter()
        local.set(hex(id(c.value.reenter)))
        c.release()
        del c

        self.assertTrue(local.get(), "the finalizer did not run or got the wrong object")

    @unittest.skipUnless(immutable._cross_interpreter_sharing,
        "FIXME(regions): On free-threading we currently just open the region")
    def test_finalizer_revivial(self):
        local_bridge = InterpreterLocal(None)
        local_medic = InterpreterLocal(None)

        @freezable
        class Medic:
            def __del__(self, loca_bridge=local_bridge, local_medic=local_medic):
                local_bridge.set(self.bridge)
                local_medic.set(self)

        c1 = Mutex(Region())
        c1.value.reviver = Medic()
        c1.value.reviver.bridge = c1.value
        c1.release()
        del c1

        # Retrieve the revived bridge
        self.assertIsInstance(local_bridge.get(), Region);
        c2 = Mutex(local_bridge.get())
        local_bridge.set(None)

        with self.assertRaises(RuntimeError) as e:
            c2.release()
        self.assertTrue(str(e.exception).endswith("has been finalized and cannot be closed again"))

        # Check that the revived medic object is valid
        self.assertIn("Medic object at 0x", str(local_medic.get()))


class TestRegionRef(unittest.TestCase):
    """A region reference does not keep a region open. It checks on every
    dereference whether this interpreter may reach the target, and opens the
    region tree on the way."""

    def _obj(self, tag=0):
        @freezable
        class A:
            pass
        obj = A()
        obj.tag = tag
        return obj

    def test_deref_opens_the_region(self):
        r = Region()
        r.obj = self._obj(4)
        rr = weakref.ref(r.obj)
        c = Mutex(r)
        del r
        c.release()
        c.acquire()

        self.assertTrue(Region.is_closed(c.value))
        obj = rr()
        self.assertFalse(Region.is_closed(c.value))
        self.assertIs(obj, c.value.obj)

    def test_deref_opens_the_whole_chain(self):
        """A reference into a nested region has to open every region above it,
        not just the one holding the target."""
        child = Region()
        child.obj = self._obj(5)
        rr = weakref.ref(child.obj)
        r = Region()
        r.child = child
        del child

        c = Mutex(r)
        del r
        c.release()
        c.acquire()

        self.assertTrue(Region.is_closed(c.value))
        self.assertEqual(rr().tag, 5)
        self.assertFalse(Region.is_closed(c.value))
        self.assertEqual(c.value.child.obj.tag, 5)

    def test_release_after_acquire_without_opening(self):
        """A release does not re-trace an already closed region, so nothing
        re-stamps its node. The mutex node is what keeps the next owner able to
        dereference."""
        r = Region()
        r.obj = self._obj(6)
        rr = weakref.ref(r.obj)
        c = Mutex(r)
        del r

        c.release()
        c.acquire()
        c.release()

        self.assertIsNone(rr())

        c.acquire()
        self.assertEqual(rr().tag, 6)

    def test_region_outliving_its_mutex(self):
        r = Region()
        r.obj = self._obj(7)
        rr = weakref.ref(r.obj)
        c = Mutex(r)
        del r
        c.release()
        c.acquire()

        escaped = c.value
        del c
        gc.collect()

        self.assertEqual(rr().tag, 7)
        self.assertIsNotNone(escaped)

    def test_target_moving_between_regions(self):
        """A close re-homes every reference to the objects it traced, so an
        object that changed regions resolves through the one it lives in."""
        c1 = Mutex(Region())
        c1.value.obj = self._obj(8)
        rr = weakref.ref(c1.value.obj)
        c1.release()
        c1.acquire()

        c2 = Mutex(Region())
        c2.value.obj = c1.value.obj
        c1.value.obj = None

        # Closing the old region should not restrict the region reference
        c1.release()
        self.assertEqual(rr().tag, 8)

        # Closing the owning reference should restrict the region reference
        c2.release()
        self.assertIsNone(rr())

        # Opening the owning mutex allows the region reference again
        c2.acquire()
        self.assertEqual(rr().tag, 8)

    def test_dead_target(self):
        r = Region()
        r.obj = self._obj(9)
        rr = weakref.ref(r.obj)
        r.obj = None
        gc.collect()

        self.assertIsNone(rr())
        self.assertIn("dead", repr(rr))

    def test_frozen_target_drops_the_check(self):
        """A frozen object is reachable from everywhere, so its references stop
        carrying an ownership check."""
        obj = self._obj(10)
        rr = weakref.ref(obj)
        deep_freeze(obj)
        self.assertEqual(rr().tag, 10)

    def test_repr_does_not_open_the_region(self):
        r = Region()
        r.obj = self._obj(11)
        rr = weakref.ref(r.obj)
        c = Mutex(r)
        del r
        c.release()

        repr(rr)

        c.acquire()
        self.assertTrue(Region.is_closed(c.value))
        self.assertIn("to '", repr(rr))
        self.assertFalse(Region.is_closed(c.value))

    def test_failed_close_leaves_the_reference_local(self):
        """A close that fails leaves its node unresolved. It has to end up
        local to this interpreter, or the reference would be stuck."""
        child = Region()
        child.obj = self._obj(15)
        rr = weakref.ref(child.obj)
        r = Region()
        r.child = child
        del child

        leak = self._obj(16)
        r.leak = leak
        c = Mutex(r)
        del r

        with self.assertRaises(RuntimeError):
            c.release()
        self.assertEqual(rr().tag, 15)

    def test_closed_but_not_released_leaves_the_reference_local(self):
        """The tree can close and the mutex still refuse to release. Nothing
        roots the region in that case, so it stays ours."""
        r = Region()
        r.obj = self._obj(17)
        rr = weakref.ref(r.obj)
        c = Mutex(r)
        del r

        held = c.value
        with self.assertRaises(RuntimeError) as cm:
            c.release()
        self.assertIn("incoming references", str(cm.exception))
        self.assertEqual(rr().tag, 17)

        del held
        gc.collect()
        c.release()
        self.assertIsNone(rr())

    def test_deeply_nested_region(self):
        inner = Region()
        inner.obj = self._obj(18)
        rr = weakref.ref(inner.obj)
        node = inner
        for _ in range(30):
            outer = Region()
            outer.child = node
            node = outer
        c = Mutex(node)
        del node, inner, outer

        c.release()
        c.acquire()
        self.assertEqual(rr().tag, 18)

    def test_repeated_close_open_cycles(self):
        """Each close allocates a fresh node and re-homes the reference onto
        it. The old ones have to go away with it."""
        r = Region()
        r.obj = self._obj(19)
        rr = weakref.ref(r.obj)
        c = Mutex(r)
        del r

        for _ in range(100):
            c.release()
            c.acquire()
            self.assertEqual(rr().tag, 19)

    def test_reference_itself_is_cyclic_garbage(self):
        """A RegionRef that is itself unreachable has to be cleared before the
        garbage is deleted, or a __del__ could still dereference it."""
        obj = self._obj(21)
        cell = [weakref.ref(obj)]
        cell.append(cell)
        del cell
        gc.collect()
        self.assertIsNotNone(obj)

    def test_already_frozen_target_is_unrestricted(self):
        """Freezing before or after the reference is created has to give the
        same answer, otherwise the semantics depend on ordering."""
        early = self._obj(23)
        deep_freeze(early)
        rr_early = weakref.ref(early)

        late = self._obj(24)
        rr_late = weakref.ref(late)
        deep_freeze(late)

        self.assertEqual(rr_early().tag, 23)
        self.assertEqual(rr_late().tag, 24)

    def test_hash_is_checked_even_when_cached(self):
        """A cached hash would otherwise stand as an answer about an object
        this interpreter may no longer touch."""
        r = Region()
        r.obj = self._obj(26)
        rr = weakref.ref(r.obj)
        hash(rr)

        c = Mutex(r)
        del r
        c.release()
        c.acquire()
        self.assertIsInstance(hash(rr), int)


class TestRegionRefSubinterpreters(unittest.TestCase):
    """The point of the ownership check: another interpreter may only
    dereference what it actually owns."""

    def setUp(self):
        self._interpreters = import_helper.import_module('_interpreters')

    def _run_in_subinterp(self, code, shared=None):
        interp = self._interpreters.create()
        try:
            # run_string() reports errors by returning them, not by raising
            err = self._interpreters.run_string(interp, code, shared=shared or {})
            if err is not None:
                self.fail(f"subinterpreter raised:\n{err.errdisplay}")
        finally:
            self._interpreters.destroy(interp)

    @unittest.skipUnless(immutable._cross_interpreter_sharing,
        "objects are not shared across interpreters")
    def test_frozen_target_reachable_from_everywhere(self):
        """A frozen target is shareable, so its references carry no ownership
        check at all -- whichever order the freeze and the reference happened
        in."""
        @freezable
        class A:
            pass

        early = A(); early.tag = "early"
        deep_freeze(early)
        late = A(); late.tag = "late"

        r = Region()
        r.early_ref = weakref.ref(early)
        r.late_ref = weakref.ref(late)
        deep_freeze(late)
        c = Mutex(r)
        del r
        c.release()

        self._run_in_subinterp("""
c.acquire()
assert c.value.early_ref().tag == "early", "frozen before the ref was created"
assert c.value.late_ref().tag == "late", "frozen after the ref was created"
c.release()
""", shared={"c": c})

    @unittest.skipUnless(immutable._cross_interpreter_sharing,
        "objects are not shared across interpreters")
    def test_foreign_deallocation_does_not_transfer_ownership(self):
        """A mutex is immutable and may live inside a region, so the last
        reference to it can be dropped by an interpreter that never owned it.
        Its region must go to the mutex's owner, not to whoever runs the
        deallocator."""
        @freezable
        class A:
            pass

        inner_region = Region()
        inner_region.obj = A()
        inner_region.obj.tag = "owned by the creator"
        mine = weakref.ref(inner_region.obj)
        travelling = weakref.ref(inner_region.obj)

        inner = Mutex(inner_region)
        del inner_region
        inner.release()
        inner.acquire()
        # Keep the region alive past the mutex, so the mutex can die alone.
        escaped = inner.value

        outer_region = Region()
        outer_region.inner = inner
        outer_region.ref = travelling
        del travelling
        outer = Mutex(outer_region)
        del outer_region
        outer.release()
        del inner
        gc.collect()

        self._run_in_subinterp("""
import gc
c.acquire()
c.value.inner = None      # drops the last reference to a mutex we never owned
gc.collect()
try:
    c.value.ref()
    raise AssertionError("reached a region owned by another interpreter")
except RuntimeError:
    pass
c.release()
""", shared={"c": outer})

        # The creator still owns it, and is not locked out of its own region.
        self.assertEqual(escaped.obj.tag, "owned by the creator")
        self.assertEqual(mine().tag, "owned by the creator")

    @unittest.skipUnless(immutable._cross_interpreter_sharing,
        "objects are not shared across interpreters")
    def test_foreign_deallocation_defers_the_teardown_to_the_owner(self):
        """The last reference to a mutex owned by this interpreter may be
        dropped by another one. The region inside is reference counted
        non-atomically and tracked in this interpreter's GC list, so the
        teardown has to be handed back here instead of running there."""
        log = os_helper.TESTFN
        self.addCleanup(os_helper.unlink, log)

        @freezable
        class Marker:
            def __del__(self):
                with open(self.log, "a") as f:
                    f.write("region\n")

        inner_region = Region()
        inner_region.marker = Marker()
        inner_region.marker.log = log
        ref = weakref.ref(inner_region.marker)

        inner = Mutex(inner_region)
        del inner_region
        inner.release()
        inner.acquire()          # owned by this interpreter from here on

        outer_region = Region()
        outer_region.inner = inner
        outer = Mutex(outer_region)
        del outer_region
        outer.release()
        del inner
        gc.collect()

        with open(log, "w"):
            pass

        self._run_in_subinterp("""
import gc
c.acquire()
c.value.inner = None      # drops the last reference to a mutex we never owned
gc.collect()
with open(log, "a") as f:
    f.write("subinterpreter ")
c.release()
""", shared={"c": outer, "log": log})

        # The teardown was scheduled here and runs at the next eval breaker.
        recorded = []
        for _ in range(10000):
            with open(log) as f:
                recorded = f.read().split()
            if len(recorded) == 2:
                break
        self.assertEqual(recorded, ["subinterpreter", "region"])
        self.assertIsNone(ref())

    @unittest.skipUnless(immutable._cross_interpreter_sharing,
        "objects are not shared across interpreters")
    def test_owner_may_deref_and_others_may_not(self):
        """`local` was never in a region, so nothing ever re-homes the
        reference to it and it stays local to this interpreter. `owned` travels
        with the region and becomes reachable by whoever holds the mutex."""
        @freezable
        class A:
            pass

        local = A()
        local.tag = "local"

        r = Region()
        r.local_ref = weakref.ref(local)
        r.owned = A()
        r.owned.tag = "owned"
        r.owned_ref = weakref.ref(r.owned)
        c = Mutex(r)
        del r
        c.release()

        self._run_in_subinterp("""
c.acquire()
try:
    c.value.local_ref()
    raise AssertionError("reached a foreign local object")
except RuntimeError:
    pass
assert c.value.owned_ref().tag == "owned"
c.release()
""", shared={"c": c})

        c.acquire()
        self.assertEqual(c.value.local_ref().tag, "local")


@unittest.skipUnless(support.Py_GIL_DISABLED,
                     "cross-thread object ownership only exists on free threading")
class TestCrossThreadClose(unittest.TestCase):
    """Closing a region containing objects owned by another, still-running
    thread. Those objects start out non-local, so the close has to queue them
    for the owning thread to merge their refcounts and wait until that happened
    before it can account them."""

    @threading_helper.reap_threads
    def test_close_objects_owned_by_other_thread(self):
        @freezable
        class A:
            pass

        N = 128
        handoff = []
        allocated = threading.Event()
        stop = threading.Event()

        def allocate():
            # Objects allocated here are biased-refcount owned by this thread.
            handoff.append([A() for _ in range(N)])
            allocated.set()
            # Keep running bytecode so the eval breaker fires
            while not stop.is_set():
                pass

        owner = threading.Thread(target=allocate)
        owner.start()
        try:
            self.assertTrue(allocated.wait(support.SHORT_TIMEOUT))

            c = Mutex(Region())
            # Move the owner thread's objects into the region and drop every
            # reference this thread holds, so only the region reaches them.
            objs = handoff.pop()
            c.value.objs = objs
            del objs

            c.release()
            c.acquire()
            self.assertTrue(Region.is_closed(c.value))
        finally:
            stop.set()
            owner.join()

    @threading_helper.reap_threads
    def test_close_wrefed_objects_owned_by_other_thread(self):
        @freezable
        class A:
            pass

        N = 20
        handoff = []
        allocated = threading.Event()
        stop = threading.Event()

        def allocate():
            # Objects allocated here are biased-refcount owned by this thread.
            handoff.append([A() for _ in range(N)])
            allocated.set()
            # Keep running bytecode so the eval breaker fires
            while not stop.is_set():
                pass

        owner = threading.Thread(target=allocate)
        owner.start()
        try:
            self.assertTrue(allocated.wait(support.SHORT_TIMEOUT))

            c = Mutex(Region())
            # Move the owner thread's objects into the region and drop every
            # reference this thread holds, so only the region reaches them.
            objs = handoff.pop()
            c.value.objs = objs
            c.value.wref = weakref.ref(objs[0])
            del objs

            c.release()
            c.acquire()
            self.assertTrue(Region.is_closed(c.value))
        finally:
            stop.set()
            owner.join()

    @threading_helper.reap_threads
    def test_close_objects_owned_by_blocked_thread(self):
        @freezable
        class A:
            pass

        N = 128
        handoff = []
        allocated = threading.Event()
        release_owner = threading.Event()

        def allocate():
            handoff.append([A() for _ in range(N)])
            allocated.set()
            # Block without running bytecode. The owner is detached here, so it
            # won't service its merge queue via an eval breaker; the close has to
            # pin it and merge the refcounts directly instead of timing out.
            release_owner.wait(support.LONG_TIMEOUT)

        owner = threading.Thread(target=allocate)
        owner.start()
        try:
            self.assertTrue(allocated.wait(support.SHORT_TIMEOUT))

            c = Mutex(Region())
            objs = handoff.pop()
            c.value.objs = objs
            del objs

            c.release()
            c.acquire()
            self.assertTrue(Region.is_closed(c.value))
        finally:
            release_owner.set()
            owner.join()


@unittest.skipUnless(support.Py_GIL_DISABLED,
                     "the FREE owner only exists on free threading")
class TestFreeOwner(unittest.TestCase):
    """A successful close marks the closing thread's objects as FREE, so the
    next thread that touches them takes over the biased refcount."""

    N = 8

    def setUp(self):
        # Items are only inspected through their container: passing an object
        # directly would incref it and claim it for the calling thread.
        self.item_owner = import_helper.import_module(
            '_testinternalcapi').get_item_owner

    def owners(self, objs):
        return [self.item_owner(objs, i) for i in range(len(objs))]

    def make_closed_mutex(self):
        @freezable
        class A:
            pass

        c = Mutex(Region())
        c.value.objs = [A() for _ in range(self.N)]
        c.release()
        return c

    def run_in_thread(self, func):
        result = []
        t = threading.Thread(target=lambda: result.append(func()))
        t.start()
        t.join()
        self.assertEqual(len(result), 1, "thread raised an exception")
        return result[0]

    def test_close_marks_objects_free(self):
        c = self.make_closed_mutex()
        c.acquire()
        self.assertEqual(self.owners(c.value.objs), ["free"] * self.N)

    def test_gc_keeps_objects_free(self):
        c = self.make_closed_mutex()
        c.acquire()
        gc.collect()
        self.assertEqual(self.owners(c.value.objs), ["free"] * self.N)

    @threading_helper.reap_threads
    def test_other_thread_claims_touched_objects(self):
        c = self.make_closed_mutex()

        def claim():
            c.acquire()
            objs = c.value.objs
            x, y = objs[0], objs[1]
            owners = self.owners(objs)
            del x, y, objs
            c.release()
            return owners

        owners = self.run_in_thread(claim)
        self.assertEqual(owners, ["self"] * 2 + ["free"] * (self.N - 2))

    @threading_helper.reap_threads
    def test_gc_resets_claimed_objects_to_free(self):
        # The FREE owner GC bit is never cleared, so the GC hands a claimed
        # object back to FREE. This asserts the current behaviour.
        c = self.make_closed_mutex()

        def claim_and_collect():
            c.acquire()
            objs = c.value.objs
            x = objs[0]
            x.tag = "claimed"
            claimed = self.item_owner(objs, 0)
            rc_before = sys.getrefcount(x)
            gc.collect()
            after_gc = self.item_owner(objs, 0)
            rc_after = sys.getrefcount(x)
            del x, objs
            c.release()
            return claimed, after_gc, rc_before, rc_after

        claimed, after_gc, rc_before, rc_after = \
            self.run_in_thread(claim_and_collect)
        self.assertEqual(claimed, "self")
        self.assertEqual(after_gc, "free")
        self.assertEqual(rc_before, rc_after)

        c.acquire()
        self.assertEqual(c.value.objs[0].tag, "claimed")

    @threading_helper.reap_threads
    def test_reclose_from_other_thread(self):
        c = self.make_closed_mutex()

        def touch_and_reclose():
            c.acquire()
            objs = c.value.objs
            for i in range(len(objs)):
                objs[i].tag = i
            del objs
            c.release()
            return True

        self.assertTrue(self.run_in_thread(touch_and_reclose))

        c.acquire()
        objs = c.value.objs
        self.assertEqual(self.owners(objs), ["free"] * self.N)
        self.assertEqual([o.tag for o in objs], list(range(self.N)))

    @threading_helper.reap_threads
    def test_objects_of_other_threads_stay_unowned(self):
        @freezable
        class A:
            pass

        handoff = []
        allocated = threading.Event()
        stop = threading.Event()

        def allocate():
            handoff.append([A() for _ in range(self.N)])
            allocated.set()
            while not stop.is_set():
                pass

        owner = threading.Thread(target=allocate)
        owner.start()
        try:
            self.assertTrue(allocated.wait(support.SHORT_TIMEOUT))
            c = Mutex(Region())
            c.value.objs = handoff.pop()
            c.release()
            c.acquire()
            # Merged refcounts are left alone, only our own objects become FREE
            self.assertEqual(self.owners(c.value.objs), ["unowned"] * self.N)
        finally:
            stop.set()
            owner.join()


@unittest.skipUnless(support.Py_GIL_DISABLED,
                     "concurrent traces only exist on free threading")
class TestConcurrentClose(unittest.TestCase):
    """Races against a close. These are not deterministic: each close may
    succeed or fail, but only with a clean error."""

    ITERATIONS = 100

    CONCURRENT_ERRORS = (
        r"^The region could not be closed due to:",
        r"is part of a concurrent trace$",
        r"due to a concurrent access to the object at 0x[0-9a-f]+ \[[12]\]$",
        r"was opend during tracing by a concurrent thread$",
        # Concurrent opens of a sub-region keep restarting the trace
        r"could not be closed after \d+ tracing attempts$",
        r"cannot be closed since a concurrent thread accessed during tracing$",
        r"couldn't be detached due to a concurrent access$",
        # A concurrent thread may hold the bridge while it is detached
        r"couldn't be detached, due to incoming references to the bridge$",
    )

    def assert_concurrent_error(self, exc):
        msg = str(exc)
        self.assertTrue(
            any(re.search(p, msg, re.MULTILINE) for p in self.CONCURRENT_ERRORS),
            msg)

    def release_while(self, c, mutate):
        """Releases `c` while `mutate` runs in a loop on another thread.
        Returns True if the region was closed."""
        stop = threading.Event()
        started = threading.Event()

        def mutator():
            started.set()
            while not stop.is_set():
                mutate()

        t = threading.Thread(target=mutator)
        t.start()
        try:
            self.assertTrue(started.wait(support.SHORT_TIMEOUT))
            try:
                c.release()
            except RuntimeError as e:
                self.assert_concurrent_error(e)
                return False
            return True
        finally:
            stop.set()
            t.join()

    @threading_helper.reap_threads
    def test_shared_object_in_two_regions(self):
        @freezable
        class A:
            pass

        for _ in range(self.ITERATIONS):
            shared = A()
            regions = [Region(), Region()]
            for r in regions:
                r.x = shared
            del shared
            barrier = threading.Barrier(len(regions))
            errors = []

            def close(r):
                barrier.wait()
                try:
                    Region.close(r)
                except RuntimeError as e:
                    errors.append(e)

            threads = [threading.Thread(target=close, args=(r,)) for r in regions]
            for t in threads:
                t.start()
            for t in threads:
                t.join()

            # The shared object makes both regions fail
            self.assertEqual(len(errors), 2)
            for e in errors:
                self.assert_concurrent_error(e)

    @threading_helper.reap_threads
    def test_many_closes_of_one_region(self):
        # A third close must not slip in while a failed trace is still running
        @freezable
        class A:
            pass

        for _ in range(self.ITERATIONS):
            r = Region()
            r.objs = [A() for _ in range(100)]
            threads_n = 4
            barrier = threading.Barrier(threads_n)
            errors = []

            def close():
                barrier.wait()
                try:
                    Region.close(r)
                except RuntimeError as e:
                    errors.append(e)

            threads = [threading.Thread(target=close) for _ in range(threads_n)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()

            for e in errors:
                self.assert_concurrent_error(e)
            if len(errors) == threads_n:
                self.assertFalse(Region.is_closed(r))
                Region.close(r)
            self.assertTrue(Region.is_closed(r))

    @threading_helper.reap_threads
    def test_refcount_changes_during_close(self):
        @freezable
        class A:
            pass

        for _ in range(self.ITERATIONS):
            c = Mutex(Region())
            c.value.objs = [A() for _ in range(100)]
            # A weak reference only creates short lived strong references
            wref = weakref.ref(c.value.objs[50])

            def touch():
                x = wref()
                del x

            if self.release_while(c, touch):
                c.acquire()

    @threading_helper.reap_threads
    def test_locked_mutation_during_close(self):
        # Rotating the list moves references under its lock without changing
        # any refcount, so only the write watch can catch it.
        rotate = import_helper.import_module(
            '_testinternalcapi').rotate_list_via_weakref

        @freezable
        class L(list):
            pass

        @freezable
        class A:
            pass

        for _ in range(self.ITERATIONS):
            c = Mutex(Region())
            c.value.objs = L(A() for _ in range(100))
            wref = weakref.ref(c.value.objs)

            if self.release_while(c, lambda: rotate(wref)):
                c.acquire()

    @threading_helper.reap_threads
    def test_sub_region_opened_during_close(self):
        @freezable
        class A:
            pass

        for _ in range(self.ITERATIONS):
            c = Mutex(Region())
            c.value.sub = Region()
            c.value.sub.a = A()
            sub = [c.value.sub]
            Region.close(sub[0])

            def open_sub():
                sub[0].x = 1

            # The reference to the sub-region bridge keeps the parent open
            self.assertFalse(self.release_while(c, open_sub))

    @threading_helper.reap_threads
    def test_bridge_accessed_during_close(self):
        @freezable
        class A:
            pass

        for _ in range(self.ITERATIONS):
            c = Mutex(Region())
            c.value.objs = [A() for _ in range(100)]
            bridge = [c.value]

            def touch_bridge():
                bridge[0].x = 1

            # The reference to the bridge prevents the detach
            self.assertFalse(self.release_while(c, touch_bridge))


class TestMutex(unittest.TestCase):
    def test_default_value(self):
        m = Mutex()
        self.assertIsNone(m.value)
        self.assertEqual(Mutex(value=(1, 2)).value, (1, 2))

    def test_new_without_init(self):
        m = Mutex.__new__(Mutex)
        self.assertTrue(m.owned())
        self.assertIsNone(m.value)
        m.release()
        self.assertFalse(m.locked())
        self.assertEqual(Mutex.__new__(Mutex, (1, 2)).value, (1, 2))

    def test_reinit_is_noop(self):
        r = Region()
        m = Mutex(r)
        m.__init__(None)
        self.assertTrue(m.owned())
        self.assertIs(m.value, r)
        del r
        m.release()
        self.assertFalse(m.locked())

    @unittest.skipIf(immutable._cross_interpreter_sharing,
        "mutexes are owned by interpreters, not threads")
    @threading_helper.reap_threads
    def test_reinit_from_other_thread_is_noop(self):
        r = Region()
        m = Mutex(r)
        t = threading.Thread(target=m.__init__, args=(None,))
        t.start()
        t.join()
        self.assertTrue(m.owned())
        self.assertIs(m.value, r)
        del r
        m.release()
        self.assertFalse(m.locked())

    def test_type_is_frozen(self):
        self.assertTrue(is_deep_frozen(Mutex))
        deep_freeze(Mutex)
        self.assertIsNone(Mutex().value)

    def test_reject_local_mutable(self):
        msg = "attempted to store a local mutable object in a mutex"
        with self.assertRaisesRegex(RuntimeError, msg):
            Mutex([])
        m = Mutex()
        with self.assertRaisesRegex(RuntimeError, msg):
            m.value = []
        self.assertIsNone(m.value)

    def test_accepted_values(self):
        m = Mutex()
        frozen = deep_freeze([1])
        m.value = frozen
        self.assertIs(m.value, frozen)
        r = Region()
        m.value = r
        self.assertIs(m.value, r)

    def test_locked_owned_and_repr(self):
        m = Mutex()
        self.assertTrue(m.locked())
        self.assertTrue(m.owned())
        self.assertRegex(repr(m), r"^Mutex\(owner=\d+ \(this\), value=None\)$")

        m.release()
        self.assertFalse(m.locked())
        self.assertFalse(m.owned())
        self.assertEqual(repr(m), "Mutex(owner=None, status=Released)")

    def test_repr_propagates_value_error(self):
        @freezable
        class BadRepr:
            def __repr__(self):
                raise ZeroDivisionError("repr failed")

        m = Mutex(deep_freeze(BadRepr()))
        with self.assertRaisesRegex(ZeroDivisionError, "repr failed"):
            repr(m)

    def test_acquire_argument_errors(self):
        m = Mutex()
        with self.assertRaisesRegex(ValueError, "can't specify a timeout"):
            m.acquire(blocking=False, timeout=1)
        with self.assertRaisesRegex(ValueError, "must be a non-negative number"):
            m.acquire(timeout=-5)
        with self.assertRaises(OverflowError):
            m.acquire(timeout=1e300)
        self.assertTrue(m.owned())

    def test_access_after_release(self):
        m = Mutex()
        m.release()
        with self.assertRaisesRegex(RuntimeError, r"attempted to access a mutex owned by \d+ from \d+"):
            m.value
        with self.assertRaisesRegex(RuntimeError, r"attempted to access a mutex owned by \d+ from \d+"):
            m.value = None
        with self.assertRaisesRegex(RuntimeError, r"owner \d+ attempted to release a released mutex"):
            m.release()

    @unittest.skipIf(immutable._cross_interpreter_sharing,
        "mutexes are owned by interpreters, not threads")
    @threading_helper.reap_threads
    def test_held_by_other_thread(self):
        m = Mutex()
        m.release()
        acquired = threading.Event()
        done = threading.Event()

        def hold():
            m.acquire()
            acquired.set()
            done.wait(support.LONG_TIMEOUT)
            m.release()

        t = threading.Thread(target=hold)
        t.start()
        try:
            self.assertTrue(acquired.wait(support.SHORT_TIMEOUT))
            self.assertTrue(m.locked())
            self.assertFalse(m.owned())
            self.assertRegex(repr(m), r"^Mutex\(owner=\d+ \(other\)\)$")

            self.assertFalse(m.acquire(blocking=False))
            self.assertFalse(m.acquire(timeout=0.01))
            with self.assertRaisesRegex(RuntimeError, r"attempted to access a mutex owned by \d+ from \d+"):
                m.value
            with self.assertRaisesRegex(RuntimeError, r"owner \d+ attempted to release a mutex owned by \d+"):
                m.release()
        finally:
            done.set()
            t.join()

        self.assertTrue(m.acquire(timeout=support.SHORT_TIMEOUT))
        self.assertTrue(m.owned())

    def test_alive_at_shutdown(self):
        # The GC clears the mutex during finalization, before it is deallocated
        for code in ("m = Mutex()",
                     "m = Mutex(); m.release()",
                     "m = Mutex(TracingRegion())"):
            with self.subTest(code=code):
                script_helper.assert_python_ok(
                    "-c", f"from immutable import Mutex, TracingRegion; {code}")


class TestMutexSubinterpreters(unittest.TestCase):
    def setUp(self):
        self._interpreters = import_helper.import_module('_interpreters')

    def _run_in_subinterp(self, code, shared=None):
        interp = self._interpreters.create()
        try:
            # run_string() reports errors by returning them, not by raising
            err = self._interpreters.run_string(interp, code, shared=shared or {})
            if err is not None:
                self.fail(f"subinterpreter raised:\n{err.errdisplay}")
        finally:
            self._interpreters.destroy(interp)

    @unittest.skipUnless(immutable._cross_interpreter_sharing,
        "objects are not shared across interpreters")
    def test_share_mutex_with_frozen_value(self):
        m = Mutex(deep_freeze([1, 2]))
        m.release()
        self._run_in_subinterp("""
m.acquire()
assert m.value == [1, 2]
m.release()
""", shared={"m": m})
        m.acquire()
        self.assertEqual(m.value, [1, 2])

    @unittest.skipUnless(immutable._cross_interpreter_sharing,
        "objects are not shared across interpreters")
    def test_share_mutex_with_region(self):
        @freezable
        class A:
            pass

        m = Mutex(Region())
        m.value.a = A()
        m.value.a.tag = 1
        m.release()
        self._run_in_subinterp("""
m.acquire()
assert m.value.a.tag == 1
m.value.a.tag = 2
m.release()
""", shared={"m": m})
        m.acquire()
        self.assertEqual(m.value.a.tag, 2)
