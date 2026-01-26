import unittest
from regions import Region
from immutable import freeze
import gc

class TestOwnership(unittest.TestCase):
    class A:
        pass

    def build_cycle(self):
        freeze(self.A)
        a = self.A()
        a.b = self.A()
        a.b.a = a
        return a

    def test_owned_cycles_are_ignored(self):
        r = Region()

        # Make sure that there are no lingering cycles
        gc.collect()

        # A normal cycle should be collected
        self.build_cycle()
        self.assertGreaterEqual(gc.collect(), 2)

        # A cycle inside a region should be ignored
        r.c = self.build_cycle()
        r.c = None
        self.assertEqual(gc.collect(), 0)

        # Dissolving a region should allow cycles to be collected again
        r = None
        self.assertGreaterEqual(gc.collect(), 2)
