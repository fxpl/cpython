from bz2 import BZ2Compressor, BZ2Decompressor

from . import BaseNotFreezableTest


class TestBZ2Compressor(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=BZ2Compressor(), **kwargs)


class TestBZ2Decompressor(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=BZ2Decompressor(), **kwargs)
