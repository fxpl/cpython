import unittest
from regions import Cown, Region, is_local
from immutable import freeze
import threading

class TestBasicCownObject(unittest.TestCase):
    def test_valid_cown_construction(self):
        # No argument, is a good argument
        c = Cown()
        self.assertIsNone(c.value)

        # Immortal builtin object
        c = Cown(None)
        self.assertIsNone(c.value)

        # Frozen object
        x = {}
        freeze(x)
        c = Cown(x)
        self.assertEqual(c.value, x)

        # Cowns are allowed
        c = Cown()
        c1 = Cown(c)
        c2 = Cown(c)

        # Regions are allowed
        r = Region(name="dummy")
        Cown(r)
        Cown(Region(name="new region"))

    def test_cown_construction_error_for_local(self):
        x = {}
        self.assertTrue(is_local(x))

        # Local arguments are forbidden
        with self.assertRaises(RuntimeError) as e:
            Cown(x)

    def test_cown_construction_error_for_owned(self):
        r = Region()
        x = {}
        r.x = x
        self.assertTrue(r.owns(x))

        # Owned arguments are forbidden
        with self.assertRaises(RuntimeError) as e:
            Cown(x)

class TestCownValueField(unittest.TestCase):
    def test_cown_valid_value_fields(self):
        # No argument, is a good argument
        c = Cown()
        self.assertIsNone(c.value)

        # Immortal builtin object
        c.value = None
        self.assertIsNone(c.value)

        # Frozen object
        x = {}
        freeze(x)
        c.value = x
        self.assertEqual(c.value, x)

        # Cowns are allowed
        c1 = Cown(c)
        c.value = c1
        self.assertEqual(c.value, c1)

        # Regions are allowed
        r = Region(name="dummy")
        c.value = r
        self.assertEqual(c.value, r)

    def test_cown_value_error_for_local(self):
        x = {}
        self.assertTrue(is_local(x))
        c = Cown()

        # Local values are forbidden
        with self.assertRaises(RuntimeError) as e:
            c.value = x

    def test_cown_construction_error_for_owned(self):
        r = Region()
        x = {}
        r.x = x
        self.assertTrue(r.owns(x))

        c = Cown()
        # Owned values are forbidden
        with self.assertRaises(RuntimeError) as e:
            c.value = x

class TestCownLocking(unittest.TestCase):
    def test_release_and_reacquire(self):
        c = Cown()
        self.assertTrue(c.locked())
        self.assertTrue(c.owned())
        self.assertTrue(c.owned_by_thread())

        c.release()
        self.assertFalse(c.locked())
        self.assertFalse(c.owned())
        self.assertFalse(c.owned_by_thread())

        c.acquire()
        self.assertTrue(c.locked())
        self.assertTrue(c.owned())
        self.assertTrue(c.owned_by_thread())

    def test_blocking_with_timeout(self):
        c = Cown()
        self.assertTrue(c.owned())

        # Blocking in the owning thread is allowed
        # It should block until the cown is released or the timeout is over
        self.assertFalse(c.acquire(timeout=0.1))
        self.assertFalse(c.acquire(blocking=False))

    def test_blocking_with_no_timeout(self):
        # Setup
        freeze(True)
        freeze(False)

        # The cown in question
        c = Cown(False)
        self.assertTrue(c.owned())

        def other_thread():
            # Check that this runs in a different thread
            self.assertTrue(c.owned())
            self.assertFalse(c.owned_by_thread())

            # Set the cown value to check ordering
            c.value = True
            c.release()

        # Start another thread
        t2 = threading.Thread(target=other_thread)
        t2.start()

        # The acquire should block until t2 releases the cown
        self.assertTrue(c.acquire())
        self.assertTrue(c.value, "This should be true if the ordering is correct")

        # Cleanup
        t2.join()

    def test_release_fails_for_open_regions(self):
        r = Region()
        c = Cown(r)

        # Releasing a cown with an open region should error
        with self.assertRaises(RuntimeError):
            c.release()

        r = None

        # This release should succeed, since `r` should be closed
        c.release()

    def test_release_cleans_region(self):
        c = Cown(Region())
        c.value._make_dirty()

        self.assertTrue(c.value.is_dirty)

        # Releasing the cown should clean the region first in an
        # attempt to close it
        with self.assertRaises(RuntimeError):
            c.release()
