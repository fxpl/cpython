import os
from test.support import load_package_tests
import unittest


def load_tests(*args):
    return load_package_tests(os.path.dirname(__file__), *args)


class BaseObjectTest(unittest.TestCase):
    def __init__(self, *args, obj=None, **kwargs):
        unittest.TestCase.__init__(self, *args, **kwargs)
        self.obj = obj

    def setUp(self):
        freeze(self.obj)

    def test_immutable(self):
        self.assertTrue(isimmutable(self.obj))

    def test_add_attribute(self):
        with self.assertRaises(NotWritableError):
            self.obj.new_attribute = 'value'

    def test_type_immutable(self):
        self.assertTrue(isimmutable(type(self.obj)))


class BaseNotFreezableTest(unittest.TestCase):
    def __init__(self, *args, obj=notfreezable(), **kwargs):
        unittest.TestCase.__init__(self, *args, **kwargs)
        self.obj = obj

    def test_isinstance(self):
        self.assertTrue(isinstance(self.obj, notfreezable))

    def test_not_freezable(self):
        with self.assertRaises(TypeError):
            freeze(self.obj)
