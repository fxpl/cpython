import unittest
from itertools import batched, pairwise
from regions import Region, is_local
from immutable import freeze, register_freezable


freeze(batched)
freeze(pairwise)


class TestRegionBatchedBasic(unittest.TestCase):
    """Tests for basic batched iterator construction and LRC behavior with regions."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_initial_lrc(self):
        r = Region()
        self.assertEqual(r._lrc, 1)

    def test_batched_from_local_iter_does_not_change_lrc(self):
        """
        Creating a batched iterator from a local iterator over a region array
        should not change the LRC — the batched object itself is local and
        holds no references yet.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.arr = [r.a, r.b, r.c]

        it = iter(r.arr)
        base_lrc = r._lrc

        obj = batched(it, 2)
        self.assertEqual(r._lrc, base_lrc)
        self.assertTrue(is_local(obj))

    def test_batched_moved_into_region_adjusts_lrc(self):
        """
        Moving a batched iterator into a region should adjust the LRC:
        the region now owns the batched object, so the external reference
        is no longer borrowed.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.arr = [r.a, r.b, r.c]
        r.it_arr = iter(r.arr)
        base_lrc = r._lrc

        obj = batched(r.it_arr, 2)
        self.assertEqual(r._lrc, base_lrc + 1)  # obj borrows r.it_arr

        r.obj = obj
        self.assertEqual(r._lrc, base_lrc+1) # obj points to the batched object.


class TestRegionBatchedNext(unittest.TestCase):
    """Tests for next() on batched iterators and their effect on LRC."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_next_on_local_batched_increases_lrc(self):
        """
        Calling next() on a local batched iterator yields a tuple of
        borrowed references, increasing the LRC by the batch size.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        r.e = self.A()
        r.f = self.A()
        r.arr = [r.a, r.b, r.c, r.d, r.e, r.f]
        r.it_arr = iter(r.arr)

        obj = batched(r.it_arr, 3)
        base_lrc = r._lrc

        x = next(obj)
        # x is a tuple of 3 borrowed refs (a, b, c)
        self.assertEqual(r._lrc, base_lrc + 3)

    def test_next_twice_on_local_batched_increases_lrc_cumulatively(self):
        """
        Each call to next() borrows another batch of elements, so the LRC
        should increase by batch_size for each call.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        r.e = self.A()
        r.f = self.A()
        r.arr = [r.a, r.b, r.c, r.d, r.e, r.f]
        r.it_arr = iter(r.arr)

        obj = batched(r.it_arr, 3)
        base_lrc = r._lrc

        x = next(obj)
        self.assertEqual(r._lrc, base_lrc + 3)

        y = next(obj)
        self.assertEqual(r._lrc, base_lrc + 6)

    def test_next_result_released_decreases_lrc(self):
        """
        Releasing the tuple returned by next() should release all borrowed
        references it holds, bringing the LRC back down.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        r.e = self.A()
        r.f = self.A()
        r.arr = [r.a, r.b, r.c, r.d, r.e, r.f]
        r.it_arr = iter(r.arr)

        obj = batched(r.it_arr, 3)
        base_lrc = r._lrc

        x = next(obj)
        self.assertEqual(r._lrc, base_lrc + 3)

        y = next(obj)
        self.assertEqual(r._lrc, base_lrc + 6)

        x = None
        self.assertEqual(r._lrc, base_lrc + 3)

        y = None
        self.assertEqual(r._lrc, base_lrc)

    def test_next_result_moved_into_region_adjusts_lrc(self):
        """
        Assigning the result of next() directly into the region transfers
        ownership of the tuple and its elements, so the LRC should not
        increase by the full batch size for a region-owned result.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        r.e = self.A()
        r.f = self.A()
        r.arr = [r.a, r.b, r.c, r.d, r.e, r.f]
        r.it_arr = iter(r.arr)

        r.obj = batched(r.it_arr, 3)
        base_lrc = r._lrc

        x = next(r.obj)        # external local ref to tuple of 3 elements
        self.assertEqual(r._lrc, base_lrc + 3)

        r.y = next(r.obj)      # moved into region, no external borrow
        self.assertEqual(r._lrc, base_lrc + 3)  # only x's 3 refs still borrowed


class TestRegionBatchedRelease(unittest.TestCase):
    """Tests for releasing batched iterators and their effect on LRC."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_batched_set_to_none_releases_lrc(self):
        """
        Setting the batched iterator to None before consuming it should
        release any references it holds (none, if next() was never called).
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.arr = [r.a, r.b, r.c]
        r.it_arr = iter(r.arr)
        base_lrc = r._lrc

        obj = batched(r.it_arr, 2)
        obj = None
        self.assertEqual(r._lrc, base_lrc)

    def test_partial_consumption_then_release(self):
        """
        Releasing a partially consumed batched iterator (after one next())
        should only drop the reference to the batched object itself, not
        any already-yielded tuples.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        r.arr = [r.a, r.b, r.c, r.d]
        r.it_arr = iter(r.arr)

        obj = batched(r.it_arr, 2)
        x = next(obj)   # borrows a, b
        base_lrc = r._lrc

        obj = None      # release the iterator; x still holds a, b
        self.assertEqual(r._lrc, base_lrc-1)

        x = None        # now release the tuple
        self.assertEqual(r._lrc, base_lrc-1-2)  # -1 for obj, -2 for a,b

    def test_region_owned_batched_set_to_none(self):
        """
        Releasing a region-owned batched iterator by setting it to None
        should clean up any internal state without external LRC leaks.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        r.e = self.A()
        r.f = self.A()
        r.arr = [r.a, r.b, r.c, r.d, r.e, r.f]
        r.it_arr = iter(r.arr)

        r.obj = batched(r.it_arr, 3)
        x = next(r.obj)   # borrows 3 elements externally
        r.y = next(r.obj) # owned by region

        base_lrc = r._lrc

        x = None
        self.assertEqual(r._lrc, base_lrc - 3)

        r.y = None
        self.assertEqual(r._lrc, base_lrc - 3)  # r.y was owned, no external borrow change


class TestRegionBatchedUnevenBatch(unittest.TestCase):
    """Tests for batched iterators where the last batch is smaller than batch_size."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_last_batch_smaller(self):
        """
        When the number of elements is not evenly divisible by batch_size,
        the last batch should contain fewer elements, and the LRC increase
        should reflect the actual number of elements in that batch.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        r.e = self.A()
        r.arr = [r.a, r.b, r.c, r.d, r.e]
        r.it_arr = iter(r.arr)

        obj = batched(r.it_arr, 3)
        base_lrc = r._lrc

        x = next(obj)           # full batch: a, b, c
        self.assertEqual(r._lrc, base_lrc + 3)

        y = next(obj)           # partial batch: d, e
        self.assertEqual(r._lrc, base_lrc + 3 + 2)

        x = None
        self.assertEqual(r._lrc, base_lrc + 2)

        y = None
        self.assertEqual(r._lrc, base_lrc)

    def test_single_element_batch(self):
        """
        A batch size of 1 should yield one element at a time, increasing
        the LRC by 1 per next() call.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr = [r.a, r.b]
        r.it_arr = iter(r.arr)

        obj = batched(r.it_arr, 1)
        base_lrc = r._lrc

        x = next(obj)
        self.assertEqual(r._lrc, base_lrc + 1)

        y = next(obj)
        self.assertEqual(r._lrc, base_lrc + 2)

        x = None
        y = None
        self.assertEqual(r._lrc, base_lrc)


class TestRegionBatchedFromLocalIterator(unittest.TestCase):
    """Tests for batched over a local (non-region) iterator that yields region objects."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_batched_from_local_list_of_region_objects(self):
        """
        Creating a batched iterator from a plain local list containing
        region objects should borrow those objects when next() is called.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        arr = [r.a, r.b, r.c, r.d]  # local list, not in region

        obj = batched(iter(arr), 2)
        base_lrc = r._lrc

        x = next(obj)
        self.assertEqual(r._lrc, base_lrc + 2)

        y = next(obj)
        self.assertEqual(r._lrc, base_lrc + 4)

        x = None
        y = None
        self.assertEqual(r._lrc, base_lrc)

    def test_batched_next_into_region_from_local_list(self):
        """
        Assigning the result of next() into the region when iterating
        a local list of region objects should transfer ownership.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        arr = [r.a, r.b, r.c, r.d]

        obj = batched(iter(arr), 2)
        base_lrc = r._lrc

        r.x = next(obj)    # owned by region
        self.assertEqual(r._lrc, base_lrc)

        y = next(obj)      # external borrow
        self.assertEqual(r._lrc, base_lrc + 2)

        y = None
        self.assertEqual(r._lrc, base_lrc)


class TestRegionBatchedIsLocal(unittest.TestCase):
    """Tests that batched iterators and their results have correct locality."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_local_batched_is_local(self):
        """
        A batched iterator created outside of a region should be local.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr = [r.a, r.b]
        it = iter(r.arr)

        obj = batched(it, 2)
        self.assertTrue(is_local(obj))

    def test_next_result_is_local(self):
        """
        The tuple returned by next() on a local batched iterator should be local.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr = [r.a, r.b]
        r.it_arr = iter(r.arr)

        obj = batched(r.it_arr, 2)
        x = next(obj)
        self.assertTrue(is_local(x))

    def test_region_owned_batched_is_not_local(self):
        """
        A batched iterator moved into a region should no longer be local.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr = [r.a, r.b]
        r.it_arr = iter(r.arr)

        r.obj = batched(r.it_arr, 2)
        self.assertFalse(is_local(r.obj))

class TestRegionPairwiseBasic(unittest.TestCase):
    """Tests for basic pairwise iterator construction and LRC behavior."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_pairwise_from_region_iterator_does_not_change_lrc(self):
        """
        Creating a pairwise iterator from a region-owned iterator should
        not change the LRC — the pairwise object borrows the iterator
        but holds no element references yet.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.arr = [r.a, r.b, r.c]
        r.it_arr = iter(r.arr)
        base_lrc = r._lrc

        obj = pairwise(r.it_arr)
        # pairwise borrows r.it_arr externally
        self.assertEqual(r._lrc, base_lrc + 1)
        self.assertTrue(is_local(obj))

    def test_pairwise_from_local_iterator_does_not_change_lrc(self):
        """
        Creating a pairwise iterator from a local iterator (not region-owned)
        should not change the LRC.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.arr = [r.a, r.b, r.c]
        it = iter(r.arr)
        base_lrc = r._lrc

        obj = pairwise(it)
        self.assertEqual(r._lrc, base_lrc)
        self.assertTrue(is_local(obj))

    def test_pairwise_moved_into_region_adjusts_lrc(self):
        """
        Moving a pairwise iterator into a region should transfer ownership,
        removing the external borrow on the underlying iterator.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.arr = [r.a, r.b, r.c]
        r.it_arr = iter(r.arr)
        base_lrc = r._lrc

        obj = pairwise(r.it_arr)
        self.assertEqual(r._lrc, base_lrc + 1)  # pairwise points to r.it_arr

        r.obj = obj
        self.assertEqual(r._lrc, base_lrc + 1)  # obj points to pairwise


class TestRegionPairwiseNextLocal(unittest.TestCase):
    """
    Tests for next() on a local pairwise iterator over a region-owned iterator.

    pairwise internally caches the right element of the last yielded pair as `old`.
    On the first next():
      - yields (left, right): LRC +2 for the tuple elements, +1 for obj's internal
        ref to `old` (right) = +3 total.
    On subsequent next() calls (when `old` is already cached):
      - the new left == previous right, already counted via `old`,
        so: +2 for the new tuple, -1 for releasing old `old` ref = net +1... 
        but if previous tuple is still alive (sharing the right element),
        the ref to `old` is not a new borrow = +2 net.
    """

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_first_next_increases_lrc_by_3(self):
        """
        The first next() yields (a, b) and caches b as `old`.
        LRC increases by 3: +2 for the tuple (a, b), +1 for internal `old` ref to b.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        r.arr = [r.a, r.b, r.c, r.d]
        r.it_arr = iter(r.arr)

        obj = pairwise(r.it_arr)
        base_lrc = r._lrc

        x = next(obj)
        self.assertEqual(r._lrc, base_lrc + 3)  # tuple(a,b) + internal old->b

    def test_release_first_tuple_decreases_lrc_by_2(self):
        """
        Releasing the first tuple (a, b) drops 2 refs.
        The internal `old` ref to b is still held by pairwise, so LRC drops by 2.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        r.arr = [r.a, r.b, r.c, r.d]
        r.it_arr = iter(r.arr)

        obj = pairwise(r.it_arr)
        base_lrc = r._lrc

        x = next(obj)
        self.assertEqual(r._lrc, base_lrc + 3)

        x = None  # releases (a, b) but old still holds b
        self.assertEqual(r._lrc, base_lrc + 3)  # The tuple that points to two elements in the region is not deallocated because obj->result still points to.

    def test_second_next_after_release_does_not_increase_lrc(self):
        """
        After releasing x=(a,b) and calling next() again to get (b,c):
        b is already borrowed via `old`, so only c is a new borrow.
        But old is updated to c, releasing the old->b ref.
        Net change from the second next(): 0 (new tuple borrows b,c: +2,
        old releases b: -1, old takes c: already counted in tuple = net +2 -1 = +1,
        but since we re-use old's slot for the right element: net 0 from base+1).
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        r.arr = [r.a, r.b, r.c, r.d]
        r.it_arr = iter(r.arr)

        obj = pairwise(r.it_arr)
        base_lrc = r._lrc

        x = next(obj) # LRC +3
        x = None # LRC -0
        x = next(obj) # LRC +0 since the first next(obj) has set up the internal state of pairwise to reuse the old ref for the right element of the new tuple.

        self.assertEqual(r._lrc, base_lrc + 3)

    def test_second_next_while_first_tuple_alive(self):
        """
        Calling next() a second time while first tuple x=(a,b) is still alive.
        Second tuple y=(b,c): b is shared between x and old, c is new.
        old moves from b to c: -1 for old->b, +1 for old->c, +1 for new c in y's tuple.
        But b in y's tuple was already in old: net +2 for y (b already counted), -1 for old release of b, +1 new old->c.
        Total from base_lrc (after first next at base+3): +2 for y = base+3+2 = base+5? 
        Actually: y=(b,c) borrows b (was in old, now also in tuple +1) and c (+1), old->c replaces old->b (net 0 for old).
        So +2 for tuple y's new borrows.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        r.arr = [r.a, r.b, r.c, r.d]
        r.it_arr = iter(r.arr)

        obj = pairwise(r.it_arr)
        base_lrc = r._lrc # Initial LRC includes the borrow ref from pairwise to r.it_arr

        x = next(obj)   # (a,b), old=b  → base+3
        self.assertEqual(r._lrc, base_lrc + 3)

        y = next(obj)   # (b,c), old=c  → +2 for (b,c) in y, old moves b→c (net 0)
        self.assertEqual(r._lrc, base_lrc + 5)

        x = None        # releases (a, b): -2
        self.assertEqual(r._lrc, base_lrc + 3)

        y = None        # releases (b, c): -2
        self.assertEqual(r._lrc, base_lrc + 1)  # only old->c remains

        obj = None      # releases pairwise (drops old->c and it_arr borrow)
        self.assertEqual(r._lrc, base_lrc - 1)

    def test_three_nexts_full_sequence(self):
        """
        Full sequence over [a,b,c,d,e,f] with batch of pairs:
        next1 → (a,b): +3 (tuple a,b + old b)
        next2 → (b,c): +2 (tuple b,c; old moves b→c)
        next3 → (c,d): +2 (tuple c,d; old moves c→d)
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        r.e = self.A()
        r.f = self.A()
        r.arr = [r.a, r.b, r.c, r.d, r.e, r.f]
        r.it_arr = iter(r.arr)

        obj = pairwise(r.it_arr)
        base_lrc = r._lrc

        x = next(obj)
        self.assertEqual(r._lrc, base_lrc + 3)

        y = next(obj)
        self.assertEqual(r._lrc, base_lrc + 5)

        z = next(obj)
        self.assertEqual(r._lrc, base_lrc + 7)

        x = None
        self.assertEqual(r._lrc, base_lrc + 5)

        y = None
        self.assertEqual(r._lrc, base_lrc + 3)

        z = None
        self.assertEqual(r._lrc, base_lrc + 1)

        obj = None
        self.assertEqual(r._lrc, base_lrc - 1)


class TestRegionPairwiseNextIntoRegion(unittest.TestCase):
    """Tests for assigning next() results into the region."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_first_next_into_region_lrc(self):
        """
        From commented block 3:
        r.x = next(obj): LRC +1 from obj->tuple (owned by region) and +1 from old->right_element.
        Since the tuple is owned by the region, its elements are not external borrows.
        But old still holds an external ref to the right element.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        r.arr = [r.a, r.b, r.c, r.d]
        r.it_arr = iter(r.arr)

        obj = pairwise(r.it_arr)
        base_lrc = r._lrc

        # obj->result points to tuple that is now owned by region. LRC +1
        # obj->old points to right element of tuple, which is still an external borrow. LRC +1
        r.x = next(obj)  
        self.assertEqual(r._lrc, base_lrc + 2)

    def test_second_next_local_after_first_into_region(self):
        """
        From commented block 3:
        After r.x = next(obj) (LRC base+1), y = next(obj):
        yields (b,c), tuple borrowed externally (+2), old moves from b to c (-1+1=0 net for old).
        But b was already in old (base+1 had old->b), so:
        y's tuple borrows b (+1) and c (+1), old releases b (-1) and holds c (already in tuple).
        Net: base+1 + 2(tuple y) - 1(old releases b) = base+2.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        r.arr = [r.a, r.b, r.c, r.d]
        r.it_arr = iter(r.arr)

        obj = pairwise(r.it_arr)
        base_lrc = r._lrc

        r.x = next(obj)
        self.assertEqual(r._lrc, base_lrc + 2)

        y = next(obj)   # (b,c): +2 for tuple, -1 from derefing obj->result to tuple
        self.assertEqual(r._lrc, base_lrc + 2 + 1)

        r.x = None      # region-owned tuple released; no external change
        self.assertEqual(r._lrc, base_lrc + 2 + 1)

        y = None        # external tuple (b,c) released: -2
        self.assertEqual(r._lrc, base_lrc + 1)  # old->c... wait, old still holds c

        obj = None
        self.assertEqual(r._lrc, base_lrc - 1)


class TestRegionPairwiseRelease(unittest.TestCase):
    """Tests for releasing the pairwise iterator and its effect on LRC."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_release_unconsumed_pairwise_from_region_iter(self):
        """
        Releasing a pairwise iterator that was never consumed (no next() calls)
        should release the borrow on the underlying region iterator.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.arr = [r.a, r.b, r.c]
        r.it_arr = iter(r.arr)
        base_lrc = r._lrc

        obj = pairwise(r.it_arr)
        self.assertEqual(r._lrc, base_lrc + 1)

        obj = None
        self.assertEqual(r._lrc, base_lrc)

    def test_release_pairwise_after_partial_consumption(self):
        """
        From commented block 1:
        obj = None after consuming 3 pairs (with x, y released and r.z owned):
        releases obj->it_arr and obj->old (-2 total from obj's own refs).
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        r.e = self.A()
        r.f = self.A()
        r.arr = [r.a, r.b, r.c, r.d, r.e, r.f]
        r.it_arr = iter(r.arr)

        obj = pairwise(r.it_arr)
        base_lrc = r._lrc

        x = next(obj)   # (a,b) +3
        y = next(obj)   # (b,c) +2 → base+5
        r.z = next(obj) # (c,d) owned → base+5+1(old->d) = base+... let's track:
                        # r.z owned so tuple not borrowed externally, old moves c→d: +1(old->d)-1(old->c)
                        # net from base+5: base+5-1(old c release)+1(old d) = base+5

        x = None        # -2 → base+3
        y = None        # -2 → base+1  (old still holds d)
        r.z = None      # region owned, net 0 → base+1

        lrc_before_obj_none = r._lrc
        obj = None      # releases old->d and obj->it_arr
        self.assertEqual(r._lrc, lrc_before_obj_none - 2)


class TestRegionPairwiseIsLocal(unittest.TestCase):
    """Tests for locality of pairwise iterators and their yielded tuples."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_local_pairwise_is_local(self):
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr = [r.a, r.b]
        it = iter(r.arr)

        obj = pairwise(it)
        self.assertTrue(is_local(obj))

    def test_next_result_is_local(self):
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.arr = [r.a, r.b, r.c]
        r.it_arr = iter(r.arr)

        obj = pairwise(r.it_arr)
        x = next(obj)
        self.assertTrue(is_local(x))

    def test_region_owned_pairwise_is_not_local(self):
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.arr = [r.a, r.b, r.c]
        r.it_arr = iter(r.arr)

        r.obj = pairwise(r.it_arr)
        self.assertFalse(is_local(r.obj))

    def test_region_owned_next_result_is_not_local(self):
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.arr = [r.a, r.b, r.c]
        r.it_arr = iter(r.arr)

        obj = pairwise(r.it_arr)
        r.x = next(obj)
        self.assertFalse(is_local(r.x))


if __name__ == "__main__":
    unittest.main()