"""PEP 442: tp_finalize runs at most once, freezing included.

The SCC bookkeeping shares the PyGC_Head prev word with the GC's
_PyGC_PREV_MASK_FINALIZED bit, so every write to it has to keep the bit.
"""

import gc
import sys
import unittest

from immutable import deep_freeze


# The recorder lives on `sys` because module contents are hidden from the
# freeze walk. A list in this module's globals would be reached through
# Resurrecting.__del__.__globals__ and frozen, silently swallowing the
# second __del__ these tests are looking for.
class Resurrecting:
    def __del__(self):
        sys._freeze_finalize_calls += 1
        sys._freeze_finalize_box.append(self)


class Plain:
    pass


class TestFinalizeAtMostOnce(unittest.TestCase):

    def setUp(self):
        sys._freeze_finalize_calls = 0
        sys._freeze_finalize_box = []

    def tearDown(self):
        del sys._freeze_finalize_calls
        del sys._freeze_finalize_box

    def test_recorder_survives_freeze(self):
        """Guard the premise: freezing must not lock the recorder."""
        obj = Resurrecting()
        del obj
        gc.collect()
        deep_freeze(sys._freeze_finalize_box.pop())
        sys._freeze_finalize_box.append(None)
        sys._freeze_finalize_calls += 1
        self.assertEqual(sys._freeze_finalize_calls, 2)

    def test_single_object(self):
        obj = Resurrecting()
        del obj
        gc.collect()
        self.assertEqual(sys._freeze_finalize_calls, 1)

        obj = sys._freeze_finalize_box.pop()
        deep_freeze(obj)
        del obj
        gc.collect()
        self.assertEqual(sys._freeze_finalize_calls, 1)

    def test_scc(self):
        """The cycle becomes an SCC, so scc_unfreeze_and_finalize() decides."""
        obj = Resurrecting()
        obj.other = Plain()
        obj.other.back = obj
        del obj
        gc.collect()
        self.assertEqual(sys._freeze_finalize_calls, 1)

        obj = sys._freeze_finalize_box.pop()
        deep_freeze(obj)
        del obj
        gc.collect()
        self.assertEqual(sys._freeze_finalize_calls, 1)

    def test_never_finalized_object_still_runs_once(self):
        obj = Resurrecting()
        deep_freeze(obj)
        del obj
        gc.collect()
        self.assertEqual(sys._freeze_finalize_calls, 1)

        sys._freeze_finalize_box.clear()
        gc.collect()
        self.assertEqual(sys._freeze_finalize_calls, 1)


if __name__ == '__main__':
    unittest.main()
