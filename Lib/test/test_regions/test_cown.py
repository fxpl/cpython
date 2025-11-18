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

        # Local arguments are forbidden
        with self.assertRaises(RuntimeError) as e:
            Cown(x)

