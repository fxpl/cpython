import unittest

from regions import Region, Cown
import gc

class TestRegionGC(unittest.TestCase):
    class A:
        pass

    def setUp(self):
        gc.collect()  # Ensure there are no lingering cycles

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
        self.assertGreaterEqual(gc.collect(), 2)

        # A cycle inside a region should be ignored
        r.a = self.build_cycle()
        r.a = None
        self.assertEqual(gc.collect(), 0)

        # Dissolving a region should allow cycles to be collected again
        r = None
        self.assertGreaterEqual(gc.collect(), 2)

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

    # FIXME(regions-gc)
    @unittest.skip("finalizers currently do not work")
    def test_finalizer(self):
        class Finalizable:
            def __init__(self, data):
                self.data = data

            def __del__(self):
                self.data["counter"] += 1
                self.data["instance"] = self

        r = Region()
        r.data = {"counter": 0, "instance": None}
        r.a = self.build_cycle()
        r.a.f = Finalizable(r.data)
        r.a = None

        c = Cown(r)
        r = None
        c.release()
        # The cycle should be collected; the finalizer should run exactly once
        self.assertEqual(gc.collect_region(c), 2)
        self.assertEqual(r.data["counter"], 1)
        # The finalizer should not run again
        r.data["instance"] = None
        self.assertEqual(r.data["counter"], 1)

    # TODO(regions-gc): test that region GC is triggered, but not when disabled
    # TODO(regions-gc): callbacks
    # TODO(regions-gc): weakref


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
