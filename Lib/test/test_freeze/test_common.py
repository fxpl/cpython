import unittest
from immutable import deep_freeze, is_deep_frozen


class BaseObjectTest(unittest.TestCase):
    def __init__(self, *args, obj=None, **kwargs):
        unittest.TestCase.__init__(self, *args, **kwargs)
        self.obj = obj

    def setUp(self):
        # Explicitly freeze type, then the object
        # Types are not implicitly frozen by deep_freeze()
        # deep_freeze(type(self.obj))
        deep_freeze(self.obj)

    def test_immutable(self):
        self.assertTrue(is_deep_frozen(self.obj))

    def test_add_attribute(self):
        with self.assertRaises(TypeError):
            self.obj.new_attribute = 'value'

    def test_type_immutable(self):
        self.assertTrue(is_deep_frozen(self.obj))
        self.assertTrue(is_deep_frozen(type(self.obj)), "Type should be frozen when instance is frozen: {}".format(type(self.obj)))


class BaseNotFreezableTest(unittest.TestCase):
    def __init__(self, *args, obj=None, **kwargs):
        unittest.TestCase.__init__(self, *args, **kwargs)
        self.obj = obj

    def check_not_freezable(self, obj):
        self.assertIsNotNone(obj)

        with self.assertRaises(TypeError):
            deep_freeze(obj)

        self.assertFalse(is_deep_frozen(obj))


if __name__ == '__main__':
    unittest.main()
