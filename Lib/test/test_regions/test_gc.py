import unittest

from regions import Cown, Region
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

        # The cycle should be collected
        self.assertEqual(gc.collect_region(r), 2)
        # The finalizer should have run exactly once
        self.assertEqual(r.data["counter"], 1)
        # The instance should not have been collected
        self.assertIs(r.data["instance"].data, r.data)
        # The finalizer should not run again
        r.data["instance"] = None
        self.assertEqual(r.data["counter"], 1)

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
