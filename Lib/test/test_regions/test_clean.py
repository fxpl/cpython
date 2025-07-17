import unittest
from regions import Region, is_local
import sys


class TestCleanRegion(unittest.TestCase):
    def mark_region_as_dirty(self, region: Region):
        region._make_dirty()

        self.assertTrue(region.is_dirty, "Region should be dirty here")

    def test_try_close_dirty_with_local_ref(self):
        region = Region()

        self.mark_region_as_dirty(region)
        region.clean()

        self.assertFalse(region.is_dirty)

    def test_try_close_sub_region(self):
        region = Region()
        region.sub = Region()
        self.mark_region_as_dirty(region)
        self.mark_region_as_dirty(region.sub)

        sub = region.sub

        # Cleaning a dirty parent region should clean the child as well
        region.clean()

        # The regions should now be clean
        self.assertFalse(sub.is_dirty)
        self.assertEqual(sub._lrc, 1, "The local should be the only known reference")
        self.assertEqual(sub._osc, 0, "No subregions should be present")
        self.assertEqual(region._osc, 1)

        # Removing the reference into sub, should close it and inform the parent
        sub = None
        self.assertEqual(region._osc, 0)


    def test_clean_removes_unreachable(self):
        region = Region()
        obj = {}
        region.x = obj
        region.x = None

        # `region` should remain the owner of obj
        self.assertTrue(region.owns(obj))

        # Make the region dirty and clean it
        self.mark_region_as_dirty(region)
        region.clean()

        # Clean should have kicked `obj` from the region since it is no
        # longer reachable from the bridge object
        self.assertFalse(region.owns(obj))
        self.assertTrue(is_local(obj))

    def test_clean_keeps_name(self):
        region = Region("Marlin")

        self.mark_region_as_dirty(region)
        region.clean()

        self.assertEqual(region.name, "Marlin")
