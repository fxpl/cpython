import unittest
from immutable import freeze, isfrozen


class TestImplicitImmutability(unittest.TestCase):
    """Tests for objects that can be viewed as immutable."""

    def test_tuple_of_immortal_ints(self):
        """A tuple of small ints (immortal) can be viewed as immutable."""
        obj = (1, 2, 3)
        self.assertTrue(isfrozen(obj))

    def test_tuple_of_strings(self):
        """A tuple of interned strings can be viewed as immutable."""
        obj = ("hello", "world")
        self.assertTrue(isfrozen(obj))

    def test_tuple_of_none(self):
        """A tuple containing None can be viewed as immutable."""
        obj = (None, None)
        self.assertTrue(isfrozen(obj))

    def test_nested_tuples(self):
        """Nested tuples of immortal objects can be viewed as immutable."""
        obj = ((1, 2), (3, (4, 5)))
        self.assertTrue(isfrozen(obj))

    def test_tuple_with_mutable_list(self):
        """A tuple containing a mutable list cannot be viewed as immutable."""
        obj = (1, [2, 3])
        self.assertFalse(isfrozen(obj))

    def test_tuple_with_mutable_dict(self):
        """A tuple containing a mutable dict cannot be viewed as immutable."""
        obj = (1, {"a": 2})
        self.assertFalse(isfrozen(obj))

    def test_frozenset_of_ints(self):
        """A frozenset of ints can be viewed as immutable."""
        obj = frozenset([1, 2, 3])
        self.assertTrue(isfrozen(obj))

    def test_empty_tuple(self):
        """An empty tuple can be viewed as immutable."""
        obj = ()
        self.assertTrue(isfrozen(obj))

    def test_empty_frozenset(self):
        """An empty frozenset can be viewed as immutable."""
        obj = frozenset()
        self.assertTrue(isfrozen(obj))

    def test_already_frozen_object(self):
        """An already-frozen object should return True."""
        obj = [1, 2, 3]
        freeze(obj)
        self.assertTrue(isfrozen(obj))

    def test_tuple_containing_frozen_object(self):
        """A tuple containing a frozen list can be viewed as immutable."""
        inner = [1, 2, 3]
        freeze(inner)
        obj = (inner, 4, 5)
        self.assertTrue(isfrozen(obj))

    def test_mutable_list(self):
        """A plain mutable list cannot be viewed as immutable."""
        obj = [1, 2, 3]
        self.assertFalse(isfrozen(obj))

    def test_mutable_dict(self):
        """A plain mutable dict cannot be viewed as immutable."""
        obj = {"a": 1}
        self.assertFalse(isfrozen(obj))

    def test_tuple_with_bytes(self):
        """A tuple containing bytes can be viewed as immutable."""
        obj = (b"hello", b"world")
        self.assertTrue(isfrozen(obj))

    def test_tuple_with_float(self):
        """A tuple with floats can be viewed as immutable."""
        obj = (1.0, 2.5, 3.14)
        self.assertTrue(isfrozen(obj))

    def test_tuple_with_complex(self):
        """A tuple with complex numbers can be viewed as immutable."""
        obj = (1+2j, 3+4j)
        self.assertTrue(isfrozen(obj))

    def test_tuple_with_bool(self):
        """A tuple with booleans can be viewed as immutable."""
        obj = (True, False)
        self.assertTrue(isfrozen(obj))

    def test_isfrozen_freezes_non_immortal(self):
        """A graph that can be viewed as immutable gets frozen by isfrozen."""
        big = 10**100
        obj = (big,)
        result = isfrozen(obj)
        self.assertTrue(result)
        self.assertTrue(isfrozen(obj))

    def test_tuple_of_range(self):
        """A tuple containing a range object can be viewed as immutable."""
        obj = (range(10),)
        self.assertTrue(isfrozen(obj))

    def test_deeply_nested(self):
        """Deeply nested tuples can be viewed as immutable."""
        obj = (1,)
        for _ in range(100):
            obj = (obj,)
        self.assertTrue(isfrozen(obj))

    def test_tuple_with_set(self):
        """A tuple containing a mutable set cannot be viewed as immutable."""
        obj = (1, {2, 3})
        self.assertFalse(isfrozen(obj))

    def test_c_shallow_immutable_type(self):
        """A C type registered as shallow immutable can be viewed as immutable."""
        import _test_reachable
        freeze(_test_reachable.ShallowImmutable)
        obj = (_test_reachable.ShallowImmutable(1),)
        self.assertTrue(isfrozen(obj))

    def test_c_shallow_immutable_with_mutable_referent(self):
        """A C shallow immutable containing a mutable object cannot be viewed as immutable."""
        import _test_reachable
        freeze(_test_reachable.ShallowImmutable)
        obj = (_test_reachable.ShallowImmutable([1, 2]),)
        self.assertFalse(isfrozen(obj))

    def test_deeply_nested_no_stack_overflow(self):
        """Very deep nesting should not cause a stack overflow."""
        obj = (1,)
        for _ in range(10000):
            obj = (obj,)
        self.assertTrue(isfrozen(obj))


if __name__ == '__main__':
    unittest.main()
