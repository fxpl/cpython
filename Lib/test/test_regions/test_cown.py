import unittest
from regions import Cown, Region, is_local
from immutable import freeze

class TestBasicCownObject(unittest.TestCase):
    def test_valid_cown_construction(self):
        # No argument, is a good argument
        c = Cown()
        self.assertIsNone(c.value)

        # Immortal builtin object
        c = Cown(None)
        self.assertIsNone(c.value)

        # Frozen object
        x = {}
        freeze(x)
        c = Cown(x)
        self.assertEqual(c.value, x)

        # Cowns are allowed
        c = Cown()
        c1 = Cown(c)
        c2 = Cown(c)

        # Regions are allowed
        r = Region(name="dummy")
        Cown(r)
        Cown(Region(name="new region"))

    def test_cown_construction_error_for_local(self):
        x = {}
        self.assertTrue(is_local(x))

        # Local arguments are forbidden
        with self.assertRaises(RuntimeError) as e:
            Cown(x)

    def test_cown_construction_error_for_owned(self):
        r = Region()
        x = {}
        r.x = x
        self.assertTrue(r.owns(x))

        # Owned arguments are forbidden
        with self.assertRaises(RuntimeError) as e:
            Cown(x)

class TestCownValueField(unittest.TestCase):
    def test_cown_valid_value_fields(self):
        # No argument, is a good argument
        c = Cown()
        self.assertIsNone(c.value)

        # Immortal builtin object
        c.value = None
        self.assertIsNone(c.value)

        # Frozen object
        x = {}
        freeze(x)
        c.value = x
        self.assertEqual(c.value, x)

        # Cowns are allowed
        c1 = Cown(c)
        c.value = c1
        self.assertEqual(c.value, c1)

        # Regions are allowed
        r = Region(name="dummy")
        c.value = r
        self.assertEqual(c.value, r)

    def test_cown_value_error_for_local(self):
        x = {}
        self.assertTrue(is_local(x))
        c = Cown()

        # Local values are forbidden
        with self.assertRaises(RuntimeError) as e:
            c.value = x

    def test_cown_construction_error_for_owned(self):
        r = Region()
        x = {}
        r.x = x
        self.assertTrue(r.owns(x))

        c = Cown()
        # Owned values are forbidden
        with self.assertRaises(RuntimeError) as e:
            c.value = x

