import zlib

from .test_common import BaseNotFreezableTest

class ZlibCompressTest(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=zlib.compressobj(), **kwargs)

class ZlibDecompressTest(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=zlib.decompressobj(), **kwargs)

class ZlibDecompressorTest(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=zlib._ZlibDecompressor(), **kwargs)
