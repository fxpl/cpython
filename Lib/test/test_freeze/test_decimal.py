import decimal

from . import BaseObjectTest


class TestContext(BaseObjectTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=decimal.Context(), **kwargs)

    def test_prec(self):
        with self.assertRaises(NotWritableError):
            self.obj.prec = 10

    def test_emax(self):
        with self.assertRaises(NotWritableError):
            self.obj.Emax = 10

    def test_emin(self):
        with self.assertRaises(NotWritableError):
            self.obj.Emin = -10

    def test_rounding(self):
        with self.assertRaises(NotWritableError):
            self.obj.rounding = decimal.ROUND_DOWN

    def test_capitals(self):
        with self.assertRaises(NotWritableError):
            self.obj.capitals = 0

    def test_clamp(self):
        with self.assertRaises(NotWritableError):
            self.obj.clamp = 1
