"""Region write-barrier tests for the list type.

Each group maps to a distinct barrier shape introduced by the migration:

  Group A — Transfer: local element becomes region-owned (append/insert/extend/+=)
  Group B — Returning-borrow: item access returns a local borrow (subscript/slice/copy/concat)
  Group C — In-place operators return self via PyRegion_NewRef (+=, *=)
  Group D — Iterator lifecycle (forward and reverse: AddRef on create, RemoveRef on exhaustion/dealloc)
  Group E — Neutral operations: LRC unchanged (repr, richcompare, contains, remove, index, count, sort, reverse)
  Group F — Dealloc: RemoveRef per item
"""

import unittest
from regions import Region, is_local
from immutable import freeze


class A:
    """Plain element class for list contents.

    Using a plain class gives identity-based equality, which avoids surprising
    __eq__ / __hash__ side-effects during comparison-heavy operations.  The
    class is primed with freeze(A()) in setUpClass so that the first live
    instance can be moved into a region without being auto-frozen.
    """
    pass


class TestRegionList(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        freeze(A())

    # ---------------------------------------------------------------------- #
    # Group A — Transfer (local element → region-owned)                       #
    # ---------------------------------------------------------------------- #

    def test_append_transfers_element(self):
        """list.append moves a local element into the list's region."""
        r = Region()
        r.lst = []
        elem = A()
        self.assertTrue(is_local(elem))

        r.lst.append(elem)

        self.assertTrue(r.owns(elem))
        self.assertFalse(is_local(elem))

    def test_append_multiple_elements(self):
        """Appending multiple elements all transfer into the region."""
        r = Region()
        r.lst = []
        elems = [A(), A(), A()]
        for e in elems:
            r.lst.append(e)
        for e in elems:
            self.assertTrue(r.owns(e))

    def test_insert_transfers_element(self):
        """list.insert moves a local element into the list's region."""
        r = Region()
        r.lst = [A(), A()]
        elem = A()
        self.assertTrue(is_local(elem))

        r.lst.insert(0, elem)

        self.assertTrue(r.owns(elem))
        self.assertFalse(is_local(elem))

    def test_extend_transfers_elements(self):
        """list.extend transfers all local elements into the list's region."""
        r = Region()
        r.lst = []
        elems = [A(), A(), A()]

        r.lst.extend(elems)

        for e in elems:
            self.assertTrue(r.owns(e))

    def test_setitem_transfers_element(self):
        """Assigning via index moves a local element into the list's region."""
        r = Region()
        r.lst = [A()]
        new_elem = A()
        self.assertTrue(is_local(new_elem))

        r.lst[0] = new_elem

        self.assertTrue(r.owns(new_elem))

    def test_setslice_transfers_elements(self):
        """Slice assignment transfers all new local elements into the region."""
        r = Region()
        r.lst = [A(), A()]
        new_elems = [A(), A()]

        r.lst[0:2] = new_elems

        for e in new_elems:
            self.assertTrue(r.owns(e))

    # ---------------------------------------------------------------------- #
    # Group B — Returning-borrow (item access creates a local borrow)         #
    # ---------------------------------------------------------------------- #

    def test_getitem_returns_borrow(self):
        """list[i] returns a borrow; LRC rises while the result is held."""
        r = Region()
        r.lst = [A()]
        base = r._lrc

        item = r.lst[0]
        self.assertEqual(r._lrc, base + 1)

        item = None
        self.assertEqual(r._lrc, base)

    def test_getitem_negative_index_returns_borrow(self):
        """list[-1] also returns a borrow via the same code path."""
        r = Region()
        r.lst = [A(), A()]
        base = r._lrc

        item = r.lst[-1]
        self.assertEqual(r._lrc, base + 1)

        item = None
        self.assertEqual(r._lrc, base)

    def test_slice_returns_borrow_per_item(self):
        """list[i:j] borrows each item; LRC rises by the number of region-items."""
        r = Region()
        r.lst = [A(), A(), A()]
        base = r._lrc

        slc = r.lst[0:2]
        # Two items from r are in the slice — two borrows recorded.
        self.assertEqual(r._lrc, base + 2)

        slc = None
        self.assertEqual(r._lrc, base)

    def test_stepped_slice_returns_borrow_per_item(self):
        """list[::2] (stepped slice) borrows each copied item."""
        r = Region()
        r.lst = [A(), A(), A(), A()]
        base = r._lrc

        slc = r.lst[::2]   # elements 0 and 2 — two borrows
        self.assertEqual(r._lrc, base + 2)

        slc = None
        self.assertEqual(r._lrc, base)

    def test_copy_returns_borrow_per_item(self):
        """list.copy() borrows each item from the source region."""
        r = Region()
        r.lst = [A(), A()]
        base = r._lrc

        copy = r.lst.copy()
        self.assertTrue(is_local(copy))
        self.assertEqual(r._lrc, base + 2)

        copy = None
        self.assertEqual(r._lrc, base)

    def test_concat_returns_borrow_per_item(self):
        """list + list borrows each item from the source region."""
        r = Region()
        r.lst = [A(), A()]
        base = r._lrc

        result = r.lst + []
        self.assertTrue(is_local(result))
        self.assertEqual(r._lrc, base + 2)

        result = None
        self.assertEqual(r._lrc, base)

    def test_concat_both_operands_borrow(self):
        """list + list borrows items from both source regions."""
        r = Region()
        r.lst = [A()]
        r.other = [A()]
        base = r._lrc

        result = r.lst + r.other
        self.assertTrue(is_local(result))
        # One borrow per item from r (two items total: one from each list).
        self.assertEqual(r._lrc, base + 2)

        result = None
        self.assertEqual(r._lrc, base)

    # ---------------------------------------------------------------------- #
    # Group C — In-place operators return self via PyRegion_NewRef            #
    # ---------------------------------------------------------------------- #

    def test_iadd_returns_borrow(self):
        """list += other returns self via PyRegion_NewRef; LRC rises while held."""
        r = Region()
        r.lst = []
        base = r._lrc

        result = r.lst.__iadd__([])
        self.assertIs(result, r.lst)
        self.assertEqual(r._lrc, base + 1)

        result = None
        self.assertEqual(r._lrc, base)

    def test_iadd_transfers_new_elements(self):
        """list += other transfers appended elements into the list's region."""
        r = Region()
        r.lst = []
        elem = A()

        result = r.lst.__iadd__([elem])
        result = None

        self.assertTrue(r.owns(elem))

    def test_imul_returns_borrow(self):
        """list *= n returns self via PyRegion_NewRef; LRC rises while held."""
        r = Region()
        r.lst = [A()]
        base = r._lrc

        result = r.lst.__imul__(2)
        self.assertIs(result, r.lst)
        self.assertEqual(r._lrc, base + 1)

        result = None
        self.assertEqual(r._lrc, base)

    def test_imul_zero_returns_borrow(self):
        """list *= 0 still returns self via PyRegion_NewRef."""
        r = Region()
        r.lst = [A()]
        base = r._lrc

        result = r.lst.__imul__(0)
        self.assertIs(result, r.lst)
        self.assertEqual(r._lrc, base + 1)

        result = None
        self.assertEqual(r._lrc, base)

    # ---------------------------------------------------------------------- #
    # Group D — Iterator lifecycle                                             #
    # ---------------------------------------------------------------------- #

    def test_iter_adds_ref_to_list(self):
        """iter(list) records an owning reference to the list; LRC rises."""
        r = Region()
        r.lst = [A()]
        base = r._lrc

        it = iter(r.lst)
        self.assertEqual(r._lrc, base + 1)
        self.assertTrue(is_local(it))

        it = None
        self.assertEqual(r._lrc, base)

    def test_iter_next_returns_borrow(self):
        """Each next() call returns a local borrow of the yielded item."""
        r = Region()
        r.lst = [A()]
        base = r._lrc

        it = iter(r.lst)
        lrc_with_iter = r._lrc

        item = next(it)
        # One extra borrow for the item.
        self.assertEqual(r._lrc, lrc_with_iter + 1)

        item = None
        self.assertEqual(r._lrc, lrc_with_iter)

        it = None
        self.assertEqual(r._lrc, base)

    def test_iter_exhaustion_removes_ref(self):
        """Exhausting the iterator triggers RemoveRef(it, seq); LRC drops."""
        r = Region()
        r.lst = [A()]
        base = r._lrc

        it = iter(r.lst)
        self.assertEqual(r._lrc, base + 1)

        item = next(it)
        item = None
        try:
            next(it)
        except StopIteration:
            pass

        # RemoveRef fires on exhaustion inside listiter_next.
        self.assertEqual(r._lrc, base)

        it = None  # it_seq already NULL; dealloc RemoveRef is a no-op
        self.assertEqual(r._lrc, base)

    def test_iter_early_abandon_removes_ref(self):
        """Dropping a non-exhausted iterator triggers RemoveRef in dealloc."""
        r = Region()
        r.lst = [A(), A(), A()]
        base = r._lrc

        it = iter(r.lst)
        self.assertEqual(r._lrc, base + 1)
        next(it)  # partially consumed

        it = None  # dealloc → RemoveRef → LRC drops
        self.assertEqual(r._lrc, base)

    def test_iter_loop_per_item_borrow(self):
        """Each loop variable holds one extra borrow; clearing it releases it."""
        r = Region()
        r.lst = [A(), A()]
        base = r._lrc
        it = iter(r.lst)
        lrc_with_iter = r._lrc

        for v in it:
            self.assertEqual(r._lrc, lrc_with_iter + 1)
            v = None
            self.assertEqual(r._lrc, lrc_with_iter)

        v = None
        self.assertEqual(r._lrc, base)

    def test_reversed_iter_adds_ref(self):
        """reversed(list) also records an owning reference; LRC rises."""
        r = Region()
        r.lst = [A(), A()]
        base = r._lrc

        it = reversed(r.lst)
        self.assertEqual(r._lrc, base + 1)
        self.assertTrue(is_local(it))

        it = None
        self.assertEqual(r._lrc, base)

    def test_reversed_iter_exhaustion_removes_ref(self):
        """reversed(list) holds its ref until dealloc, not until StopIteration.

        The reversed iterator's exhaustion path goes through the early-return
        branch ``if (index < 0) return NULL``, which predates the region
        migration and never contained a Py_DECREF.  RemoveRef therefore only
        fires in listreviter_dealloc, not on the StopIteration-signalling call.
        """
        r = Region()
        r.lst = [A()]
        base = r._lrc

        it = reversed(r.lst)
        self.assertEqual(r._lrc, base + 1)

        item = next(it)
        item = None
        try:
            next(it)
        except StopIteration:
            pass

        # Owning ref still held — RemoveRef has not fired yet.
        self.assertEqual(r._lrc, base + 1)

        it = None  # listreviter_dealloc → RemoveRef
        self.assertEqual(r._lrc, base)

    def test_reversed_iter_next_returns_borrow(self):
        """Each reversed-iterator next() borrows one item."""
        r = Region()
        r.lst = [A()]
        base = r._lrc

        it = reversed(r.lst)
        lrc_with_iter = r._lrc

        item = next(it)
        self.assertEqual(r._lrc, lrc_with_iter + 1)

        item = None
        self.assertEqual(r._lrc, lrc_with_iter)

        it = None
        self.assertEqual(r._lrc, base)

    # ---------------------------------------------------------------------- #
    # Group E — Neutral operations (LRC unchanged throughout)                 #
    # ---------------------------------------------------------------------- #

    def test_repr_is_neutral(self):
        """repr(list) borrows items during formatting but LRC is neutral after."""
        r = Region()
        r.lst = [A()]
        base = r._lrc

        s = repr(r.lst)
        self.assertIn("[", s)
        self.assertEqual(r._lrc, base)

    def test_richcompare_is_neutral(self):
        """List equality comparison borrows items but LRC is neutral after."""
        r = Region()
        r.lst = [A()]
        base = r._lrc

        # Comparing against an empty list avoids per-element __eq__ calls.
        result = r.lst == []
        self.assertFalse(result)
        self.assertEqual(r._lrc, base)

    def test_contains_is_neutral(self):
        """'in' operator borrows items during search; LRC neutral after."""
        r = Region()
        elem = A()
        r.lst = [elem]
        base = r._lrc

        # elem is now owned by r; searching for a different object is neutral.
        result = A() in r.lst
        self.assertFalse(result)
        self.assertEqual(r._lrc, base)

    def test_remove_is_neutral(self):
        """list.remove borrows items for comparison; LRC neutral after removal."""
        r = Region()
        sentinel = A()
        r.lst = [A(), sentinel]
        base = r._lrc

        r.lst.remove(sentinel)
        self.assertEqual(r._lrc, base)

    def test_index_is_neutral(self):
        """list.index borrows items for comparison; LRC neutral after."""
        r = Region()
        target = A()
        r.lst = [A(), target]
        base = r._lrc

        idx = r.lst.index(target)
        self.assertEqual(idx, 1)
        self.assertEqual(r._lrc, base)

    def test_count_is_neutral(self):
        """list.count borrows items for comparison; LRC neutral after."""
        r = Region()
        elem = A()
        r.lst = [elem, A()]
        base = r._lrc

        n = r.lst.count(elem)
        self.assertEqual(n, 1)
        self.assertEqual(r._lrc, base)

    def test_sort_is_neutral(self):
        """list.sort reorders elements in place; LRC neutral after."""
        r = Region()
        r.lst = [A(), A(), A()]
        base = r._lrc

        r.lst.sort(key=id)
        self.assertEqual(r._lrc, base)

    def test_reverse_is_neutral(self):
        """list.reverse reorders elements in place without any borrow; neutral."""
        r = Region()
        r.lst = [A(), A()]
        base = r._lrc

        r.lst.reverse()
        self.assertEqual(r._lrc, base)

    def test_len_is_neutral(self):
        """len(list) reads the ob_size field; no borrow needed; neutral."""
        r = Region()
        r.lst = [A(), A()]
        base = r._lrc

        n = len(r.lst)
        self.assertEqual(n, 2)
        self.assertEqual(r._lrc, base)

    # ---------------------------------------------------------------------- #
    # Group F — Dealloc: RemoveRef per item                                   #
    # ---------------------------------------------------------------------- #

    def test_dealloc_no_crash(self):
        """Deallocating a region-owned list with region-owned elements is safe."""
        r = Region()
        r.lst = [A(), A(), A()]
        # r goes out of scope → list_dealloc fires RemoveRef per item; no crash.

    def test_dealloc_after_clear_no_crash(self):
        """Clearing a list and then deallocating it is safe."""
        r = Region()
        r.lst = [A(), A()]
        r.lst.clear()
        # Dealloc on an already-cleared list should not double-decrement.

    def test_clear_neutral_on_lrc(self):
        """list.clear removes owning references, not local borrows; LRC neutral."""
        r = Region()
        r.lst = [A(), A()]
        base = r._lrc

        r.lst.clear()
        # clear() calls RemoveRef (owning-ref removal), not RemoveLocalRef,
        # so the region's LRC is unchanged.
        self.assertEqual(r._lrc, base)


    # ---------------------------------------------------------------------- #
    # Group G — Isolation enforcement (cross-region rejection)               #
    # ---------------------------------------------------------------------- #

    def _make_sibling_elem(self):
        """Return (r2, elem) where elem is owned by r2."""
        r2 = Region()
        elem = A()
        r2.elem = elem
        return r2, elem

    def test_append_rejects_sibling_region_element(self):
        """append raises RuntimeError when element is owned by a different region."""
        r1 = Region()
        r1.lst = []
        r2, elem = self._make_sibling_elem()
        base = r1._lrc

        with self.assertRaises(RuntimeError):
            r1.lst.append(elem)

        self.assertEqual(len(r1.lst), 0)
        self.assertTrue(r2.owns(elem))
        self.assertEqual(r1._lrc, base)

    def test_insert_rejects_sibling_region_element(self):
        """insert raises RuntimeError when element is owned by a different region."""
        r1 = Region()
        r1.lst = [A()]
        r2, elem = self._make_sibling_elem()
        base = r1._lrc

        with self.assertRaises(RuntimeError):
            r1.lst.insert(0, elem)

        self.assertEqual(len(r1.lst), 1)
        self.assertTrue(r2.owns(elem))
        self.assertEqual(r1._lrc, base)

    def test_setitem_rejects_sibling_region_element(self):
        """Index assignment raises RuntimeError when new element is in a sibling region."""
        r1 = Region()
        r1.lst = [A()]
        r2, elem = self._make_sibling_elem()
        base = r1._lrc

        with self.assertRaises(RuntimeError):
            r1.lst[0] = elem

        self.assertEqual(len(r1.lst), 1)
        self.assertTrue(r2.owns(elem))
        self.assertEqual(r1._lrc, base)

    def test_setslice_rejects_sibling_region_element(self):
        """Slice assignment raises RuntimeError when replacement element is in a sibling region."""
        r1 = Region()
        r1.lst = [A()]
        r2, elem = self._make_sibling_elem()
        base = r1._lrc

        with self.assertRaises(RuntimeError):
            r1.lst[0:1] = [elem]

        self.assertEqual(r1._lrc, base)

    def test_extend_rejects_sibling_region_element(self):
        """extend raises RuntimeError when any element is owned by a different region."""
        r1 = Region()
        r1.lst = []
        r2, elem = self._make_sibling_elem()
        base = r1._lrc

        with self.assertRaises(RuntimeError):
            r1.lst.extend([elem])

        self.assertEqual(len(r1.lst), 0)
        self.assertEqual(r1._lrc, base)

    def test_extend_rollback_on_partial_failure(self):
        """extend rolls back all transfers atomically if one element is invalid.

        Layout: [local, sibling-owned, local].  The middle element is already
        owned by a sibling region.  All three elements must remain in their
        original locations after the RuntimeError — in particular 'before' and
        'after' must stay local, proving the transfer was rolled back and not
        committed up to the point of failure.
        """
        r1 = Region()
        r1.lst = []
        before = A()
        r2, bad = self._make_sibling_elem()
        after = A()
        base = r1._lrc

        with self.assertRaises(RuntimeError):
            r1.lst.extend([before, bad, after])

        self.assertEqual(len(r1.lst), 0)
        self.assertTrue(is_local(before))   # not transferred
        self.assertTrue(r2.owns(bad))
        self.assertTrue(is_local(after))    # not transferred either
        self.assertEqual(r1._lrc, base)

    def test_iadd_rollback_on_partial_failure(self):
        """list += [local, sibling-owned, local] rolls back atomically.

        Mirrors test_extend_rollback_on_partial_failure for the iadd path
        (list_inplace_concat → list_extend_fast → PyRegion_AddRefsArray).
        """
        r1 = Region()
        r1.lst = []
        before = A()
        r2, bad = self._make_sibling_elem()
        after = A()
        base = r1._lrc

        with self.assertRaises(RuntimeError):
            r1.lst.__iadd__([before, bad, after])

        self.assertEqual(len(r1.lst), 0)
        self.assertTrue(is_local(before))
        self.assertTrue(r2.owns(bad))
        self.assertTrue(is_local(after))
        self.assertEqual(r1._lrc, base)

    def test_iadd_rejects_sibling_region_element(self):
        """list += other raises RuntimeError when other contains a sibling-region element."""
        r1 = Region()
        r1.lst = []
        r2, elem = self._make_sibling_elem()
        base = r1._lrc

        with self.assertRaises(RuntimeError):
            r1.lst.__iadd__([elem])

        self.assertEqual(len(r1.lst), 0)
        self.assertTrue(r2.owns(elem))
        self.assertEqual(r1._lrc, base)


if __name__ == "__main__":
    unittest.main()
