import unittest
from immutable import freeze, isfrozen, NotFreezable


class BaseObjectTest(unittest.TestCase):
    def __init__(self, *args, obj=None, **kwargs):
        unittest.TestCase.__init__(self, *args, **kwargs)
        self.obj = obj

    def setUp(self):
        # Explicitly freeze type, then the object
        # Types are not implicitly frozen by freeze()
        # freeze(type(self.obj))
        freeze(self.obj)

    def test_immutable(self):
        self.assertTrue(isfrozen(self.obj))

    def test_add_attribute(self):
        with self.assertRaises(TypeError):
            self.obj.new_attribute = 'value'

    def test_type_immutable(self):
        self.assertTrue(isfrozen(self.obj))
        self.assertTrue(isfrozen(type(self.obj)), "Type should be frozen when instance is frozen: {}".format(type(self.obj)))


class BaseNotFreezableTest(unittest.TestCase):
    def __init__(self, *args, obj=NotFreezable(), **kwargs):
        unittest.TestCase.__init__(self, *args, **kwargs)
        self.obj = obj

    def test_not_freezable(self):
        with self.assertRaises(TypeError):
            freeze(self.obj)

        self.assertFalse(isfrozen(self.obj))


if __name__ == '__main__':
    unittest.main()
