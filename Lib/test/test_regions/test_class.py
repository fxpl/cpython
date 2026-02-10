import unittest
from regions import Region, is_local
from immutable import freeze, isfrozen

class TestInterRegionRelations(unittest.TestCase):
    class A:
        pass

    def setUp(self):
        # Allows the A type to be referenced from multiple regions
        freeze(self.A)

    def test_regression_instance_attribute_wb(self):
        r = Region()
        r.a = self.A()
        r.a.child = Region()
        r.a.child = None

    def test_attr_attr_lrc(self):
        r = Region()
        r.a = self.A()
        r.a.b = self.A()

        lrc = r._lrc
        r.a.b.a = r.a
        self.assertEqual(r._lrc, lrc)

