import csv
from io import BytesIO

from .test_common import BaseNotFreezableTest


class TestCSVReader(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=csv.reader([]), **kwargs)


class TestCSVWriter(BaseNotFreezableTest):
    def __init__(self, *args, **kwargs):
        self.buffer = BytesIO()
        super().__init__(*args, obj=csv.writer(self.buffer), **kwargs)
