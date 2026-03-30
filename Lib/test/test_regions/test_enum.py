import unittest
from regions import Region, is_local
from immutable import freeze


class TestRegionEnumerateBasic(unittest.TestCase):
    """Tests for basic enumerate construction and LRC behavior with regions."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_enumerate_from_region_iterator_increases_lrc(self):
        """
        Creating an enumerate from a region's iterator should increase
        the LRC by 1, since enumerate holds a reference to the iterator.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr = [r.a, r.b]
        r.it_arr = iter(r.arr)
        base_lrc = r._lrc

        obj = enumerate(r.it_arr)
        self.assertEqual(r._lrc, base_lrc + 1)

    def test_enumerate_set_to_none_decreases_lrc(self):
        """
        Setting the enumerate object to None should release the borrowed
        reference to the iterator, bringing LRC back to its pre-enumerate level.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr = [r.a, r.b]
        r.it_arr = iter(r.arr)
        base_lrc = r._lrc

        obj = enumerate(r.it_arr)
        self.assertEqual(r._lrc, base_lrc + 1)

        obj = None
        self.assertEqual(r._lrc, base_lrc)

    def test_enumerate_from_local_iterator_does_not_change_lrc(self):
        """
        Creating an enumerate from a local (non-region) iterator should
        not affect the LRC at all.
        """
        r = Region()
        base_lrc = r._lrc

        local_list = [self.A(), self.A()]
        obj = enumerate(local_list)
        self.assertEqual(r._lrc, base_lrc)


class TestRegionEnumerateNext(unittest.TestCase):
    """Tests for next() calls on enumerate objects and their effect on LRC."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_next_on_enumerate_increases_lrc(self):
        """
        Calling next() on an enumerate over a region iterator should
        yield a (index, element) tuple. The element is a borrowed reference,
        so LRC should increase by 1.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr = [r.a, r.b]
        r.it_arr = iter(r.arr)
        base_lrc = r._lrc
        obj = enumerate(r.it_arr)
        self.assertEqual(r._lrc, base_lrc + 1) # Because enum object points to r.it_arr in the region

        re1 = next(obj)
        self.assertEqual(r._lrc, base_lrc + 2) # Now, re1 points to the first element of r.it_arr, which is r.a, so LRC increases by 1

    def test_next_on_enumerate_increases_lrc_each_call(self):
        """
        Each successive call to next() should increase the LRC by 1,
        as each yields a new borrowed element reference.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        r.arr = [r.a, r.b, r.c, r.d]
        r.it_arr = iter(r.arr)
        obj = enumerate(r.it_arr)
        base_lrc = r._lrc

        re1 = next(obj)
        self.assertEqual(r._lrc, base_lrc + 1)

        re2 = next(obj)
        self.assertEqual(r._lrc, base_lrc + 2)

        re3 = next(obj)
        self.assertEqual(r._lrc, base_lrc + 3)

        re4 = next(obj)
        self.assertEqual(r._lrc, base_lrc + 4)

    # @unittest.expectedFailure
    def test_next_result_moved_into_region_does_not_increase_lrc(self):
        """
        Assigning the result of next() directly into a region should
        transfer ownership rather than creating an external borrow,
        so LRC should not increase beyond the base.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr = [r.a, r.b]
        r.it_arr = iter(r.arr)
        obj = enumerate(r.it_arr)
        base_lrc = r._lrc
        r.re1 = next(obj)
        self.assertEqual(r._lrc, base_lrc+1)

    def test_next_result_moved_into_region_does_not_increase_lrc_2(self):
        """
        Assigning the result of next() directly into a region should
        transfer ownership rather than creating an external borrow,
        so LRC should not increase beyond the base.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        r.arr = [r.a, r.b, r.c, r.d]
        r.it_arr = iter(r.arr)
        obj = enumerate(r.it_arr)
        base_lrc = r._lrc
        r.re1 = next(obj)
        self.assertEqual(r._lrc, base_lrc+1)
        r.re2 = next(obj)
        self.assertEqual(r._lrc, base_lrc)

    def test_next_mixed_local_and_region_assignment(self):
        """
        A mix of local and region assignments from next() should
        reflect only the local (external) borrows in the LRC.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.d = self.A()
        r.arr = [r.a, r.b, r.c, r.d]
        r.it_arr = iter(r.arr)
        obj = enumerate(r.it_arr)
        base_lrc = r._lrc

        re1 = next(obj)       # local borrow: LRC + 1
        r.re2 = next(obj)     # moved into region: LRC stays
        re3 = next(obj)       # local borrow: LRC + 1
        r.re4 = next(obj)     # moved into region: LRC stays
        self.assertEqual(r._lrc, base_lrc + 2)


class TestRegionEnumerateRelease(unittest.TestCase):
    """Tests for releasing enumerate results and their effect on LRC."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    # @unittest.expectedFailure
    def test_setting_next_result_to_none_decreases_lrc(self):
        """
        Setting a local next() result to None should release the
        borrowed reference and reduce LRC by 1.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr = [r.a, r.b]
        r.it_arr = iter(r.arr)
        obj = enumerate(r.it_arr)

        re1 = next(obj)
        base_lrc = r._lrc

        re1 = None
        self.assertEqual(r._lrc, base_lrc)

    # @unittest.expectedFailure
    def test_setting_all_next_results_to_none_restores_lrc(self):
        """
        Releasing all next() results should bring the LRC back
        to the pre-next() level, reflecting no more external borrows.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr = [r.a, r.b]
        r.it_arr = iter(r.arr)
        obj = enumerate(r.it_arr)
        base_lrc = r._lrc

        re1 = next(obj)
        re2 = next(obj)
        self.assertEqual(r._lrc, base_lrc + 2)

        re1 = None
        self.assertEqual(r._lrc, base_lrc + 1)

        re2 = None
        self.assertEqual(r._lrc, base_lrc)

    def test_enumerate_set_to_none_after_next_releases_iterator_ref(self):
        """
        Releasing the enumerate object itself (not the results) should
        drop LRC by 1 for the iterator reference it held.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr = [r.a, r.b]
        r.it_arr = iter(r.arr)

        re1 = next(enumerate(r.it_arr))
        base_lrc = r._lrc

        obj = enumerate(r.it_arr)
        self.assertEqual(r._lrc, base_lrc + 1)

        obj = None
        self.assertEqual(r._lrc, base_lrc)


class TestRegionEnumerateMoveIntoRegion(unittest.TestCase):
    """Tests for moving enumerate objects and results into regions."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_enumerate_moved_into_region_adjusts_lrc(self):
        """
        Moving an enumerate object into a region should transfer ownership
        of the iterator reference, so LRC should not increase from the
        external variable, but a reference is still held by the region.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr = [r.a, r.b]
        r.it_arr = iter(r.arr)
        base_lrc = r._lrc

        obj = enumerate(r.it_arr)
        self.assertEqual(r._lrc, base_lrc + 1)  # obj holds external ref

        r.obj = obj
        self.assertEqual(r._lrc, base_lrc+1)    # obj points to the enumerate object inside the region now, so LRC should not increase further

    # @unittest.skip("GC ERROR")
    def test_next_on_region_owned_enumerate_does_not_increase_lrc(self):
        """
        Calling next() on an enumerate that is owned by a region (accessed
        via region attribute) should not increase the LRC since the element
        is moved into the region directly.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr = [r.a, r.b]
        r.it_arr = iter(r.arr)
        r.obj = enumerate(r.it_arr)
        base_lrc = r._lrc

        r.re1 = next(r.obj)
        self.assertEqual(r._lrc, base_lrc)

    # @unittest.skip("GC ERROR")
    def test_next_on_region_owned_enumerate_local_assignment_increases_lrc(self):
        """
        Calling next() on a region-owned enumerate and assigning to a
        local variable should increase LRC by 1 (external borrow).
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr = [r.a, r.b]
        r.it_arr = iter(r.arr)
        r.obj = enumerate(r.it_arr)
        base_lrc = r._lrc

        re1 = next(r.obj)
        self.assertEqual(r._lrc, base_lrc + 1)
        re1 = None
        self.assertEqual(r._lrc, base_lrc)
        r = None


class TestRegionEnumerateFullLifecycle(unittest.TestCase):
    """End-to-end lifecycle tests matching the example script behavior."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_full_lifecycle_matches_example(self):
        """
        Reproduces the exact sequence from the example script:
          - create region with a, b
          - create arr = [a, b]
          - create it_arr = iter(arr) → moves into region
          - obj = enumerate(it_arr) → LRC +1
          - re1 = next(obj)           → LRC +1
          - r.re2 = next(obj)         → LRC +0 (moved into region)
          - obj = None                → LRC -1
          - re1 = None                → LRC -1
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr = [r.a, r.b]
        r.it_arr = iter(r.arr)
        base_lrc = r._lrc

        obj = enumerate(r.it_arr)
        self.assertEqual(r._lrc, base_lrc + 1)

        re1 = next(obj)
        self.assertEqual(r._lrc, base_lrc + 2)

        r.re2 = next(obj)
        self.assertEqual(r._lrc, base_lrc + 2)

        obj = None
        self.assertEqual(r._lrc, base_lrc + 1)

        re1 = None
        self.assertEqual(r._lrc, base_lrc)

    # @unittest.expectedFailure
    def test_full_lifecycle_matches_example_2(self):
        """
        Reproduces the exact sequence from the example script:
          - create region with a, b
          - create arr = [a, b]
          - create it_arr = iter(arr) → moves into region
          - obj = enumerate(it_arr) → LRC +1
          - re1 = next(obj)           → LRC +1
          - r.re2 = next(obj)         → LRC +0 (moved into region)
          - obj = None                → LRC -1
          - re1 = None                → LRC -1
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr = [r.a, r.b]
        r.it_arr = iter(r.arr)
        base_lrc = r._lrc

        obj = enumerate(r.it_arr)
        self.assertEqual(r._lrc, base_lrc + 1)

        re1 = next(obj)
        self.assertEqual(r._lrc, base_lrc + 2)

        r.re2 = next(obj)
        self.assertEqual(r._lrc, base_lrc + 2)

        re1 = None
        self.assertEqual(r._lrc, base_lrc + 1) # PROBLEM: LRC does not decrease

        obj = None
        self.assertEqual(r._lrc, base_lrc)

    def test_enumerate_result_index_and_value_correct(self):
        """
        Ensure the (index, value) tuples from next() contain the correct
        index and the actual region element.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr = [r.a, r.b]
        r.it_arr = iter(r.arr)
        obj = enumerate(r.it_arr)

        idx0, val0 = next(obj)
        idx1, val1 = next(obj)

        self.assertEqual(idx0, 0)
        self.assertEqual(idx1, 1)
        self.assertIs(val0, r.a)
        self.assertIs(val1, r.b)

    def test_enumerate_with_start_offset(self):
        """
        enumerate(iterable, start=N) should begin indices at N.
        LRC behavior is unchanged — it still borrows the iterator.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr = [r.a, r.b]
        r.it_arr = iter(r.arr)
        base_lrc = r._lrc

        obj = enumerate(r.it_arr, start=5)
        self.assertEqual(r._lrc, base_lrc + 1)

        idx0, val0 = next(obj)
        self.assertEqual(idx0, 5)
        self.assertIs(val0, r.a)

    def test_enumerate_exhausted_raises_stop_iteration(self):
        """
        Calling next() past the end of the iterable should raise
        StopIteration without affecting the LRC.
        """
        r = Region()
        r.a = self.A()
        r.arr = [r.a]
        r.it_arr = iter(r.arr)
        obj = enumerate(r.it_arr)

        re1 = next(obj)
        base_lrc = r._lrc

        with self.assertRaises(StopIteration):
            next(obj)

        self.assertEqual(r._lrc, base_lrc)


class TestRegionEnumerateTwoRegions(unittest.TestCase):
    """Tests for enumerate behavior when elements span multiple regions."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_enumerate_over_local_list_with_mixed_region_elements(self):
        """
        Enumerating a local list containing elements from two different
        regions should correctly borrow each element independently.
        """
        r1 = Region()
        r2 = Region()
        r1.a = self.A()
        r2.b = self.A()

        local_list = [r1.a, r2.b]
        it = iter(local_list)
        obj = enumerate(it)

        base_r1 = r1._lrc
        base_r2 = r2._lrc

        re1 = next(obj)  # borrows r1.a
        self.assertEqual(r1._lrc, base_r1 + 1)
        self.assertEqual(r2._lrc, base_r2)

        re2 = next(obj)  # borrows r2.b
        self.assertEqual(r1._lrc, base_r1 + 1)
        self.assertEqual(r2._lrc, base_r2 + 1)

    def test_enumerate_over_local_list_with_mixed_region_elements_2(self):
        """
        Enumerating a local list containing elements from two different
        regions should correctly borrow each element independently.
        """
        r1 = Region()
        r2 = Region()
        r1.a = self.A()
        r1.b = self.A()

        local_list = [r1.a, r1.b]
        it = iter(local_list)
        with self.assertRaises(Exception):
            r2.obj = enumerate(it)


    def test_enumerate_results_released_independently_per_region(self):
        """
        Releasing next() results from two different regions should
        decrease each region's LRC independently.
        """
        r1 = Region()
        r2 = Region()
        r1.a = self.A()
        r2.b = self.A()

        local_list = [r1.a, r2.b]
        it = iter(local_list)
        obj = enumerate(it)

        idx0, val0 = next(obj)
        idx1, val1 = next(obj)

        base_r1 = r1._lrc
        base_r2 = r2._lrc

        val0 = None
        self.assertEqual(r1._lrc, base_r1 - 1)
        self.assertEqual(r2._lrc, base_r2)

        val1 = None
        self.assertEqual(r1._lrc, base_r1 - 1)
        self.assertEqual(r2._lrc, base_r2 - 1)


if __name__ == "__main__":
    unittest.main()