import unittest
from regions import Region, is_local
from immutable import freeze, isfrozen

class TestHardcodedMovability(unittest.TestCase):
    def test_frame_not_movable(self):
        import sys

        frame = sys._getframe()

        # Moving the frame into the region should fail, since frames have to be local
        r = Region()
        with self.assertRaises(RuntimeError) as e:
            r.frame = frame

        self.assertTrue(is_local(frame))