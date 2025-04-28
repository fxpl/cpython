import unittest
from test.support import import_helper
import types

xxlimited = import_helper.import_module('xxlimited')
xxlimited_35 = import_helper.import_module('xxlimited_35')

class CommonTests:
    module: types.ModuleType

    def test_xxo_set_attribute(self):
        xxo = self.module.Xxo()

        freeze(xxo)

        with self.assertRaises(TypeError):
            xxo.foo = 1234

    def test_xxo_del_attribute(self):
        xxo = self.module.Xxo()

        freeze(xxo)

        with self.assertRaises(TypeError):
            del xxo.foo


class TestXXLimited(CommonTests, unittest.TestCase):
    module = xxlimited

    def test_buffer(self):
        xxo = self.module.Xxo()

        freeze(xxo)

        # Creating a buffer into immutable memory should be fine.
        b1 = memoryview(xxo)
        del b1

class TestXXLimited35(CommonTests, unittest.TestCase):
    module = xxlimited_35
