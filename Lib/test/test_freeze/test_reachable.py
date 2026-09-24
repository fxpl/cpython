import unittest
import sys
from immutable import deep_freeze, is_deep_frozen

class BaseObjectTest(unittest.TestCase):
    def test_correct_traverse_visit_once(self):
        from _test_reachable import HasTraverseNoReachableHeap

        # Make sure this is a fresh import
        self.assertFalse(is_deep_frozen(HasTraverseNoReachableHeap))

        HasTraverseNoReachableHeap.a = HasTraverseNoReachableHeap()
        HasTraverseNoReachableHeap.b = HasTraverseNoReachableHeap()
        HasTraverseNoReachableHeap.c = HasTraverseNoReachableHeap()
        # This used to crash, if it doesn't we're probably fine
        deep_freeze(HasTraverseNoReachableHeap)

        # Unimport the module
        sys.modules.pop("_test_reachable", None)
        sys.mut_modules.pop("_test_reachable", None)

    def test_traverse_misses_type(self):
        from _test_reachable import IncorrectTraverseNoReachableHeap

        # Make sure this is a fresh import
        self.assertFalse(is_deep_frozen(IncorrectTraverseNoReachableHeap))

        # Freezing an instance should also freeze the type, even if
        # the traverse forgot to visit the type
        deep_freeze(IncorrectTraverseNoReachableHeap())
        self.assertTrue(is_deep_frozen(IncorrectTraverseNoReachableHeap))

        # Unimport the module
        sys.modules.pop("_test_reachable", None)
        sys.mut_modules.pop("_test_reachable", None)

if __name__ == '__main__':
    unittest.main()
