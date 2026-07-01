import gc
import test.support
import unittest

from immutable import InterpreterLocal
from regions import Cown, Region


class TestRegionGC(unittest.TestCase):
    class A:
        pass

    class Resurrectable:
        def __init__(self, data):
            self.data = data

        def __del__(self):
            self.data["counter"] += 1
            self.data["instance"] = self

    class GcDetector:
        def __init__(self, data):
            self.data = data
            self.loop = self

        def __del__(self):
            self.data["counter"] += 1

    class CownReleaser:
        def __init__(self, to_release):
            self.to_release = to_release
            self.loop = self

        def __del__(self):
            self.to_release.release()

    class RegionOpener:
        def __init__(self, iplocal):
            self.iplocal = iplocal
            self.loop = self

        def __del__(self):
            self.iplocal.set(self)

    def setUp(self):
        # Need to run collection multiple times to clean up region chains
        while gc.collect() > 0:
            pass

    def tearDown(self):
        # All tests run with the GC disabled, but some tests can enable it.
        # Once all tests are done, tearDownModule restores the GC state.
        gc.disable()


    def build_cycle(self):
        a = self.A()
        a.b = self.A()
        a.b.a = a
        return a

    def build_region_with_unreachable_cycle(self):
        r = Region()
        r.a = self.build_cycle()
        r.a = None
        return r

    def build_detector_cown(self):
        r = Region()
        r.data = {"counter": 0}
        r.detector = self.GcDetector(r.data)
        r.detector = None
        return Cown(r)

    def test_local_gc_ignores_regions(self):
        r = Region()

        # A normal cycle should be collected
        self.build_cycle()
        self.assertEqual(gc.collect(), 2)
        self.assertEqual(gc.collect(), 0)

        # A cycle inside a region should be ignored
        r.a = self.build_cycle()
        r.a = None
        self.assertEqual(gc.collect(), 0)

        # Dissolving a region should allow cycles to be collected again
        r = None
        self.assertEqual(gc.collect(), 2)

    def test_collect_cycle(self):
        r = self.build_region_with_unreachable_cycle()

        # The cycle inside the region should be collected
        self.assertEqual(gc.collect_region(r), 2)

    def test_acquired_cown(self):
        r = self.build_region_with_unreachable_cycle()
        cown = Cown(r)
        r = None

        # The cycle inside the region should be collected
        self.assertEqual(gc.collect_region(cown), 2)

    def test_released_cown(self):
        r = self.build_region_with_unreachable_cycle()
        cown = Cown(r)
        r = None
        cown.release()

        # If passing a cown, it needs to be acquired
        with self.assertRaises(TypeError):
            gc.collect_region(cown)

    def test_collect_cycle_with_backlink(self):
        r = Region()
        r.a = self.build_cycle()
        r.a.r = r
        r.a = None

        # The cycle inside the region should be collected
        self.assertEqual(gc.collect_region(r), 2)

    def test_collect_child_region(self):
        r = Region()
        r.child = self.build_region_with_unreachable_cycle()

        # The cycle inside the child region should be collected
        self.assertEqual(gc.collect_region(r), 2)

    def test_collect_unreachable_child_region(self):
        r = Region()
        r.a = self.build_cycle()
        r.a.child = self.build_region_with_unreachable_cycle()
        r.a = None

        # Both cycles should be collected.
        # Note that the bridge object is never counted;
        # perhaps not ideal, but it would be difficult to implement otherwise.
        self.assertEqual(gc.collect_region(r), 4)
        # Nothing should have been dissolved.
        self.assertEqual(gc.collect(), 0)

    def test_finalizer(self):
        r = Region()
        r.data = {"counter": 0, "instance": None}
        r.a = self.build_cycle()
        r.a.resurrectable = self.Resurrectable(r.data)
        r.a = None

        # The cycle should be collected
        self.assertEqual(gc.collect_region(r), 2)
        # The finalizer should have run exactly once
        self.assertEqual(r.data["counter"], 1)
        # The instance should not have been collected
        self.assertIs(r.data["instance"].data, r.data)
        # The finalizer should not run again
        r.data["instance"] = None
        self.assertEqual(r.data["counter"], 1)

    def test_release_while_gc(self):
        r = Region()
        cown = Cown(r)
        r.releaser = self.CownReleaser(cown)
        r.releaser = None
        r = None

        # Releasing a garbage collected cown should fail
        with test.support.catch_unraisable_exception() as cm:
            gc.collect_region(cown)
            self.assertIs(cm.unraisable.exc_type, RuntimeError)

    def test_collection_triggered(self):
        gc.enable()
        cown = self.build_detector_cown()
        # Assuming that the budget was increased sufficiently.
        cown.release()
        cown.acquire()

        # The cown should have been collected on release
        self.assertEqual(cown.value.data["counter"], 1)

    def test_region_left_open(self):
        gc.enable()
        r = Region()
        cown = Cown(r)
        # Using InterpreterLocal to create a local reference
        r.iplocal = InterpreterLocal(None)
        r.opener = self.RegionOpener(r.iplocal)
        r.opener = None
        r = None

        with test.support.catch_unraisable_exception() as cm:
            # Assuming that the budget was increased sufficiently.
            cown.release()
            cown.acquire()
            # The cown could not have been released.
            # That should have triggered an unraisable exception
            # and replaced the cown value with None to indicate an error.
            self.assertIs(cm.unraisable.exc_type, RuntimeError)
            self.assertIs(cown.value, None)


def setUpModule():
    global enabled, debug, threshold
    enabled = gc.isenabled()
    debug = gc.get_debug()
    threshold = gc.get_threshold()
    gc.disable()
    gc.set_debug(debug & ~gc.DEBUG_LEAK)
    gc.set_threshold(20, 10, 10)


def tearDownModule():
    gc.set_debug(debug)
    gc.enable() if enabled else gc.disable()
    gc.set_threshold(*threshold)

if __name__ == "__main__":
    unittest.main()
