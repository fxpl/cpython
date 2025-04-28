import os
from test.support import load_package_tests
import unittest
from immutable import freeze, isfrozen, NotFreezable


def load_tests(*args):
    return load_package_tests(os.path.dirname(__file__), *args)


class BaseObjectTest(unittest.TestCase):
    def __init__(self, *args, obj=None, **kwargs):
        unittest.TestCase.__init__(self, *args, **kwargs)
        self.obj = obj

    def setUp(self):
        freeze(self.obj)

    def test_immutable(self):
        self.assertTrue(isfrozen(self.obj))

    def test_add_attribute(self):
        with self.assertRaises(TypeError):
            self.obj.new_attribute = 'value'

    def test_type_immutable(self):
        self.assertTrue(isfrozen(type(self.obj)))


class BaseNotFreezableTest(unittest.TestCase):
    def __init__(self, *args, obj=NotFreezable(), **kwargs):
        unittest.TestCase.__init__(self, *args, **kwargs)
        self.obj = obj

    def test_not_freezable(self):
        with self.assertRaises(TypeError):
            freeze(self.obj)
