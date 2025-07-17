import unittest
from regions import Region
from immutable import freeze

class TestInterRegionRelations(unittest.TestCase):
    class A:
        pass

    class B:
        def __init__(self, x):
            self.x = x

    def setUp(self):
        # Allows the types to be referenced from multiple regions
        freeze(self.A)
        freeze(self.B)

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

    def test_call_init_lrc(self):
        r = Region()
        r.a = self.A()

        lrc = r._lrc
        b = self.B(r.a)
        self.assertEqual(r._lrc, lrc + 1)

    def test_call_init_lrc_and_take_ownership(self):
        r = Region()
        r.a = self.A()

        lrc = r._lrc
        r.b = self.B(r.a)
        self.assertEqual(r._lrc, lrc)
