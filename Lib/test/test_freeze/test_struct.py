import unittest
from struct import Struct

from .test_common import BaseObjectTest


class TestStruct(BaseObjectTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=Struct("i"), **kwargs)

    def test_init(self):
        with self.assertRaisesRegex(TypeError, "is immutable"):
            self.obj.__init__("q")
        self.assertEqual(self.obj.format, "i")


if __name__ == "__main__":
    unittest.main()
