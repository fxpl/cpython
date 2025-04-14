import io

from . import BaseNotFreezableTest


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
        handle = open('test_file.txt', 'w')
        super().__init__(*args, obj=handle, **kwargs)

    def tearDown(self):
        self.obj.close()


class RawWrapperTest(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        handle = open('test_file.txt', 'wb')
        super().__init__(*args, obj=handle, **kwargs)

    def tearDown(self):
        self.obj.close()
