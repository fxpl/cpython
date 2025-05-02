from test.support import import_helper
import unittest

from .test_common import BaseNotFreezableTest

bz2 = import_helper.import_module('bz2')
from bz2 import BZ2Compressor, BZ2Decompressor


class TestBZ2Compressor(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=BZ2Compressor(), **kwargs)


class TestBZ2Decompressor(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=BZ2Decompressor(), **kwargs)


if __name__ == '__main__':
    unittest.main()
