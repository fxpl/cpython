import operator
import unittest

import _testinternalcapi

from immutable import freeze
from regions import Region, is_local


class SetElement:
    pass


class SetEqObj:
    __slots__ = ()

    def __hash__(self):
        return 42

    def __eq__(self, other):
        return isinstance(other, SetEqObj)


class SetBadEq:
    __slots__ = ()

    def __hash__(self):
        return 42

    def __eq__(self, other):
        raise ValueError("comparison failed")


class SetRecordRepr:
    def __init__(self, region, seen_lrc):
        self.region = region
        self.seen_lrc = seen_lrc

    def __repr__(self):
        self.seen_lrc.append(self.region._lrc)
        return "RecordRepr"


class TestRegionSet(unittest.TestCase):
    A = SetElement
    EqObj = SetEqObj
    BadEq = SetBadEq
    RecordRepr = SetRecordRepr

    def setUp(self):
        freeze(type(self).A)
        freeze(type(self).EqObj)
        freeze(type(self).BadEq)
        freeze(type(self).RecordRepr)

    def region_set(self):
        r = Region()
        r.s = set()
        return r, r.s

    def add_region_element(self, r, s):
        obj = self.A()
        s.add(obj)
        self.assertTrue(r.owns(obj))
        obj = None

    def test_add_transfers_local_element(self):
        r, s = self.region_set()
        obj = self.A()

        self.assertTrue(is_local(obj))
        s.add(obj)

        self.assertTrue(r.owns(obj))
        self.assertFalse(is_local(obj))

    def test_update_and_constructor_transfer_local_elements(self):
        r, s = self.region_set()
        x = self.A()
        y = self.A()

        s.update([x])
        self.assertTrue(r.owns(x))

        r.t = set([y])
        self.assertTrue(r.owns(y))

    def test_update_from_local_set_with_same_region_key_is_lrc_neutral(self):
        r, s = self.region_set()
        r.value = self.A()
        other = {r.value}
        base_lrc = r._lrc

        s.update(other)

        self.assertTrue(r.owns(r.value))
        self.assertIn(r.value, s)
        self.assertEqual(r._lrc, base_lrc)

    def test_failed_bulk_update_keeps_local_key_local(self):
        r1, s = self.region_set()
        r2 = Region()
        r2.obj = self.A()
        local = self.A()
        other = {local, r2.obj}
        base_lrc = r1._lrc

        with self.assertRaises(RuntimeError):
            s.update(other)

        self.assertEqual(len(s), 0)
        self.assertTrue(is_local(local))
        self.assertEqual(r1._lrc, base_lrc)

    def test_init_replaces_contents_and_transfers_local_element(self):
        r, s = self.region_set()
        old = self.A()
        new = self.A()
        s.add(old)
        old = None
        base_lrc = r._lrc

        result = s.__init__([new])

        self.assertIsNone(result)
        self.assertTrue(r.owns(new))
        self.assertEqual(len(s), 1)
        self.assertIn(new, s)
        self.assertEqual(r._lrc, base_lrc + 1)
        new = None
        self.assertEqual(r._lrc, base_lrc)

    def test_add_failure_leaves_set_unchanged(self):
        r1, s = self.region_set()
        r2 = Region()
        r2.obj = self.A()
        base_lrc = r1._lrc

        with self.assertRaises(RuntimeError):
            s.add(r2.obj)

        self.assertEqual(len(s), 0)
        self.assertEqual(r1._lrc, base_lrc)
        self.assertTrue(r2.owns(r2.obj))

    def test_pop_returns_borrow_until_result_released(self):
        r, s = self.region_set()
        self.add_region_element(r, s)
        base_lrc = r._lrc

        obj = s.pop()
        self.assertEqual(r._lrc, base_lrc + 1)

        obj = None
        self.assertEqual(r._lrc, base_lrc)

    def test_iterator_creation_yield_exhaustion_and_abandonment(self):
        r, s = self.region_set()
        self.add_region_element(r, s)
        base_lrc = r._lrc

        it = iter(s)
        self.assertEqual(r._lrc, base_lrc + 1)
        operator.length_hint(it)
        self.assertEqual(r._lrc, base_lrc + 1)
        obj = next(it)
        self.assertEqual(r._lrc, base_lrc + 2)
        obj = None
        self.assertEqual(r._lrc, base_lrc + 1)
        with self.assertRaises(StopIteration):
            next(it)
        self.assertEqual(r._lrc, base_lrc)

        it = iter(s)
        self.assertEqual(r._lrc, base_lrc + 1)
        it = None
        self.assertEqual(r._lrc, base_lrc)

    def test_copy_union_difference_and_intersection_hold_element_borrows(self):
        r, s = self.region_set()
        self.add_region_element(r, s)
        base_lrc = r._lrc

        for build in (
            lambda: s.copy(),
            lambda: s.union(set()),
            lambda: s | set(),
            lambda: s.difference(set()),
            lambda: s - set(),
            lambda: s.intersection(s),
            lambda: s & s,
            lambda: s.symmetric_difference(set()),
            lambda: s ^ set(),
            lambda: s.difference({self.A(): None}),
        ):
            result = build()
            self.assertEqual(r._lrc, base_lrc + 1)
            result = None
            self.assertEqual(r._lrc, base_lrc)

        for build in (
            lambda: s.symmetric_difference(s),
            lambda: s ^ s,
        ):
            result = build()
            self.assertEqual(len(result), 0)
            self.assertEqual(r._lrc, base_lrc)
            result = None
            self.assertEqual(r._lrc, base_lrc)

    def test_inplace_operators_return_self_borrow(self):
        for op, other_factory in (
            (operator.ior, lambda s: set()),
            (operator.iand, lambda s: set()),
            (operator.isub, lambda s: set()),
            (operator.ixor, lambda s: set()),
        ):
            r, s = self.region_set()
            self.add_region_element(r, s)
            other = other_factory(s)
            base_lrc = r._lrc
            result = op(s, other)
            held_lrc = r._lrc
            self.assertIs(result, s)
            self.assertEqual(held_lrc, base_lrc + 1)
            result = None
            self.assertEqual(r._lrc, base_lrc)

    def test_update_methods_return_none_and_are_lrc_neutral(self):
        for update in (
            lambda s: s.difference_update(set()),
            lambda s: s.intersection_update(set()),
            lambda s: s.symmetric_difference_update(set()),
        ):
            r, s = self.region_set()
            self.add_region_element(r, s)
            base_lrc = r._lrc

            result = update(s)

            self.assertIsNone(result)
            self.assertEqual(r._lrc, base_lrc)

    def test_intersection_update_with_retained_local_temp_is_lrc_neutral(self):
        r, s = self.region_set()
        self.add_region_element(r, s)
        other = set(s)
        base_lrc = r._lrc

        result = s.intersection_update(other)

        self.assertIsNone(result)
        self.assertEqual(len(s), 1)
        self.assertEqual(r._lrc, base_lrc)

    def test_intersection_update_retained_local_equal_key_accounted(self):
        r, s = self.region_set()
        original = self.EqObj()
        s.add(original)
        original = None
        local = self.EqObj()
        other = {local}
        base_lrc = r._lrc

        result = s.intersection_update(other)

        self.assertIsNone(result)
        self.assertEqual(len(s), 1)
        self.assertGreaterEqual(r._lrc, base_lrc)
        other = None
        local = None
        self.assertEqual(r._lrc, base_lrc)

    def test_intersection_update_retained_bridge_element_reuses_barrier(self):
        r1, s = self.region_set()
        r2 = Region()
        s.add(r2)

        result = s.intersection_update(s)

        self.assertIsNone(result)
        self.assertEqual(s, {r2})

    def test_update_method_failures_leave_region_unchanged(self):
        r, s = self.region_set()
        s.add(self.BadEq())
        base_lrc = r._lrc
        base_len = len(s)

        with self.assertRaises(ValueError):
            s.difference_update({self.BadEq()})

        self.assertEqual(len(s), base_len)
        self.assertEqual(r._lrc, base_lrc)

        r1, s = self.region_set()
        r2 = Region()
        s.add(self.A())
        r2.obj = self.A()
        base_lrc = r1._lrc
        base_len = len(s)

        with self.assertRaises(RuntimeError):
            s.symmetric_difference_update({r2.obj})

        self.assertEqual(len(s), base_len)
        self.assertEqual(r1._lrc, base_lrc)
        self.assertTrue(r2.owns(r2.obj))

    def test_difference_update_large_other_path_is_lrc_neutral(self):
        r, s = self.region_set()
        obj = self.A()
        s.add(obj)
        obj = None
        large_other = set(s)
        for _ in range(9):
            large_other.add(self.A())
        base_lrc = r._lrc

        result = s.difference_update(large_other)

        self.assertIsNone(result)
        self.assertEqual(len(s), 0)
        self.assertEqual(r._lrc, base_lrc)
        large_other = None

    def test_dict_fromkeys_set_fast_path_accounts_region_value(self):
        r = Region()
        r.value = self.A()
        base_lrc = r._lrc

        d = dict.fromkeys({1, 2}, r.value)

        self.assertEqual(r._lrc, base_lrc + 2)
        d = None
        self.assertEqual(r._lrc, base_lrc)

    def test_dict_fromkeys_set_fast_path_existing_region_dict_value(self):
        r = Region()
        r.d = {}
        r.value = self.A()

        class ReturningDict(dict):
            def __new__(cls):
                return r.d

        base_lrc = r._lrc

        d = ReturningDict.fromkeys({1, 2}, r.value)

        self.assertIs(d, r.d)
        self.assertEqual(d, {1: r.value, 2: r.value})
        self.assertEqual(r._lrc, base_lrc + 1)
        d = None
        self.assertEqual(r._lrc, base_lrc)

    def test_dict_fromkeys_set_fast_path_identical_existing_value(self):
        r = Region()
        r.d = {}
        r.value = self.A()
        r.d[1] = r.value

        class ReturningDict(dict):
            def __new__(cls):
                return r.d

        base_lrc = r._lrc

        d = ReturningDict.fromkeys({1}, r.value)

        self.assertIs(d, r.d)
        self.assertEqual(d, {1: r.value})
        self.assertEqual(r._lrc, base_lrc + 1)
        d = None
        self.assertEqual(r._lrc, base_lrc)

    def test_testinternalcapi_set_next_entry_accounts_return_tuple(self):
        r, s = self.region_set()
        self.add_region_element(r, s)
        base_lrc = r._lrc

        entry = _testinternalcapi.set_next_entry(s, 0)

        self.assertEqual(entry[0], 1)
        self.assertEqual(r._lrc, base_lrc + 1)
        entry = None
        self.assertEqual(r._lrc, base_lrc)

    def test_contains_comparisons_and_predicates_are_lrc_neutral(self):
        r, s = self.region_set()
        self.add_region_element(r, s)
        obj = next(iter(s))
        base_lrc = r._lrc

        self.assertIn(obj, s)
        self.assertFalse(s.isdisjoint({obj}))
        self.assertFalse(s.isdisjoint(s))
        self.assertTrue(s.issubset(set(s)))
        self.assertTrue(s.issuperset(set(s)))
        self.assertTrue(s == set(s))
        self.assertFalse(s != set(s))
        self.assertEqual(r._lrc, base_lrc)

        obj = None
        self.assertEqual(r._lrc, base_lrc - 1)

    def test_remove_discard_clear_are_lrc_neutral_for_region_set(self):
        r, s = self.region_set()
        obj = self.A()
        s.add(obj)
        obj = None
        base_lrc = r._lrc

        obj = next(iter(s))
        s.discard(obj)
        obj = None
        self.assertEqual(r._lrc, base_lrc)

        obj = self.A()
        s.add(obj)
        obj = None
        obj = next(iter(s))
        s.remove(obj)
        obj = None
        self.assertEqual(r._lrc, base_lrc)

        s.add(self.A())
        s.clear()
        self.assertEqual(len(s), 0)
        self.assertEqual(r._lrc, base_lrc)

    def test_repr_reduce_and_sizeof_are_lrc_neutral(self):
        r, s = self.region_set()
        seen_lrc = []

        obj = self.RecordRepr(r, seen_lrc)
        s.add(obj)
        obj = None
        base_lrc = r._lrc

        repr(s)
        self.assertEqual(r._lrc, base_lrc)
        self.assertGreater(max(seen_lrc), base_lrc)

        reduced = s.__reduce__()
        self.assertEqual(r._lrc, base_lrc + 1)
        reduced = None
        self.assertEqual(r._lrc, base_lrc)

        s.__sizeof__()
        self.assertEqual(r._lrc, base_lrc)

    def test_iterator_reduce_is_lrc_neutral_after_result_release(self):
        r, s = self.region_set()
        self.add_region_element(r, s)
        base_lrc = r._lrc

        it = iter(s)
        iter_lrc = r._lrc
        reduced = it.__reduce__()
        self.assertGreaterEqual(r._lrc, iter_lrc)

        reduced = None
        it = None
        self.assertEqual(r._lrc, base_lrc)

    def test_frozenset_idempotent_constructor_and_copy_return_borrow(self):
        r = Region()
        obj = self.A()
        r.f = frozenset([obj])
        self.assertTrue(r.owns(obj))
        obj = None
        f = r.f
        base_lrc = r._lrc

        same = frozenset(f)
        self.assertIs(same, f)
        self.assertEqual(r._lrc, base_lrc + 1)
        same = None
        self.assertEqual(r._lrc, base_lrc)

        same = f.copy()
        self.assertIs(same, f)
        self.assertEqual(r._lrc, base_lrc + 1)
        same = None
        self.assertEqual(r._lrc, base_lrc)

    def test_frozenset_operators_reduce_and_contains(self):
        r = Region()
        obj = self.A()
        r.f = frozenset([obj])
        self.assertTrue(r.owns(obj))
        obj = None
        f = r.f
        base_lrc = r._lrc

        for build in (
            lambda: f | frozenset(),
            lambda: f.union(frozenset()),
            lambda: f - frozenset(),
            lambda: f.difference(frozenset()),
            lambda: f & f,
            lambda: f.intersection(f),
            lambda: f ^ frozenset(),
            lambda: f.symmetric_difference(frozenset()),
        ):
            result = build()
            self.assertEqual(r._lrc, base_lrc + 1)
            result = None
            self.assertEqual(r._lrc, base_lrc)

        reduced = f.__reduce__()
        self.assertEqual(r._lrc, base_lrc + 1)
        reduced = None
        self.assertEqual(r._lrc, base_lrc)

        hash(f)
        f.__sizeof__()
        self.assertTrue(f.isdisjoint(frozenset()))
        self.assertFalse(f.isdisjoint(f))
        self.assertTrue(f.issubset(f))
        self.assertTrue(f.issuperset(f))
        self.assertEqual(r._lrc, base_lrc)

        r.g = frozenset([self.EqObj()])
        g = r.g
        probe = self.EqObj()
        contains_base_lrc = r._lrc
        self.assertIn(probe, g)
        self.assertEqual(r._lrc, contains_base_lrc)

    def test_intersection_update_failure_leaves_region_unchanged(self):
        r1, s = self.region_set()
        r2 = Region()
        a = self.EqObj()
        b = self.EqObj()
        s.add(a)
        r2.b = b
        a = None
        b = None
        base_lrc = r1._lrc

        with self.assertRaises(RuntimeError):
            s.intersection_update({r2.b})

        self.assertEqual(len(s), 1)
        self.assertEqual(r1._lrc, base_lrc)
        self.assertFalse(r1.owns(r2.b))


if __name__ == "__main__":
    unittest.main()
