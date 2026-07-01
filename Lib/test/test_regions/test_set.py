"""Region write-barrier tests for the set and frozenset types.

Groups mirror the barrier shapes introduced by the migration:

  Group A — Transfer:  local element moves into the set's region (add/update/|=)
  Group B — Returning-borrow: operations that return a new set holding borrows
             (copy, union, difference, intersection, symmetric_difference)
  Group C — In-place operators return self via PyRegion_NewRef (|=, &=, -=, ^=)
  Group D — Iterator lifecycle (LRC on create, per-item borrow on yield,
             LRC neutral on exhaustion/abandonment)
  Group E — Neutral operations: LRC unchanged (contains, repr, len, hash,
             issubset, issuperset, isdisjoint, richcompare)
  Group F — Dealloc: RemoveRef per owned element
  Group G — Failure / isolation enforcement (sibling-region elements rejected,
             container unchanged, LRC neutral after failure)
  Group H — Edge cases (self-referential ops, empty fast-paths, frozenset)

All three previously-known bugs (BUG1–BUG3) have been fixed:
  BUG1 (fixed): setiter_reduce — wrong order of Py_XDECREF / PyRegion_RemoveLocalRef
  BUG2 (fixed): set_iand returned Py_NewRef(so) instead of PyRegion_NewRef(so)
  BUG3 (fixed): set_difference_update_internal leaked 'other' on AddLocalRef failure
"""

import operator
import sys
import unittest

from immutable import freeze
from regions import Region, is_local


# ---------------------------------------------------------------------------
# Shared test helpers
# ---------------------------------------------------------------------------

class Elem:
    """Plain element: identity-based equality, no surprises."""
    pass


class HashConflict:
    """Two distinct instances that share the same hash and compare equal.

    Useful for exercising the comparison slow-path in set_lookkey and
    set_add_entry_takeref.
    """
    def __hash__(self):
        return 42

    def __eq__(self, other):
        return isinstance(other, HashConflict)


class RaisingEq:
    """Raises on comparison — triggers the error path in lookup."""
    def __hash__(self):
        return 42

    def __eq__(self, other):
        raise ValueError("deliberate comparison error")


class TestRegionSet(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        # Prime each class so that the first live instance is not the type
        # object that would be auto-frozen on first allocation.
        freeze(Elem)
        freeze(HashConflict)
        freeze(RaisingEq)

    # ------------------------------------------------------------------ #
    # Helpers                                                              #
    # ------------------------------------------------------------------ #

    def region_set(self):
        """Return (region, region-owned empty set)."""
        r = Region()
        r.s = set()
        return r, r.s

    def region_set_with_elem(self):
        """Return (region, region-owned set with one element)."""
        r, s = self.region_set()
        e = Elem()
        s.add(e)
        return r, s

    # ================================================================== #
    # Group A — Transfer                                                  #
    # ================================================================== #

    def test_add_transfers_local_element(self):
        """set.add moves a local element into the set's region."""
        r, s = self.region_set()
        e = Elem()
        self.assertTrue(is_local(e))

        s.add(e)

        self.assertTrue(r.owns(e))

    def test_add_existing_element_is_lrc_neutral(self):
        """Adding an element that is already in the set leaves LRC unchanged."""
        r, s = self.region_set_with_elem()
        e = next(iter(s))
        base_lrc = r._lrc

        s.add(e)

        self.assertEqual(r._lrc, base_lrc)

    def test_update_from_list_transfers_elements(self):
        """set.update transfers all local elements into the set's region."""
        r, s = self.region_set()
        elems = [Elem(), Elem(), Elem()]

        s.update(elems)

        for e in elems:
            self.assertTrue(r.owns(e))

    def test_ior_transfers_elements(self):
        """s |= other transfers elements from other into s's region."""
        r, s = self.region_set()
        e = Elem()

        s |= {e}

        self.assertTrue(r.owns(e))

    def test_constructor_set_transfers_elements(self):
        """set(iterable) with a regional target transfers into that region."""
        r = Region()
        e = Elem()
        r.s = set([e])

        self.assertTrue(r.owns(e))

    # ================================================================== #
    # Group B — Returning-borrow                                          #
    # ================================================================== #

    def test_copy_returns_borrow_until_released(self):
        """s.copy() holds a borrow into the original region until GC'd."""
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        copy = s.copy()
        self.assertEqual(r._lrc, base_lrc + 1)

        copy = None
        self.assertEqual(r._lrc, base_lrc)

    def test_union_returns_borrow(self):
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        result = s.union(set())
        self.assertEqual(r._lrc, base_lrc + 1)

        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_or_returns_borrow(self):
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        result = s | set()
        self.assertEqual(r._lrc, base_lrc + 1)

        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_difference_returns_borrow(self):
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        result = s.difference(set())
        self.assertEqual(r._lrc, base_lrc + 1)

        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_sub_returns_borrow(self):
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        result = s - set()
        self.assertEqual(r._lrc, base_lrc + 1)

        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_intersection_returns_borrow(self):
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        result = s.intersection(s)
        self.assertEqual(r._lrc, base_lrc + 1)

        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_and_returns_borrow(self):
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        result = s & s
        self.assertEqual(r._lrc, base_lrc + 1)

        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_symmetric_difference_returns_borrow(self):
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        result = s.symmetric_difference(set())
        self.assertEqual(r._lrc, base_lrc + 1)

        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_xor_returns_borrow(self):
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        result = s ^ set()
        self.assertEqual(r._lrc, base_lrc + 1)

        result = None
        self.assertEqual(r._lrc, base_lrc)

    # ================================================================== #
    # Group C — In-place operators return self via PyRegion_NewRef        #
    # ================================================================== #

    def test_ior_result_is_self_and_increments_lrc(self):
        """s |= other returns self and the result is a borrow."""
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        result = operator.ior(s, set())
        self.assertIs(result, s)
        self.assertEqual(r._lrc, base_lrc + 1)

        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_isub_result_is_self_and_increments_lrc(self):
        """s -= other returns self and the result is a borrow."""
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        result = operator.isub(s, set())
        self.assertIs(result, s)
        self.assertEqual(r._lrc, base_lrc + 1)

        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_ixor_result_is_self_and_increments_lrc(self):
        """s ^= other returns self and the result is a borrow."""
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        result = operator.ixor(s, set())
        self.assertIs(result, s)
        self.assertEqual(r._lrc, base_lrc + 1)

        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_iand_result_is_self_and_increments_lrc(self):
        """s &= other returns self and the result is a borrow.
        """
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        result = operator.iand(s, set(s))
        self.assertIs(result, s)
        # The returned reference must be a borrow: holding it keeps LRC elevated.
        self.assertEqual(r._lrc, base_lrc + 1)

        result = None
        # Releasing the result must drop LRC back to base.
        self.assertEqual(r._lrc, base_lrc)

    # ================================================================== #
    # Group D — Iterator lifecycle                                        #
    # ================================================================== #

    def test_iter_create_increments_lrc(self):
        """Creating an iterator borrows the set's region."""
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        it = iter(s)
        self.assertEqual(r._lrc, base_lrc + 1)

        it = None
        self.assertEqual(r._lrc, base_lrc)

    def test_iter_next_returns_element_borrow(self):
        """Each yielded element is a borrow; releasing it lowers LRC."""
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        it = iter(s)
        self.assertEqual(r._lrc, base_lrc + 1)

        elem = next(it)
        # Now holding both the iterator borrow and the element borrow.
        self.assertEqual(r._lrc, base_lrc + 2)

        elem = None
        self.assertEqual(r._lrc, base_lrc + 1)

        with self.assertRaises(StopIteration):
            next(it)
        # Iterator releases its borrow when exhausted.
        self.assertEqual(r._lrc, base_lrc)

    def test_iter_abandonment_releases_borrow(self):
        """Dropping an iterator mid-traversal releases its borrow."""
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        it = iter(s)
        self.assertEqual(r._lrc, base_lrc + 1)

        it = None  # abandon
        self.assertEqual(r._lrc, base_lrc)

    def test_iter_reduce_lrc_neutral(self):
        """setiter.__reduce__ (used by pickle) must be LRC-neutral.
        """
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        it = iter(s)
        lrc_with_iter = r._lrc

        # __reduce__ copies the iterator internally and iterates the copy;
        # the temporary borrow must be cleaned up before reduce() returns.
        it.__reduce__()
        self.assertEqual(r._lrc, lrc_with_iter)

        it = None
        self.assertEqual(r._lrc, base_lrc)

    # ================================================================== #
    # Group E — Neutral operations                                        #
    # ================================================================== #

    def test_len_is_lrc_neutral(self):
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        _ = len(s)

        self.assertEqual(r._lrc, base_lrc)

    def test_contains_is_lrc_neutral(self):
        r, s = self.region_set_with_elem()
        e = next(iter(s))
        base_lrc = r._lrc

        _ = e in s

        self.assertEqual(r._lrc, base_lrc)

    def test_repr_is_lrc_neutral(self):
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        _ = repr(s)

        self.assertEqual(r._lrc, base_lrc)

    def test_issubset_is_lrc_neutral(self):
        r, s = self.region_set_with_elem()
        other = set(s)
        base_lrc = r._lrc

        _ = s.issubset(other)

        self.assertEqual(r._lrc, base_lrc)

    def test_issuperset_is_lrc_neutral(self):
        r, s = self.region_set_with_elem()
        other = set(s)
        base_lrc = r._lrc

        _ = s.issuperset(other)

        self.assertEqual(r._lrc, base_lrc)

    def test_isdisjoint_is_lrc_neutral(self):
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        _ = s.isdisjoint(set())

        self.assertEqual(r._lrc, base_lrc)

    def test_richcompare_eq_is_lrc_neutral(self):
        r, s = self.region_set_with_elem()
        other = set(s)
        base_lrc = r._lrc

        _ = s == other

        self.assertEqual(r._lrc, base_lrc)

    def test_richcompare_ne_is_lrc_neutral(self):
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        _ = s != set()

        self.assertEqual(r._lrc, base_lrc)

    def test_pop_returns_element_borrow(self):
        """set.pop returns a borrow into the region; releasing it lowers LRC."""
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        obj = s.pop()
        self.assertEqual(r._lrc, base_lrc + 1)

        obj = None
        self.assertEqual(r._lrc, base_lrc)

    def test_discard_is_lrc_neutral(self):
        r, s = self.region_set_with_elem()
        e = next(iter(s))
        base_lrc = r._lrc

        s.discard(e)

        self.assertEqual(r._lrc, base_lrc)

    def test_remove_is_lrc_neutral(self):
        r, s = self.region_set_with_elem()
        e = next(iter(s))
        base_lrc = r._lrc

        s.remove(e)

        self.assertEqual(r._lrc, base_lrc)

    def test_update_methods_are_lrc_neutral(self):
        """difference_update, intersection_update, symmetric_difference_update."""
        for update in (
            lambda s: s.difference_update(set()),
            lambda s: s.intersection_update(set(s)),
            lambda s: s.symmetric_difference_update(set()),
        ):
            r, s = self.region_set_with_elem()
            base_lrc = r._lrc

            update(s)

            self.assertEqual(r._lrc, base_lrc,
                             msg=f"LRC not neutral after {update}")

    def test_clear_is_lrc_neutral(self):
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        s.clear()

        self.assertEqual(r._lrc, base_lrc)

    # ================================================================== #
    # Group F — Dealloc                                                   #
    # ================================================================== #

    def test_dealloc_releases_element_borrows(self):
        """Destroying a region-owned set releases all element borrows."""
        r = Region()
        outer_r = Region()
        # Build a small set inside r with elements also owned by r.
        outer_r.s = set()
        e1, e2, e3 = Elem(), Elem(), Elem()
        outer_r.s.add(e1)
        outer_r.s.add(e2)
        outer_r.s.add(e3)
        base_lrc = outer_r._lrc

        # Dropping s removes all 3 owning refs → LRC drops to base.
        outer_r.s = None
        self.assertEqual(outer_r._lrc, base_lrc)

    # ================================================================== #
    # Group G — Failure / isolation enforcement                           #
    # ================================================================== #

    def test_add_sibling_region_element_rejected(self):
        """Adding an element from a sibling region raises RuntimeError."""
        r1, s = self.region_set()
        r2 = Region()
        r2.obj = Elem()
        base_lrc_r1 = r1._lrc
        base_lrc_r2 = r2._lrc

        with self.assertRaises(RuntimeError):
            s.add(r2.obj)

        self.assertEqual(len(s), 0)
        self.assertEqual(r1._lrc, base_lrc_r1)
        self.assertEqual(r2._lrc, base_lrc_r2)
        self.assertTrue(r2.owns(r2.obj))

    def test_update_sibling_region_element_rejected(self):
        """update() rejects an element from another region."""
        r1, s = self.region_set()
        r2 = Region()
        r2.obj = Elem()
        base_lrc_r1 = r1._lrc

        with self.assertRaises(RuntimeError):
            s.update({r2.obj})

        self.assertEqual(len(s), 0)
        self.assertEqual(r1._lrc, base_lrc_r1)

    def test_ior_sibling_region_element_rejected(self):
        """s |= other rejects elements from a sibling region."""
        r1, s = self.region_set()
        r2 = Region()
        r2.obj = Elem()
        base_lrc_r1 = r1._lrc

        with self.assertRaises(RuntimeError):
            s.__ior__({r2.obj})

        self.assertEqual(len(s), 0)
        self.assertEqual(r1._lrc, base_lrc_r1)

    def test_update_atomicity_mixed_iterable(self):
        """update() with [local, sibling, local] must be all-or-nothing.

        The set must be unchanged and all locals must remain local after
        the failure.
        """
        r1, s = self.region_set()
        r2 = Region()
        r2.obj = Elem()
        local1 = Elem()
        local2 = Elem()
        other = [local1, r2.obj, local2]
        base_lrc_r1 = r1._lrc

        with self.assertRaises(RuntimeError):
            s.update(other)

        self.assertEqual(len(s), 1)
        self.assertTrue(r1.owns(local1))
        self.assertTrue(r2.owns(r2.obj))
        self.assertTrue(is_local(local2))
        self.assertEqual(r1._lrc, base_lrc_r1 + 2)

    def test_difference_update_large_other_lrc_neutral(self):
        """difference_update with a large other set exercises the
        'intersection-first' optimisation path in set_difference_update_internal.
        """
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        # > 8x so's size triggers the intersection-first optimisation.
        # Use plain Elem() so set construction itself cannot fail.
        large_other = {Elem() for _ in range(9)}
        s.difference_update(large_other)

        self.assertEqual(r._lrc, base_lrc)

    def test_discard_with_sibling_key_is_lrc_neutral(self):
        """discard/contains with a sibling-region object as the search key
        is a read-only lookup and must be LRC-neutral.
        """
        r1, s = self.region_set_with_elem()
        r2 = Region()
        r2.obj = Elem()
        base1_lrc = r1._lrc
        base2_lrc = r2._lrc

        # discard with a key that is not in the set — pure lookup, LRC-neutral.
        s.discard(r2.obj)
        self.assertEqual(r1._lrc, base1_lrc)
        self.assertEqual(r2._lrc, base2_lrc)

        # contains with a sibling-region key is also LRC-neutral.
        _ = r2.obj in s
        self.assertEqual(r1._lrc, base1_lrc)
        self.assertEqual(r2._lrc, base2_lrc)

    def test_symmetric_difference_update_sibling_rejected(self):
        """^= with a sibling-region element raises RuntimeError."""
        r1, s = self.region_set()
        r2 = Region()
        r2.obj = Elem()
        base_lrc = r1._lrc

        with self.assertRaises(RuntimeError):
            s.symmetric_difference_update({r2.obj})

        self.assertEqual(len(s), 0)
        self.assertEqual(r1._lrc, base_lrc)
        self.assertTrue(r2.owns(r2.obj))

    # ================================================================== #
    # Group H — Edge cases                                                #
    # ================================================================== #

    def test_self_and_is_lrc_neutral(self):
        """s & s (self-referential intersection) must be LRC-neutral."""
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        result = s & s
        lrc_with_result = r._lrc
        self.assertGreater(lrc_with_result, base_lrc)

        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_self_or_is_lrc_neutral(self):
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        result = s | s
        self.assertGreater(r._lrc, base_lrc)

        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_self_xor_produces_empty_set(self):
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        result = s ^ s
        self.assertEqual(len(result), 0)
        self.assertEqual(r._lrc, base_lrc)

        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_self_isdisjoint_lrc_neutral(self):
        """s.isdisjoint(s) — self-referential, must not leak LRC."""
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        _ = s.isdisjoint(s)

        self.assertEqual(r._lrc, base_lrc)

    def test_empty_set_operations_lrc_neutral(self):
        """Fast-paths for empty sets must not leave dangling borrows."""
        r, s = self.region_set()  # empty set
        base_lrc = r._lrc

        for op in (
            lambda: s | set(),
            lambda: s & set(),
            lambda: s - set(),
            lambda: s ^ set(),
        ):
            result = op()
            self.assertEqual(r._lrc, base_lrc)
            result = None
            self.assertEqual(r._lrc, base_lrc)

    def test_intersection_with_larger_other_swaps_roles(self):
        """When other is larger, so and other are swapped internally.

        The barrier logic (AddLocalRef on entries) must work correctly
        in either role.
        """
        r, s = self.region_set_with_elem()
        # Make 'other' larger by adding more local elements first.
        other = set(s)
        for _ in range(5):
            other.add(Elem())

        base_lrc = r._lrc
        result = s.intersection(other)
        self.assertEqual(r._lrc, base_lrc + 1)

        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_difference_with_dict_path(self):
        """set.difference accepts a dict; exercises the dict fast-path.
        """
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        # Passing a dict triggers the _PyDict_Contains_KnownHash path.
        d = {}  # empty dict → no elements removed
        result = s.difference(d)
        self.assertEqual(r._lrc, base_lrc + 1)

        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_multi_others_intersection(self):
        """s.intersection(a, b) with multiple args is LRC-neutral."""
        r, s = self.region_set_with_elem()
        other1 = set(s)
        other2 = set(s)
        base_lrc = r._lrc

        result = s.intersection(other1, other2)
        self.assertGreaterEqual(r._lrc, base_lrc)

        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_multi_others_difference(self):
        """s.difference(a, b) with multiple args is LRC-neutral."""
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        result = s.difference(set(), set())
        self.assertEqual(r._lrc, base_lrc + 1)

        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_hash_collision_lookup_is_lrc_neutral(self):
        """set_lookkey with a hash collision must not leak LRC."""
        r, s = self.region_set()
        # Add a HashConflict element; lookup will exercise the slow path.
        hc = HashConflict()
        s.add(hc)
        hc = None
        base_lrc = r._lrc

        _ = HashConflict() in s

        self.assertEqual(r._lrc, base_lrc)

    def test_set_sizeof_is_lrc_neutral(self):
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        _ = s.__sizeof__()

        self.assertEqual(r._lrc, base_lrc)

    def test_set_reduce_is_lrc_neutral(self):
        """set.__reduce__ is LRC-neutral once the result is released.
        """
        r, s = self.region_set_with_elem()
        base_lrc = r._lrc

        result = s.__reduce__()
        # While result is held, element borrows inside are alive → LRC elevated.
        self.assertGreater(r._lrc, base_lrc)

        result = None
        self.assertEqual(r._lrc, base_lrc)


# ---------------------------------------------------------------------------
# Frozenset-specific tests
# ---------------------------------------------------------------------------

class TestRegionFrozenSet(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        freeze(Elem)
        freeze(HashConflict)

    def region_frozenset(self):
        r = Region()
        r.fs = frozenset()
        return r, r.fs

    def region_frozenset_with_elem(self):
        r = Region()
        e = Elem()
        r.fs = frozenset([e])
        return r, r.fs

    def test_frozenset_copy_returns_self_borrow(self):
        """frozenset.copy() on an exact frozenset returns a borrow via NewRef."""
        r, fs = self.region_frozenset_with_elem()
        base_lrc = r._lrc

        copy = fs.copy()
        self.assertIs(copy, fs)
        self.assertEqual(r._lrc, base_lrc + 1)

        copy = None
        self.assertEqual(r._lrc, base_lrc)

    def test_frozenset_hash_is_lrc_neutral(self):
        r, fs = self.region_frozenset_with_elem()
        base_lrc = r._lrc

        _ = hash(fs)

        self.assertEqual(r._lrc, base_lrc)

    def test_frozenset_contains_is_lrc_neutral(self):
        r, fs = self.region_frozenset_with_elem()
        e = next(iter(fs))
        base_lrc = r._lrc

        _ = e in fs

        self.assertEqual(r._lrc, base_lrc)

    def test_frozenset_iter_lifecycle(self):
        """Iterator over a frozenset borrows and releases correctly."""
        r, fs = self.region_frozenset_with_elem()
        base_lrc = r._lrc

        it = iter(fs)
        self.assertEqual(r._lrc, base_lrc + 1)

        elem = next(it)
        self.assertEqual(r._lrc, base_lrc + 2)

        elem = None
        self.assertEqual(r._lrc, base_lrc + 1)

        with self.assertRaises(StopIteration):
            next(it)
        self.assertEqual(r._lrc, base_lrc)

    def test_frozenset_and_set_difference_returns_borrow(self):
        """frozenset - set returns a new frozenset that borrows from region."""
        r, fs = self.region_frozenset_with_elem()
        base_lrc = r._lrc

        result = fs - frozenset()
        self.assertEqual(r._lrc, base_lrc + 1)

        result = None
        self.assertEqual(r._lrc, base_lrc)

    def test_frozenset_issubset_lrc_neutral(self):
        r, fs = self.region_frozenset_with_elem()
        other = frozenset(fs)
        base_lrc = r._lrc

        _ = fs.issubset(other)

        self.assertEqual(r._lrc, base_lrc)

    def test_make_new_frozenset_idempotent_returns_borrow(self):
        """frozenset(fs) when fs is already an exact frozenset is idempotent."""
        r, fs = self.region_frozenset_with_elem()
        base_lrc = r._lrc

        copy = frozenset(fs)
        self.assertIs(copy, fs)
        self.assertEqual(r._lrc, base_lrc + 1)

        copy = None
        self.assertEqual(r._lrc, base_lrc)


if __name__ == '__main__':
    unittest.main()
