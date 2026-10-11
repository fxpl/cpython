# TODO(Immutable): This test currently causes an ABC Meta to become frozen
# which breaks the test harness.
# Perhaps the ABC meta data cache should be converted to thread local.

# import decimal

# from .test_common import BaseObjectTest

# class TestContext(BaseObjectTest):
#     def __init__(self, *args, **kwargs):
#         super().__init__(*args, obj=decimal.Context(), **kwargs)

#     def test_prec(self):
#         with self.assertRaises(TypeError):
#             self.obj.prec = 10

#     def test_emax(self):
#         with self.assertRaises(TypeError):
#             self.obj.Emax = 10

#     def test_emin(self):
#         with self.assertRaises(TypeError):
#             self.obj.Emin = -10

#     def test_rounding(self):
#         with self.assertRaises(TypeError):
#             self.obj.rounding = decimal.ROUND_DOWN

#     def test_capitals(self):
#         with self.assertRaises(TypeError):
#             self.obj.capitals = 0

#     def test_clamp(self):
#         with self.assertRaises(TypeError):
#             self.obj.clamp = 1


import textwrap
import unittest
from test.support import import_helper
from test.support.script_helper import assert_python_ok

# The frozen-context semantics are only implemented in the C module.
import_helper.import_module('_decimal')


class TestFrozenContext(unittest.TestCase):
    # Run in a subprocess: freezing a Context freezes the MutableMapping ABC.
    def test_frozen_context_not_mutated(self):
        assert_python_ok("-c", textwrap.dedent("""
            import decimal
            from decimal import Context, Decimal, DivisionByZero, Inexact
            from immutable import deep_freeze

            c = Context(traps=[DivisionByZero])
            deep_freeze(c)

            # Flags are not recorded on a frozen context.
            c.divide(Decimal(1), Decimal(3))
            assert not c.flags[Inexact]
            c.create_decimal(1.5)
            assert not any(c.flags.values())

            # Traps are still raised.
            try:
                c.divide(Decimal(1), Decimal(0))
            except DivisionByZero:
                pass
            else:
                raise AssertionError("trap not raised")
            assert not any(c.flags.values())

            # The current context is a mutable copy.
            decimal.setcontext(c)
            cur = decimal.getcontext()
            assert cur is not c
            assert cur.traps[DivisionByZero]
            Decimal(1) / Decimal(3)
            assert cur.flags[Inexact]
            assert not c.flags[Inexact]
        """))


if __name__ == "__main__":
    unittest.main()
