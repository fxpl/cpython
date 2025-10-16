import unittest
from regions import Region, is_local
import sys


class TestCleanRegion(unittest.TestCase):
    def mark_region_as_dirty(self, region: Region):
        # FIXME(regions): xFrednet: Currently all regions are marked as dirty
        # while most write barriers are missing. This will later need some
        # magic to mark the region as dirty
        self.assertTrue(region.is_dirty, "Region should be dirty here")

    def test_try_close_dirty_with_local_ref(self):
        region = Region()
        print(sys.getrefcount(region))
        self.mark_region_as_dirty(region)

        # Cleaning should succeed
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

        # The region should now be clean
        self.assertFalse(sub.is_dirty)

    def test_try_close_removes_unreachable(self):
        region = Region()
        obj = {}
        region.x = obj
        region.x = None

        # `region` should remain the owner of obj
        self.assertTrue(region.owns(obj))

        # Make the region dirty and clean it
        self.mark_region_as_dirty(region)
        region.clean()

        # Try close should have kicked `obj` from the region since it is no
        # longer reachable from the bridge object
        self.assertFalse(region.owns(obj))
        self.assertTrue(is_local(obj))
