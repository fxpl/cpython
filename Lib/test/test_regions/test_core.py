import unittest
from regions import Region
from immutable import freeze, isfrozen

class TestBasicRegionObject(unittest.TestCase):
    def test_region_construction(self):
        r = Region()

        # A region should own itself
        self.assertTrue(r.owns_object(r))

        # A region should own its dict
        self.assertTrue(r.owns_object(r.__dict__))
