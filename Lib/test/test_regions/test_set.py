import unittest
from regions import Region, is_local
from immutable import freeze, isfrozen


class TestRegionSet(unittest.TestCase):
    """Tests for basic set construction and LRC behavior with regions."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_set_from_region_array_increases_lrc(self):
        """
        Creating a set from a region array should increase the LRC
        for each element borrowed from the region.
        """
        r = Region()
        r.word = self.A()
        r.word2 = self.A()
        r.arr = [r.word, r.word2]
        base_lrc = r._lrc

        s = set(r.arr)

        self.assertEqual(r._lrc, base_lrc + 2)

    def test_set_to_none_decreases_lrc(self):
        """
        Setting the set to None should release the borrowed references
        and bring LRC back to its pre-set level.
        """
        r = Region()
        r.word = self.A()
        r.word2 = self.A()
        r.arr = [r.word, r.word2]
        base_lrc = r._lrc

        s = set(r.arr)
        self.assertEqual(r._lrc, base_lrc + 2)

        s = None
        self.assertEqual(r._lrc, base_lrc)

    def test_set_moved_into_region_adjusts_lrc(self):
        """
        Moving a set into a region should transfer ownership of its elements,
        reducing the LRC by the number of elements (now owned) minus the
        external reference to the set itself.
        """
        r = Region()
        r.word = self.A()
        r.word2 = self.A()
        r.word3 = self.A()
        r.word4 = self.A()
        r.arr = [r.word, r.word2, r.word3, r.word4]
        base_lrc = r._lrc

        s = set(r.arr)
        # Set borrows all 4 elements
        self.assertEqual(r._lrc, base_lrc + 4)

        # Moving into region: region owns the set, elements no longer borrowed
        # but `s` still holds an external ref
        r.set = s
        self.assertEqual(r._lrc, base_lrc + 1)

    def test_set_from_set_in_region_increases_lrc(self):
        """
        Creating a new set from a set that is already inside a region
        should borrow all elements, increasing the LRC accordingly.
        """
        r = Region()
        r.word = self.A()
        r.word2 = self.A()
        r.word3 = self.A()
        r.word4 = self.A()
        r.arr = [r.word, r.word2, r.word3, r.word4]
        s = set(r.arr)
        r.set = s
        base_lrc = r._lrc

        s2 = set(r.set)
        self.assertEqual(r._lrc, base_lrc + 4)

        s2 = None
        self.assertEqual(r._lrc, base_lrc)

    def test_set_from_dict_keys_with_object_keys(self):
        """
        Creating a set from a dict that has object keys inside a region
        should borrow those keys and increase the LRC.
        """
        r = Region()
        r.word = {self.A(): "value", self.A(): "value2"}
        base_lrc = r._lrc

        s = set(r.word)
        self.assertEqual(r._lrc, base_lrc + 2)

    def test_set_from_dict_with_frozen_string_keys(self):
        """
        Creating a set from a dict with frozen string keys should
        not affect the LRC since strings are frozen/immutable.
        """
        r = Region()
        r.word = {"key": "value", "key2": "value2"}
        base_lrc = r._lrc

        s = set(r.word)
        # Frozen string keys don't bump LRC
        self.assertEqual(r._lrc, base_lrc)

    def test_set_move_into_region_fails_if_element_in_another_region(self):
        """
        Moving a set into a region should fail if any element belongs
        to a different region, and the state should remain consistent.
        """
        r = Region()
        r2 = Region()
        r2.word4 = self.A()

        aa = self.A()
        ab = self.A()
        ac = self.A()
        arr = [aa, ab, ac, r2.word4]
        s = set(arr)

        with self.assertRaises(Exception):
            r.set = s

        # Locals should still be local after the failed move
        self.assertTrue(is_local(aa))
        self.assertTrue(is_local(ab))
        self.assertTrue(is_local(ac))


class TestRegionSetDiscard(unittest.TestCase):
    """Tests for set discard/pop operations and their effect on LRC."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_discard_decreases_lrc(self):
        """
        Discarding an element from a set should release the borrowed
        reference and decrease the LRC by 1 per discard.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.arr1 = [r.a, r.b, r.c]

        original_lrc = r._lrc
        s1 = set(r.arr1)
        base_lrc = r._lrc

        s1.discard(r.a)
        self.assertEqual(r._lrc, base_lrc - 1)

        s1.discard(r.b)
        self.assertEqual(r._lrc, base_lrc - 2)

        s1.discard(r.c)
        self.assertEqual(r._lrc, base_lrc - 3)

        self.assertEqual(r._lrc, original_lrc)

    def test_discard_nonexistent_element_does_not_change_lrc(self):
        """
        Discarding an element that is not in the set should not
        affect the LRC.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.arr1 = [r.a, r.b]

        s1 = set(r.arr1)
        base_lrc = r._lrc

        s1.discard(r.a)   # exists, LRC - 1
        self.assertEqual(r._lrc, base_lrc - 1)

        s1.discard(r.a)   # already gone, should be no change
        self.assertEqual(r._lrc, base_lrc - 1)


class TestRegionSetDifference(unittest.TestCase):
    """Tests for set difference operations and their effect on LRC."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_set_difference_does_not_increase_lrc(self):
        """
        Taking a set difference should produce a new local set.
        The LRC of the source region should not increase since
        the resulting set only contains elements not in the subtracted sets.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.arr1 = [r.a, r.b, r.c]
        r.arr2 = [r.a]
        r.arr3 = [r.b]

        original_lrc = r._lrc
        s1 = set(r.arr1)
        self.assertEqual(r._lrc, original_lrc + 3)
        s2 = set(r.arr2)
        self.assertEqual(r._lrc, original_lrc + 3 + 1)
        s3 = set(r.arr3)
        self.assertEqual(r._lrc, original_lrc + 3 + 1 + 1)
        base_lrc = r._lrc

        s4 = s1.difference(s2, s3)
        self.assertEqual(r._lrc, base_lrc + 1) # s4 borrows r.c, but not r.a or r.b

        # s4 only contains r.c, so LRC should reflect that
        self.assertTrue(is_local(s4))

    def test_set_difference_result_releases_lrc_on_none(self):
        """
        Setting the result of a difference to None should release
        any borrowed references it holds.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.arr1 = [r.a, r.b, r.c]
        r.arr2 = [r.a]
        r.arr3 = [r.b]

        s1 = set(r.arr1)
        s2 = set(r.arr2)
        s3 = set(r.arr3)

        s4 = s1.difference(s2, s3)
        base_lrc = r._lrc

        s4 = None
        self.assertLess(r._lrc, base_lrc)


class TestRegionSetSymmetricDifference(unittest.TestCase):
    """Tests for symmetric difference (XOR) operations on sets."""

    def setUp(self):
        class A: pass
        freeze(A())
        self.A = A

    def test_symmetric_difference_result_is_local(self):
        """
        The result of a symmetric difference of two sets borrowing
        from a region should be a local set.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.f = self.A()
        r.arr1 = [r.a, r.b, r.c]
        r.arr2 = [r.b, r.c, r.f]

        original_lrc = r._lrc
        s1 = set(r.arr1)
        self.assertEqual(r._lrc, original_lrc + 3)
        s2 = set(r.arr2)
        self.assertEqual(r._lrc, original_lrc + 3 + 3)
        base_lrc = r._lrc

        result = s1.symmetric_difference(s2)
        self.assertEqual(r._lrc, base_lrc + 2)
        self.assertTrue(is_local(result))

    def test_symmetric_difference_lrc_released_on_none(self):
        """
        Releasing the result of symmetric_difference should bring
        the LRC back down by the number of unique elements it held.
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.f = self.A()
        r.arr1 = [r.a, r.b, r.c]
        r.arr2 = [r.b, r.c, r.f]

        s1 = set(r.arr1)
        s2 = set(r.arr2)

        result = s1.symmetric_difference(s2)
        base_lrc = r._lrc

        result = None
        # r.a and r.f are the unique elements, so LRC should drop by 2
        self.assertEqual(r._lrc, base_lrc - 2)

    def test_symmetric_difference_operator_matches_method(self):
        """
        The `^` operator should behave identically to symmetric_difference().
        """
        r = Region()
        r.a = self.A()
        r.b = self.A()
        r.c = self.A()
        r.f = self.A()
        r.arr1 = [r.a, r.b, r.c]
        r.arr2 = [r.b, r.c, r.f]

        s1 = set(r.arr1)
        s2 = set(r.arr2)
        base_lrc = r._lrc

        result_method = s1.symmetric_difference(s2)
        result_operator = s1 ^ s2
        self.assertEqual(r._lrc, base_lrc + 2 + 2) # both should borrow the same unique elements, and there are two result, +2 from result_method and +2 from result_operator

        self.assertEqual(result_method, result_operator)


if __name__ == "__main__":
    unittest.main()