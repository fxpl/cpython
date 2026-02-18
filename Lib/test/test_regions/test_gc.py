import unittest

from regions import Region, Cown
import gc

class TestRegionGC(unittest.TestCase):
    class A:
        pass

    def setUp(self):
        # Need to run collection multiple times to clean up region chains
        while gc.collect() > 0:
            pass

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

        c = Cown(r)
        r = None
        c.release()
        # The cycle inside the region should be collected
        self.assertEqual(gc.collect_region(c), 2)

    def test_collect_cycle_with_backlink(self):
        r = Region()
        r.a = self.build_cycle()
        r.a.r = r
        r.a = None

        c = Cown(r)
        r = None
        c.release()
        # The cycle inside the region should be collected
        self.assertEqual(gc.collect_region(c), 2)

    def test_collect_child_region(self):
        r = Region()
        r.child = self.build_region_with_unreachable_cycle()

        c = Cown(r)
        r = None
        c.release()
        # The cycle inside the child region should be collected
        self.assertEqual(gc.collect_region(c), 2)

    def test_collect_unreachable_child_region(self):
        r = Region()
        r.a = self.build_cycle()
        r.a.child = self.build_region_with_unreachable_cycle()
        r.a = None

        c = Cown(r)
        r = None
        c.release()
        # The cycle inside the parent region should be collected,
        # and the child region should be dissolved into the local region,
        # allowing the cycle inside it to be collected by the local GC.
        # Note that the bridge object is never counted;
        # perhaps not ideal, but it would be difficult to implement otherwise.
        self.assertEqual(gc.collect_region(c), 2)
        self.assertEqual(gc.collect(), 2)

    def test_finalizer(self):
        class Resurrectable:
            def __init__(self, data):
                self.data = data

            def __del__(self):
                self.data["counter"] += 1
                self.data["instance"] = self

        r = Region()
        r.data = {"counter": 0, "instance": None}
        r.a = self.build_cycle()
        r.a.f = Resurrectable(r.data)
        r.a = None

        c = Cown(r)
        r = None
        c.release()
        # The cycle should be collected
        self.assertEqual(gc.collect_region(c), 2)
        c.acquire()
        r = c.value
        # The finalizer should have run exactly once
        self.assertEqual(r.data["counter"], 1)
        # The instance should not have been collected
        self.assertIs(r.data["instance"].data, r.data)
        # The finalizer should not run again
        r.data["instance"] = None
        self.assertEqual(r.data["counter"], 1)

    def test_region_opened_by_finalizer(self):
        class RegionOpener:
            def __init__(self, r):
                self.r = r

            def __del__(self):
                # Create a cycle; it outlives the finalizer
                a = {}
                a["a"] = a
                # Open the region
                a["r"] = self.r

        r = Region()
        r.a = self.build_cycle()
        r.a.f = RegionOpener(r)
        r.a = None

        c = Cown(r)
        r = None
        c.release()
        # Collection should be aborted
        self.assertEqual(gc.collect_region(c), 0)
        c.acquire()
        # The region should have been replaced with None
        self.assertIsNone(c.value)

    def test_cown_changed_by_finalizer(self):
        class CownChanger:
            def __init__(self, c):
                self.c = c

            def __del__(self):
                # Change the cown's region
                self.c.value = Region()

        r = Region()
        c = Cown(r)
        r.a = self.build_cycle()
        r.a.f = CownChanger(c)
        r.a = None
        r = None
        c.release()
        # Collection should be aborted
        self.assertEqual(gc.collect_region(c), 0)


    # TODO(regions-gc): test that region GC is triggered, but not when disabled
    # TODO(regions-gc): GC callbacks
    # TODO(regions-gc): weakrefs


def setUpModule():
    global enabled, debug
    enabled = gc.isenabled()
    debug = gc.get_debug()
    gc.disable()
    gc.set_debug(debug & ~gc.DEBUG_LEAK)


def tearDownModule():
    gc.set_debug(debug)
    gc.enable() if enabled else gc.disable()


if __name__ == "__main__":
    unittest.main()
