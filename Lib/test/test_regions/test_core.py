from unittest import expectedFailure
import sys
import unittest
from regions import Region, is_local, is_owned, get_region
from immutable import freeze, is_frozen, freezable, unfreezable

class TestBasicRegionObject(unittest.TestCase):
    def test_region_construction(self):
        r = Region()

        # The region should be open since r points into it
        self.assertTrue(r.is_open)

        # A region should own itself
        self.assertTrue(r.owns(r))

        # The region should be open since r points into it
        self.assertTrue(r.is_open)

        # A new region should be clean
        self.assertFalse(r.is_dirty)

        # A new region has no parent
        self.assertIsNone(r.parent)

        # A new region should have not subregions
        self.assertListEqual(r._subregions, [])

    def test_fields_read_only(self):
        r = Region()

        # Check the exception on assignment
        with self.assertRaises(AttributeError):
            r.is_open = False

        with self.assertRaises(AttributeError):
            r.is_dirty = False

        with self.assertRaises(AttributeError):
            r.parent = None

        with self.assertRaises(AttributeError):
            r._lrc = None

        with self.assertRaises(AttributeError):
            r._osc = None

        with self.assertRaises(AttributeError):
            r._subregions = None

    def test_instance_dict_is_owned(self):
        r = Region()

        # instance dictionaries are NULL until required
        self.assertIsNone(r.__dict__)

        # Adding a field will instantiate the dict
        r.field = "Please init dict"

        # The instance attribute should be initialised and owned
        self.assertTrue(r.owns(r.__dict__))

    def test_instance_attribute_is_lrc_neutral(self):
        r = Region()
        self.assertEqual(r._lrc, 1)

        r.field = {}
        self.assertEqual(r._lrc, 1)


class TestRegionCounts(unittest.TestCase):
    def test_osc_1(self):
        r = Region()
        r.sub = Region()

        # Pre-condition
        self.assertEqual(r._osc, 0, "The sub region should be closed")

        # This should open the sub-region
        sub = r.sub

        # Post-condition
        self.assertEqual(r._osc, 1, "The sub region should be open now")


class ImplicitFreezingForImmortal(unittest.TestCase):
    def test_implicit_freeze_importal(self):
        # This would ideally check that the immortal objects
        # are unfrozen, before we add them to a region. However,
        # this would create an ordering dependency between tests.
        # So here we just check that they're frozen after the fact.
        r = Region()

        r.true = True
        self.assertFalse(r.owns(r.true))
        self.assertTrue(is_frozen(r.true))
        self.assertEqual(r.true, True)

        r.num = 12
        self.assertFalse(r.owns(r.num))
        self.assertTrue(is_frozen(r.num))
        self.assertEqual(r.num, 12)

        r.none = None
        self.assertFalse(r.owns(r.none))
        self.assertTrue(is_frozen(r.none))
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
        self.assertFalse(is_owned(a))
        self.assertEqual(get_region(a), None)

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
        self.assertTrue(is_owned(a))
        self.assertEqual(get_region(a), r)

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

    def test_get_region_lrc(self):
        # Create a region
        r = Region()
        a = self.A()
        r.a = a

        # a is owned by r
        self.assertTrue(r.owns(a))

        base_lrc = r._lrc
        self.assertEqual(get_region(a), r)
        self.assertEqual(get_region(r), r)
        self.assertEqual(get_region(a), r)
        self.assertEqual(get_region(r), r)
        self.assertEqual(r._lrc, base_lrc)

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

    def test_get_parent(self):
        r1 = Region()
        r2 = Region()

        # Make r2 a child of r1
        r1.r2 = r2

        # Check that r2 knows ab out this
        self.assertEqual(r2.parent, r1)

        # Unparent r2 again
        r1.r2 = None

        # Check that r2 has no parent
        self.assertIsNone(r2.parent)

    def test_subregions(self):
        r1 = Region()
        r2 = Region()
        r3 = Region()
        r4 = Region()

        # r1 starts with no children
        self.assertEqual(len(r1._subregions), 0)

        # This should add r2 as a subregion
        r1.r2 = r2
        self.assertEqual(len(r1._subregions), 1)
        self.assertIn(r2, r1._subregions)

        # This should add r3 as a subregion
        r1.r3 = r3
        self.assertEqual(len(r1._subregions), 2)
        self.assertIn(r2, r1._subregions)
        self.assertIn(r3, r1._subregions)

        # This should replace r3 as a subregion
        r1.r3 = r4
        self.assertEqual(len(r1._subregions), 2)
        self.assertIn(r2, r1._subregions)
        self.assertIn(r4, r1._subregions)
        self.assertNotIn(r3, r1._subregions)

        # The subregions list should be temporary and clear the LRC after
        self.assertEqual(r2._lrc, 1)
        self.assertEqual(r3._lrc, 1)
        self.assertEqual(r4._lrc, 1)

    def test_region_dissolve_bumps_subregion_lrc(self):
        r1 = Region()
        r2 = Region()
        obj = self.A()

        # Make r2 a subregion of 1
        r1.obj = obj
        obj.r2 = r2

        # Precondition
        self.assertEqual(r2.parent, r1)
        r2_lrc = r2._lrc

        # Dissolve parent region
        r1 = None

        # Postcondition
        self.assertEqual(r2._lrc, r2_lrc + 1)
        self.assertIsNone(r2.parent)
        self.assertTrue(is_local(obj))

    def test_regression_dealloc_needs_the_region_1(self):
        r = Region()
        r.a = self.A()
        r.a.child = Region()
        r.a = None
        r = None

    def test_regression_dealloc_needs_the_region_2(self):
        r = Region()
        r.a = self.A()
        r.a.child = Region()
        r.a.child.b = self.A()
        r.a.child.b.c = self.A()
        r.a.child.b.c.b = r.a.child.b
        r.a = None

class testMovability(unittest.TestCase):
    def test_types_freeze(self):
        r = Region()

        @freezable
        class A: pass

        self.assertFalse(is_frozen(A), "A should be mutable before the move")
        r.a = A()
        self.assertTrue(is_frozen(A), "A should be frozen after the move")

    def test_unfreezable_type(self):
        r = Region()

        @unfreezable
        class A: pass

        self.assertFalse(is_frozen(A), "A should be mutable before the move")

        # Moving a will attempt to freeze A and should fail
        with self.assertRaises(TypeError):
            r.a = A()

    def test_view_as_immutable(self):
        r = Region()
        base_lrc = r._lrc

        obj = (123456789, 987654321, "strings are cool")
        ref1 = obj
        ref2 = obj
        ref3 = obj
        # This would increase the LRC, if the object isn't implicitly frozen
        r.shallow_imm = obj
        self.assertEqual(r._lrc, base_lrc)

    def test_module_objs(self):
        # Make sure the module will be freshly imported
        sys.modules.pop("random", None)
        sys.mut_modules.pop("random", None)

        # Import random
        import random

        self.assertFalse(is_frozen(random), "`random` should be mutable before the move")
        r = Region()
        r.a = random
        self.assertTrue(is_frozen(random), "`random` should be frozen after the move")

        # Unimport the module
        sys.modules.pop("random", None)
        sys.mut_modules.pop("random", None)
