import io

from .test_common import BaseNotFreezableTest


class BytesIOTest(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=io.BytesIO(), **kwargs)

    def tearDown(self):
        self.obj.close()


class StringIOTest(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=io.StringIO(), **kwargs)

    def tearDown(self):
        self.obj.close()


class TextWrapperTest(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        handle = open(__file__, 'r')
        super().__init__(*args, obj=handle, **kwargs)

    def tearDown(self):
        self.obj.close()


class RawWrapperTest(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        handle = open(__file__, 'rb')
        super().__init__(*args, obj=handle, **kwargs)

    def tearDown(self):
        self.obj.close()
