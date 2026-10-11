"""An invalid ``__freezable__`` value must not make an object freezable."""
import unittest
from immutable import deep_freeze, shallow_freeze, is_deep_frozen, is_shallow_frozen


class C:
    def __init__(self):
        self.x = 1


class TestFreezableValidation(unittest.TestCase):
    def test_out_of_range_instance_attr_rejected(self):
        o = C()
        o.__freezable__ = 42  # bypasses set_freezable validation
        with self.assertRaises(TypeError):
            shallow_freeze(o)
        self.assertFalse(is_shallow_frozen(o))

    def test_negative_instance_attr_rejected(self):
        o = C()
        o.__freezable__ = -5
        with self.assertRaises(TypeError):
            deep_freeze(o)
        self.assertFalse(is_deep_frozen(o))


if __name__ == "__main__":
    unittest.main()
