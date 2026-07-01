import math
import unittest
from regions import Region, is_local
from immutable import freeze


class MyInt(int):
    """int subclass whose instances are mutable and therefore region-movable.

    Primed with ``freeze(MyInt())`` in setUpClass so the class object is frozen
    up front and the first live instance can be moved into a region without the
    move trying to freeze the type underneath it.
    """
    pass


# A value large enough (> 2 digits) that binary/bitwise ops take the general
# multi-digit path in longobject.c rather than the compact fast path.
BIG = 10 ** 40


class TestRegionLong(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        freeze(MyInt())

    def _region_with(self, value):
        """Return (r, x) where x is a MyInt(value) owned by region r."""
        r = Region()
        x = MyInt(value)
        r.x = x
        return r, x

    # ---------------------------------------------------------------------- #
    # Group A — Transfer / ownership                                          #
    # ---------------------------------------------------------------------- #

    def test_subclass_instance_transfers_into_region(self):
        """A mutable int subclass instance is moved into the region (not frozen)."""
        r = Region()
        x = MyInt(BIG)
        self.assertTrue(is_local(x))

        r.x = x

        self.assertTrue(r.owns(x))
        self.assertFalse(is_local(x))

    # ---------------------------------------------------------------------- #
    # Group B — pow() (long_pow, long_invmod)                                 #
    # ---------------------------------------------------------------------- #

    def test_pow_two_arg_small_exponent(self):
        """x ** 2 borrows the region-owned base; LRC returns to baseline."""
        r, x = self._region_with(BIG)
        base = r._lrc

        result = x ** 2

        self.assertEqual(int(result), BIG ** 2)
        self.assertEqual(r._lrc, base)

    def test_pow_two_arg_large_exponent(self):
        """A large exponent drives the windowed/binary path (table[0] borrow)."""
        r, x = self._region_with(BIG)
        base = r._lrc

        result = x ** 137

        self.assertEqual(int(result), BIG ** 137)
        self.assertEqual(r._lrc, base)

    def test_pow_three_arg_modular(self):
        """3-arg pow reduces the region-owned base by the modulus."""
        r, x = self._region_with(BIG)
        base = r._lrc

        result = pow(x, 65537, 1000003)

        self.assertEqual(int(result), pow(BIG, 65537, 1000003))
        self.assertEqual(r._lrc, base)

    def test_pow_modular_inverse(self):
        """pow(x, -1, m) routes through long_invmod with a region-owned base."""
        r, x = self._region_with(3)
        base = r._lrc

        result = pow(x, -1, 7)

        self.assertEqual(int(result), pow(3, -1, 7))  # == 5
        self.assertEqual(r._lrc, base)

    def test_pow_negative_exponent_returns_float(self):
        """Negative exponent without modulus takes the early float fallback."""
        r, x = self._region_with(2)
        base = r._lrc

        result = x ** -1

        self.assertEqual(result, 0.5)
        self.assertEqual(r._lrc, base)

    def test_pow_result_reference_is_local(self):
        """The pow() result is a fresh local int, not owned by the base's region."""
        r, x = self._region_with(BIG)
        result = x ** 2
        self.assertTrue(is_local(result))
        self.assertFalse(r.owns(result))

    # ---------------------------------------------------------------------- #
    # Group C — bitwise (long_bitwise)                                        #
    # ---------------------------------------------------------------------- #

    def _check_binop_lrc_neutral(self, op, expected):
        r = Region()
        x = MyInt(BIG)
        y = MyInt(BIG + 12345)
        r.x = x
        r.y = y
        base = r._lrc

        result = op(x, y)

        self.assertEqual(int(result), expected)
        self.assertEqual(r._lrc, base)

    def test_and_operands_in_region(self):
        """x & y borrows both region-owned operands; LRC returns to baseline."""
        self._check_binop_lrc_neutral(lambda a, b: a & b, BIG & (BIG + 12345))

    def test_or_operands_in_region(self):
        self._check_binop_lrc_neutral(lambda a, b: a | b, BIG | (BIG + 12345))

    def test_xor_operands_in_region(self):
        self._check_binop_lrc_neutral(lambda a, b: a ^ b, BIG ^ (BIG + 12345))

    def test_bitwise_self_operand(self):
        """x & x exercises the a == b aliasing without unbalancing the LRC."""
        r, x = self._region_with(BIG)
        base = r._lrc

        result = x & x

        self.assertEqual(int(result), BIG)
        self.assertEqual(r._lrc, base)

    # ---------------------------------------------------------------------- #
    # Group D — Neutral arithmetic (results + LRC neutrality)                 #
    # ---------------------------------------------------------------------- #

    def test_base_int_arithmetic_lrc_neutral(self):
        """Exact ints are frozen on entry; arithmetic on them is LRC-neutral."""
        r = Region()
        r.n = BIG                      # frozen (shallow-immutable), not contained
        base = r._lrc

        self.assertEqual(r.n // 7, BIG // 7)
        self.assertEqual(r.n % 7, BIG % 7)
        self.assertEqual(r.n / 2, BIG / 2)
        self.assertEqual(divmod(r.n, 7), divmod(BIG, 7))
        self.assertEqual(str(r.n), str(BIG))
        self.assertEqual(repr(r.n), repr(BIG))
        self.assertEqual(math.gcd(r.n, 7 * BIG), math.gcd(BIG, 7 * BIG))
        self.assertEqual(r.n.to_bytes(20, "little"),
                         BIG.to_bytes(20, "little"))

        self.assertEqual(r._lrc, base)

    def test_subclass_divmod_floordiv_mod_in_region(self):
        """divmod/// / % on a region-owned subclass base stay LRC-neutral."""
        r, x = self._region_with(BIG)
        base = r._lrc

        self.assertEqual(int(x // 7), BIG // 7)
        self.assertEqual(int(x % 7), BIG % 7)
        self.assertEqual(divmod(x, 7)[0], BIG // 7)
        self.assertEqual(divmod(x, 7)[1], BIG % 7)

        self.assertEqual(r._lrc, base)


if __name__ == "__main__":
    unittest.main()
