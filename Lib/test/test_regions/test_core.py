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
    
    def test_field_assignments(self):
        # TODO
        pass

class ImplicitFreezingForImmortal(unittest.TestCase):
    def test_implicit_freeze_importal(self):
        # This would ideally check that the immortal objects
        # are unfrozen, before we add them to a region. However,
        # this would create an ordering dependency between tests.
        # So here we just check that they're frozen after the fact.
        r = Region()

        r.true = True
        self.assertFalse(r.owns_object(r.true))
        self.assertTrue(isfrozen(r.true))
        self.assertEqual(r.true, True)

        r.num = 12
        self.assertFalse(r.owns_object(r.num))
        self.assertTrue(isfrozen(r.num))
        self.assertEqual(r.num, 12)

        r.none = None
        self.assertFalse(r.owns_object(r.none))
        self.assertTrue(isfrozen(r.none))
        self.assertEqual(r.none, None)

class TestInterRegionRelations(unittest.TestCase):
    # TODO
    pass