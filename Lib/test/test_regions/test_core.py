import unittest
from regions import Region, is_local
from immutable import freeze, isfrozen

class TestBasicRegionObject(unittest.TestCase):
    def test_region_construction(self):
        r = Region()

        # A region should own itself
        self.assertTrue(r.owns(r))

        # A region should own its dict
        self.assertTrue(r.owns(r.__dict__))

class ImplicitFreezingForImmortal(unittest.TestCase):
    def test_implicit_freeze_importal(self):
        # This would ideally check that the immortal objects
        # are unfrozen, before we add them to a region. However,
        # this would create an ordering dependency between tests.
        # So here we just check that they're frozen after the fact.
        r = Region()

        r.true = True
        self.assertFalse(r.owns(r.true))
        self.assertTrue(isfrozen(r.true))
        self.assertEqual(r.true, True)

        r.num = 12
        self.assertFalse(r.owns(r.num))
        self.assertTrue(isfrozen(r.num))
        self.assertEqual(r.num, 12)

        r.none = None
        self.assertFalse(r.owns(r.none))
        self.assertTrue(isfrozen(r.none))
        self.assertEqual(r.none, None)

class TestOwnership(unittest.TestCase):
    class A:
        pass

    def setUp(self):
        # Allows the A type to be referenced from multiple regions
        freeze(self.A)

    def test_local_not_owned(self):
        # Create a region
        r = Region()

        # Create a new local object
        a = self.A()

        self.assertTrue(is_local(a))
        self.assertFalse(r.owns(a))

    def test_region_takes_ownership_of_local(self):
        # Create a region
        r = Region()

        # Create a new local object
        a = self.A()
        self.assertTrue(is_local(a))

        # Move a into r
        r.a = a
        self.assertTrue(r.owns(a))
        self.assertFalse(is_local(a))

    def test_region_takes_ownership_of_local_is_deep(self):
        # Create a region
        r = Region()

        # Create a new local object
        a = self.A()
        a.b = self.A()
        self.assertTrue(is_local(a))
        self.assertTrue(is_local(a.b))

        # Move a into r
        r.a = a
        self.assertTrue(r.owns(a))
        self.assertTrue(r.owns(a.b))
        self.assertFalse(is_local(a))
        self.assertFalse(is_local(a.b))

class TestInterRegionRelations(unittest.TestCase):
    class A:
        pass

    def setUp(self):
        # Allows the A type to be referenced from multiple regions
        freeze(self.A)

    def test_reference_to_contained(self):
        r1 = Region()
        r2 = Region()
        a = self.A()

        # Move a into r1
        r1.a = a
        self.assertTrue(r1.owns(a))
        self.assertFalse(r2.owns(a))

        # Check the exception on assignment
        with self.assertRaises(RuntimeError) as e:
            r2.a = a
        self.assertEqual(e.exception.source, r2.__dict__)
        self.assertEqual(e.exception.target, a)

        # Check ownership is unchanged
        self.assertTrue(r1.owns(a))
        self.assertFalse(r2.owns(a))

    def test_unchanged_region_after_failure(self):
        r1 = Region()
        r2 = Region()
        a = self.A()
        a.b = self.A()
        a.b.c = self.A()

        # Move a.b.c into r1
        r1.c = a.b.c
        self.assertTrue(is_local(a))
        self.assertTrue(is_local(a.b))
        self.assertTrue(r1.owns(a.b.c))

        # Moving a into r2 will fail due to a.b.c being in a different region
        with self.assertRaises(RuntimeError) as e:
            r2.a = a
        self.assertEqual(e.exception.source, a.b)
        self.assertEqual(e.exception.target, a.b.c)

        # Object a and b should remain local
        self.assertTrue(is_local(a))
        self.assertTrue(is_local(a.b))
