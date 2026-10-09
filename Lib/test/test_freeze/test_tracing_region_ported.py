"""Tests ported from a different implementation of regions (the "experiment"
branch, Lib/test/test_free_threading/test_region.py and test_cown.py).

Only the tests of what closing a region means were ported, not those of the
experiment's machinery (colours, claims, grace periods, tags). Where the
experiment expects something that this implementation does differently by
design, the test keeps the experiment's expectation and is marked with
`diverges()`, which records why.
"""

import copy
import gc
import pickle
import random
import threading
import time
import unittest
import weakref

from immutable import TracingRegion as Region, Cown
from test import support
from test.support import import_helper, threading_helper

_testinternalcapi = import_helper.import_module("_testinternalcapi")
region_close = _testinternalcapi.region_close
region_is_closed = _testinternalcapi.region_is_closed


def close(r):
    """Close `r` without the cown's check for references to the bridge.
    Return True if `r` is now closed, False if it could not be closed."""
    try:
        region_close(r)
    except RuntimeError:
        return False
    return True


def region_state(r):
    return "closed" if region_is_closed(r) else "open"


def diverges(reason):
    """The experiment expects what the test checks, this implementation does
    something else by design. The test is an expected failure, so that it
    reports an unexpected success should the behaviour change."""
    def decorator(test):
        test.diverges = reason
        return unittest.expectedFailure(test)
    return decorator


needs_close_pause = unittest.skipUnless(
    hasattr(_testinternalcapi, "region_set_close_pause"),
    "needs the close pause of debug builds")


class close_pause:
    """While inside, every close pauses for `seconds` between its trace and
    the validation, with the thread detached, so that other threads can
    interfere with it."""

    def __init__(self, seconds):
        self.seconds = seconds

    def __enter__(self):
        _testinternalcapi.region_set_close_pause(self.seconds)
        return self

    def __exit__(self, *exc):
        _testinternalcapi.region_set_close_pause(0)


def run_in_thread(func, *args):
    """Run func(*args) in a new thread; return its result or raise its
    exception."""
    result = []

    def run():
        try:
            result.append((True, func(*args)))
        except BaseException as e:
            result.append((False, e))

    t = threading.Thread(target=run)
    t.start()
    t.join()
    ok, value = result[0]
    if not ok:
        raise value
    return value


class Foo:
    pass


class OwnRef(weakref.ref):
    # A weak reference of our own: the basic one may be shared.
    __slots__ = ()


def make_graph():
    # A graph of mortal objects created by the calling thread, with cycles
    # and edges to objects that are not part of regions (immortals, types).
    # Values are computed at run time, a constant would be shared with the
    # code object.
    foo = Foo()
    foo.items = [Foo(), {"key": Foo(), "n": pow(10, 30)},
                 (Foo(), str(10**6))]
    foo.items[0].back = foo
    foo.cls = Foo
    foo.none = None
    s = {str(10**7), 1}
    return [foo, s, foo.items]


def nest(*objs):
    """Return a new region whose contents is a list of `objs`."""
    r = Region()
    r.contents = list(objs)
    return r


class TestRegionStates(unittest.TestCase):

    def test_new_region_is_open(self):
        self.assertEqual(region_state(Region()), "open")

    def test_contents(self):
        r = Region()
        foo = Foo()
        r.contents = foo
        self.assertIs(r.contents, foo)
        del r.contents
        with self.assertRaises(AttributeError):
            r.contents

    def test_close(self):
        r = Region()
        r.contents = [Foo(), {"a": Foo()}]
        self.assertIs(close(r), True)
        self.assertEqual(region_state(r), "closed")
        # Closing a closed region is a no-op.
        self.assertIs(close(r), True)
        self.assertEqual(region_state(r), "closed")

    def test_access_opens(self):
        for access in (lambda r: r.contents,
                       lambda r: setattr(r, "contents", Foo()),
                       lambda r: delattr(r, "contents"),
                       lambda r: r.__dict__,
                       lambda r: vars(r),
                       lambda r: object.__getattribute__(r, "__dict__")):
            with self.subTest(access=access):
                r = Region()
                r.contents = Foo()
                self.assertTrue(close(r))
                access(r)
                self.assertEqual(region_state(r), "open")

    def test_repr_does_not_open(self):
        r = Region()
        close(r)
        self.assertIn("closed", repr(r))
        self.assertEqual(region_state(r), "closed")


class TestRegionType(unittest.TestCase):

    def test_not_subclassable(self):
        with self.assertRaises(TypeError):
            class Sub(Region):
                pass

    def test_no_pickle_or_copy(self):
        r = Region()
        for proto in range(pickle.HIGHEST_PROTOCOL + 1):
            with self.subTest(proto=proto):
                with self.assertRaises(TypeError):
                    pickle.dumps(r, proto)
        with self.assertRaises(TypeError):
            copy.copy(r)
        with self.assertRaises(TypeError):
            copy.deepcopy(r)

    def test_constructor_takes_no_arguments(self):
        with self.assertRaises(TypeError):
            Region(1)
        with self.assertRaises(TypeError):
            Region(contents=1)

    def test_gc(self):
        # A region in a reference cycle is collected.
        r = Region()
        foo = Foo()
        foo.region = r
        r.contents = foo
        wr = weakref.ref(foo)
        del r, foo
        gc.collect()
        self.assertIsNone(wr())


class TestIsolation(unittest.TestCase):

    def check_closes(self, contents):
        r = Region()
        r.contents = contents
        del contents
        self.assertIs(close(r), True)
        self.assertEqual(region_state(r), "closed")
        return r

    def check_does_not_close(self, r):
        self.assertIs(close(r), False)
        self.assertEqual(region_state(r), "open")

    def test_empty(self):
        self.check_closes(None)

    def test_isolated_graph(self):
        self.check_closes(make_graph())

    def test_leaf(self):
        self.check_closes(str(10**8))

    def test_strings_are_not_part_of_regions(self):
        # Strings are immutable: shared with the outside or between regions,
        # they do not keep a region from closing.
        shared = str(10**9)
        self.check_closes([shared, Foo()])
        r1 = self.check_closes([shared])
        r2 = self.check_closes({shared: 1})
        self.assertEqual(region_state(r1), "closed")
        self.assertEqual(region_state(r2), "closed")

    def test_index_by_string_closes(self):
        # dict_traverse() does not report the keys of all-string dicts, so
        # strings referenced both as keys and from elsewhere in the graph can
        # look externally referenced.
        class Item:
            def __init__(self, i):
                self.name = f"item{i}"
        def build():
            items = [Item(i) for i in range(5)]
            return [items, {o.name: o for o in items}]
        self.check_closes(build())

    def test_str_subclass_is_part_of_regions(self):
        class S(str):
            pass
        r = Region()
        r.contents = [S("mutable attributes")]
        inner = r.contents[0]
        self.check_does_not_close(r)
        del inner
        self.assertIs(close(r), True)

    def test_reference_back_to_region(self):
        # References to the region itself are allowed, also from inside.
        r = Region()
        graph = make_graph()
        graph[0].region = r
        r.contents = graph
        del graph
        others = [r, r]
        self.assertIs(close(r), True)

    def test_external_reference_to_interior(self):
        r = Region()
        graph = make_graph()
        r.contents = graph
        inner = graph[0].items[1]
        del graph
        self.check_does_not_close(r)
        del inner
        self.assertIs(close(r), True)

    def test_external_reference_to_contents(self):
        r = Region()
        graph = make_graph()
        r.contents = graph
        self.check_does_not_close(r)
        del graph
        self.assertIs(close(r), True)

    def test_reference_from_other_container(self):
        r = Region()
        graph = make_graph()
        r.contents = graph
        outside = {"alias": graph[1]}
        del graph
        self.check_does_not_close(r)
        outside.clear()
        self.assertIs(close(r), True)

    @threading_helper.requires_working_threading()
    def test_reference_held_by_other_thread(self):
        r = Region()
        r.contents = make_graph()
        holding = threading.Event()
        release = threading.Event()

        def hold(obj):
            holding.set()
            release.wait()

        t = threading.Thread(target=hold, args=(r.contents[0].items[0],))
        t.start()
        holding.wait()
        try:
            self.check_does_not_close(r)
        finally:
            release.set()
            t.join()
        self.assertIs(close(r), True)

    def test_object_in_two_regions(self):
        shared = Foo()
        r1, r2 = Region(), Region()
        r1.contents = [shared]
        r2.contents = [shared]
        del shared
        self.check_does_not_close(r1)
        self.check_does_not_close(r2)

    def test_reopen_and_close_again(self):
        r = self.check_closes(make_graph())
        alias = r.contents     # opens the region
        self.assertEqual(region_state(r), "open")
        self.check_does_not_close(r)
        del alias
        self.assertIs(close(r), True)


class TestSubregions(unittest.TestCase):
    # The builders below return only the outermost region, so that the test
    # holds no references into it.

    def test_open_subregion_closes_with_parent(self):
        outer = nest(nest(Foo()), Foo())
        self.assertTrue(close(outer))
        inner = outer.contents[0]     # opens the outer region only
        self.assertEqual(region_state(outer), "open")
        self.assertEqual(region_state(inner), "closed")

    def test_three_levels(self):
        outer = nest(nest(nest(Foo()), Foo()))
        self.assertTrue(close(outer))
        middle = outer.contents[0]
        inner = middle.contents[0]
        self.assertEqual(region_state(inner), "closed")
        # Closing the outer region again walks the open middle one and
        # counts the closed inner one.
        del middle, inner
        self.assertTrue(close(outer))

    def test_subregion_with_external_reference(self):
        for inner_closed in (False, True):
            with self.subTest(inner_closed=inner_closed):
                inner = nest(Foo())
                if inner_closed:
                    self.assertTrue(close(inner))
                outer = nest(inner)
                self.assertFalse(close(outer))
                self.assertEqual(region_state(outer), "open")

    @diverges("a failed close leaves the subregions it closed on the way "
              "closed, the experiment restores them to open")
    def test_subregion_with_external_reference_keeps_its_state(self):
        inner = nest(Foo())
        outer = nest(inner)
        self.assertFalse(close(outer))
        self.assertEqual(region_state(inner), "open")

    def test_failure_leaves_closed_subregions_closed(self):
        x = Foo()
        closed = nest(Foo())
        self.assertTrue(close(closed))
        outer = nest(nest(Foo()), closed, x)   # x is referenced from here
        del closed
        self.assertFalse(close(outer))
        _, closed_inner, _ = outer.contents
        self.assertEqual(region_state(closed_inner), "closed")
        del closed_inner, _
        del x
        self.assertTrue(close(outer))

    @diverges("a failed close leaves the subregions it closed on the way "
              "closed, the experiment restores them to open")
    def test_failure_restores_subregions(self):
        x = Foo()
        outer = nest(nest(Foo()), x)   # x is referenced from here
        self.assertFalse(close(outer))
        open_inner = outer.contents[0]
        self.assertEqual(region_state(open_inner), "open")

    def test_references_to_subregion(self):
        # A parent may refer to a subregion any number of times, and a
        # region's graph may refer back to the region.
        def build():
            inner = Region()
            inner.contents = [inner, {"self": inner}]
            return nest(inner, inner, (inner,))
        outer = build()
        self.assertTrue(close(outer))
        # The closed subregion's back-references are recorded.
        outer.contents
        self.assertTrue(close(outer))

    def test_edges_across_regions(self):
        def into_subregion():
            x = Foo()
            return nest(nest(x), x)
        def into_parent():
            x = Foo()
            outer = Region()
            inner = nest(x)
            outer.contents = [x, inner]
            return outer
        def to_parent_region():
            outer = Region()
            outer.contents = [nest(outer)]
            return outer
        def to_sibling():
            second = nest(Foo())
            return nest(nest(second), second)
        def to_grandparent_object():
            x = Foo()
            return nest(x, nest(nest(x)))
        for build in (into_subregion, into_parent, to_parent_region,
                      to_sibling, to_grandparent_object):
            with self.subTest(build=build.__name__):
                outer = build()
                self.assertFalse(close(outer))
                self.assertEqual(region_state(outer), "open")
                outer.contents = None   # break any cycle

    @threading_helper.requires_working_threading()
    def test_subregion_of_other_thread(self):
        # The experiment refuses this while the other thread lives, since
        # there an open region is owned by the thread that opened it. Here
        # regions have no owning thread, so the subregion is closed with its
        # parent, after its objects were made shared.
        made = threading.Event()
        done = threading.Event()
        box = []
        def make():
            box.append(nest(Foo()))   # created by this thread, open
            made.set()
            done.wait()
        t = threading.Thread(target=make)
        t.start()
        try:
            made.wait()
            outer = nest(box.pop())
            self.assertTrue(close(outer))
        finally:
            done.set()
            t.join()
        inner = outer.contents[0]
        self.assertEqual(region_state(inner), "closed")

    def test_cown_is_not_walked(self):
        # A cown's value is its own business, even if it is not isolated (so
        # that the cown could not be released), and references to a cown
        # from outside are fine.
        x = Foo()
        c = Cown(nest(x))
        r = nest(c)
        self.assertTrue(close(r))
        c.value = None
        c.release()

    def test_weakref_into_subregion(self):
        def build():
            obj = Foo()
            return nest(nest(obj)), OwnRef(obj)
        outer, wr = build()
        self.assertTrue(close(outer))
        inner = outer.contents[0]    # opens the outer region only
        self.assertIsNone(wr())      # the referent's region is closed

    @diverges("closing clears weak references from outside for good, the "
              "experiment refuses them only while the region is closed")
    def test_weakref_into_subregion_after_opening(self):
        def build():
            obj = Foo()
            return nest(nest(obj)), OwnRef(obj)
        outer, wr = build()
        self.assertTrue(close(outer))
        outer.contents[0].contents   # opens both
        self.assertIsInstance(wr(), Foo)

    def test_empty_subregions(self):
        self.assertTrue(close(nest(nest(Foo()), nest())))


class TestRegionWeakrefs(unittest.TestCase):

    def region_with(self, obj):
        r = Region()
        r.contents = [obj]
        return r

    @threading_helper.requires_working_threading()
    def test_external_weakref_refused_after_close(self):
        obj = Foo()
        wr = weakref.ref(obj)
        r = self.region_with(obj)
        del obj
        self.assertTrue(close(r))
        self.assertIsNone(wr())
        self.assertIsNone(run_in_thread(wr))

    @diverges("closing clears weak references from outside for good, the "
              "experiment refuses them only while the region is closed")
    def test_external_weakref_usable_after_opening(self):
        obj = Foo()
        wr = weakref.ref(obj)
        r = self.region_with(obj)
        del obj
        self.assertTrue(close(r))
        contents = r.contents
        self.assertIs(wr(), contents[0])

    def test_proxy_refused_after_close(self):
        obj = Foo()
        obj.x = 1
        proxy = weakref.proxy(obj)
        r = self.region_with(obj)
        del obj
        self.assertTrue(close(r))
        with self.assertRaises(ReferenceError):
            proxy.x

    @diverges("closing clears weak references from outside for good, the "
              "experiment refuses them only while the region is closed")
    def test_proxy_usable_after_opening(self):
        obj = Foo()
        obj.x = 1
        proxy = weakref.proxy(obj)
        r = self.region_with(obj)
        del obj
        self.assertTrue(close(r))
        r.contents
        self.assertEqual(proxy.x, 1)

    def test_weak_edges_inside_region(self):
        obj = Foo()
        inner = weakref.ref(obj)
        r = Region()
        r.contents = [obj, inner]
        del obj, inner
        self.assertTrue(close(r))
        contents = r.contents
        self.assertIs(contents[1](), contents[0])

    def test_outgoing_weakref_rejected(self):
        outside = Foo()
        r = Region()
        r.contents = [weakref.ref(outside)]
        self.assertIs(close(r), False)
        self.assertEqual(region_state(r), "open")

    def test_outgoing_weakref_to_excluded_or_dead_allowed(self):
        # A weak reference to a type (not part of regions) is allowed, and
        # so is a dead one.
        r = Region()
        r.contents = [OwnRef(Foo)]
        self.assertTrue(close(r))
        dead = Foo()
        r = Region()
        r.contents = [OwnRef(dead)]
        del dead
        self.assertTrue(close(r))

    def test_weak_value_dictionary(self):
        obj = Foo()
        d = weakref.WeakValueDictionary()
        d["k"] = obj
        r = self.region_with(obj)
        del obj
        self.assertTrue(close(r))
        self.assertNotIn("k", d)


@threading_helper.requires_working_threading()
class TestConcurrentClose(unittest.TestCase):

    @needs_close_pause
    def test_access_while_closing_fails_the_close(self):
        # The experiment refuses the access instead. Here the access goes
        # through and the close fails.
        r = Region()
        r.contents = make_graph()
        accessed = []

        def access():
            time.sleep(0.1)  # the close is pausing
            accessed.append(r.contents is not None)

        t = threading.Thread(target=access)
        with close_pause(0.4):
            t.start()
            self.assertIs(close(r), False)
        t.join()
        self.assertEqual(accessed, [True])
        self.assertEqual(region_state(r), "open")
        self.assertIs(close(r), True)

    def check_transient_access(self, keep):
        # While the close pauses, another thread takes a reference to the
        # contents without going through the region's attributes
        # (gc.get_referents() is an escape hatch) and, unless `keep`, drops
        # it again. The count is then back to what the trace recorded; only
        # the trace flag tells.
        r = Region()
        r.contents = make_graph()
        kept = []

        def touch():
            time.sleep(0.1)  # the close is pausing
            if keep:
                kept.append(gc.get_referents(r))
            else:
                gc.get_referents(r)

        t = threading.Thread(target=touch)
        with close_pause(0.4):
            t.start()
            self.assertIs(close(r), False)
        t.join()
        self.assertEqual(region_state(r), "open")
        kept.clear()
        self.assertIs(close(r), True)

    @needs_close_pause
    @unittest.skipUnless(support.Py_GIL_DISABLED,
                         "with the GIL a trace is never interleaved")
    def test_reference_taken_and_dropped_during_close(self):
        self.check_transient_access(keep=False)

    @needs_close_pause
    @unittest.skipUnless(support.Py_GIL_DISABLED,
                         "with the GIL a trace is never interleaved")
    def test_reference_taken_during_close(self):
        self.check_transient_access(keep=True)

    def test_overlapping_regions_closed_concurrently(self):
        # Each thread creates and closes its own region; all regions contain
        # the same object.
        nregions = 4
        shared = [Foo()]
        results = []
        regions = []   # keeps every region alive until all are closed
        barrier = threading.Barrier(nregions)

        def close_own():
            r = Region()
            r.contents = [shared[0], Foo()]
            regions.append(r)
            barrier.wait()   # all regions exist
            del shared[:]
            barrier.wait()
            results.append(close(r))

        threads = [threading.Thread(target=close_own) for _ in range(nregions)]
        with threading_helper.start_threads(threads):
            pass
        self.assertEqual(results, [False] * nregions)

    def test_disjoint_regions_closed_concurrently(self):
        nregions = 8
        results = []
        barrier = threading.Barrier(nregions)

        def close_own():
            r = Region()
            r.contents = make_graph()
            barrier.wait()
            results.append(close(r))

        threads = [threading.Thread(target=close_own) for _ in range(nregions)]
        with threading_helper.start_threads(threads):
            pass
        self.assertEqual(results, [True] * nregions)

    def test_gc_during_close(self):
        stop = threading.Event()

        def collect():
            # Pause between collections: back-to-back stop-the-world pauses
            # would starve the other threads.
            while not stop.is_set():
                gc.collect()
                time.sleep(0.001)

        t = threading.Thread(target=collect)
        t.start()
        try:
            for _ in range(50):
                r = Region()
                r.contents = make_graph()
                self.assertIs(close(r), True)
        finally:
            stop.set()
            t.join()

    def test_stress(self):
        # Threads keep taking and dropping references to interior objects
        # (through the escape hatch) while the main thread keeps closing and
        # reopening the region. Results may be either; nothing may crash or
        # leak.
        r = Region()
        r.contents = make_graph()
        wr = weakref.ref(r.contents[0])
        stop = threading.Event()
        errors = []

        def churn():
            try:
                while not stop.is_set():
                    for obj in gc.get_referents(r):
                        for inner in gc.get_referents(obj):
                            pass
            except BaseException as e:
                errors.append(e)

        threads = [threading.Thread(target=churn) for _ in range(4)]
        results = set()
        with threading_helper.start_threads(threads):
            try:
                deadline = time.monotonic() + 1.0
                while time.monotonic() < deadline:
                    results.add(close(r))
                    alias = r.contents   # reopen
                    del alias
            finally:
                stop.set()
        self.assertEqual(errors, [])
        self.assertLessEqual(results, {True, False})
        self.assertIs(close(r), True)
        del r
        gc.collect()
        self.assertIsNone(wr())


class acquired:
    """`with acquired(c):` acquires the cown and releases it at the end. The
    experiment's cowns are context managers themselves."""

    def __init__(self, cown):
        self.cown = cown

    def __enter__(self):
        self.cown.acquire()
        return self.cown

    def __exit__(self, *exc):
        self.cown.release()


class TestCownRegions(unittest.TestCase):
    # Releasing a cown closes the region it holds, and requires the cown's
    # `value` to be the only reference to the region. Cowns are acquired when
    # created.

    def test_release_closes_region(self):
        c = Cown(nest(Foo(), [Foo()]))
        self.assertEqual(region_state(c.value), "open")
        c.release()
        with acquired(c):
            self.assertEqual(region_state(c.value), "closed")

    def test_release_closes_subregions(self):
        c = Cown(nest(nest(Foo())))
        c.release()
        with acquired(c):
            inner = c.value.contents[0]
            self.assertEqual(region_state(inner), "closed")
            del inner

    def test_reference_to_region(self):
        r = nest(Foo())
        c = Cown(r)
        with self.assertRaisesRegex(RuntimeError, "references to the bridge"):
            c.release()
        self.assertIs(c.value, r)    # still acquired
        del r
        c.release()

    def test_region_not_isolated(self):
        x = Foo()
        c = Cown(nest(x))
        with self.assertRaisesRegex(RuntimeError, "could not be closed"):
            c.release()
        self.assertEqual(region_state(c.value), "open")
        del x
        c.release()

    def test_back_references(self):
        r = Region()
        r.contents = [r, {"r": r}]
        c = Cown(r)
        del r
        c.release()
        c.acquire()
        c.release()                  # closed: recorded back-references
        with acquired(c):
            c.value.contents         # opened: walked again

    def test_move_between_cowns(self):
        a = Cown(nest(Foo()))
        a.release()
        b = Cown()
        a.acquire()
        b.value = a.value
        a.value = None
        b.release()
        a.release()
        with acquired(b):
            self.assertEqual(region_state(b.value), "closed")

    def test_same_region_in_two_cowns(self):
        a = Cown(nest(Foo()))
        b = Cown()
        b.value = a.value
        with self.assertRaisesRegex(RuntimeError, "references to the bridge"):
            b.release()
        with self.assertRaisesRegex(RuntimeError, "references to the bridge"):
            a.release()
        b.value = None
        b.release()
        a.release()

    def test_cown_in_region(self):
        inner = Cown()
        inner.release()
        c = Cown(nest(inner, Foo()))
        c.release()
        with acquired(inner):        # shared with the region: fine
            pass

    @threading_helper.requires_working_threading()
    def test_handover(self):
        c = Cown(nest())
        c.release()
        def append():
            with acquired(c):
                c.value.contents.append(Foo())
        for _ in range(3):
            run_in_thread(append)
        with acquired(c):
            self.assertEqual(len(c.value.contents), 3)


# Helpers for the stress test. Each works in its own frame, so that its
# local references into the region are gone before the cown is released.

def bump(c, rng):
    # The region holds [counter, [objects...], subregion]; the subregion
    # holds [counter].
    data = c.value.contents
    data[0] += 1
    objs = data[1]
    if objs and rng.random() < 0.5:
        objs.pop()
    else:
        f = Foo()
        f.me = f            # a cycle: only the GC frees it once popped
        objs.append(f)
    if rng.random() < 0.3:
        inner = data[2].contents       # opens the subregion
        inner[0] += 1
        return 2
    return 1


def total(c):
    data = c.value.contents
    return data[0] + data[2].contents[0]


def stress_region():
    inner = Region()
    inner.contents = [0]
    r = Region()
    r.contents = [0, [], inner]
    return r


@threading_helper.requires_working_threading()
class TestCownStress(unittest.TestCase):

    def test_pass_regions_around(self):
        self.pass_regions_around()

    def test_pass_regions_around_with_gc(self):
        # As above, while another thread keeps collecting garbage.
        stop = threading.Event()
        def collect():
            while not stop.is_set():
                gc.collect()
                time.sleep(0.002)
        t = threading.Thread(target=collect)
        t.start()
        try:
            self.pass_regions_around()
        finally:
            stop.set()
            t.join()

    def pass_regions_around(self):
        # Threads acquire cowns at random, update the regions in them (and
        # their subregions), sometimes swap the regions of two cowns (without
        # opening them), and release; every release closes what was opened.
        NTHREADS = 6
        NCOWNS = 4
        ITERATIONS = 300
        cowns = [Cown(stress_region()) for _ in range(NCOWNS)]
        for c in cowns:
            c.release()
        bumps = [0] * NTHREADS
        errors = []

        def release(c):
            # A failed release leaves the cown acquired; empty it and
            # release it, so that the other workers do not wait for it
            # forever (the test fails on the recorded error).
            try:
                c.release()
            except Exception as e:
                errors.append(e)
                c.value = None
                c.release()

        def worker(i):
            rng = random.Random(i)
            try:
                for _ in range(ITERATIONS):
                    if rng.random() < 0.2:
                        a, b = sorted(rng.sample(range(NCOWNS), 2))
                        cowns[a].acquire()
                        cowns[b].acquire()
                        try:
                            cowns[a].value, cowns[b].value = (
                                cowns[b].value, cowns[a].value)
                        finally:
                            release(cowns[b])
                            release(cowns[a])
                    else:
                        c = cowns[rng.randrange(NCOWNS)]
                        c.acquire()
                        try:
                            if c.value is not None:
                                bumps[i] += bump(c, rng)
                        finally:
                            release(c)
            except BaseException as e:
                errors.append(e)

        threads = [threading.Thread(target=worker, args=(i,))
                   for i in range(NTHREADS)]
        with threading_helper.start_threads(threads):
            pass
        self.assertEqual(errors, [])
        found = 0
        for c in cowns:
            with acquired(c):
                self.assertEqual(region_state(c.value), "closed")
                found += total(c)
        self.assertEqual(found, sum(bumps))

    def test_release_closed_regions(self):
        # Acquiring and releasing without opening the region only reads its
        # reference count; many threads doing so do not interfere.
        c = Cown(stress_region())
        c.release()
        def worker():
            for _ in range(2000):
                with acquired(c):
                    pass
        threading_helper.run_concurrently(worker, nthreads=8)
        with acquired(c):
            self.assertEqual(region_state(c.value), "closed")


if __name__ == "__main__":
    unittest.main()
