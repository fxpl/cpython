"""Frozen cycles must be freed regardless of tp_traverse child order."""
import gc
import unittest
import weakref

from immutable import deep_freeze


class N:
    __slots__ = ("a", "b", "__weakref__")


def build(order):
    root, w = N(), N()
    root.a = w
    # Slot order decides which edge the traversal sees first.
    for slot, target in zip("ab", [w, root] if order == 0 else [root, w]):
        setattr(w, slot, target)
    return root, w


class TestSccInteriorEdges(unittest.TestCase):
    def check_freed(self, order):
        root, w = build(order)
        deep_freeze(root)
        refs = [weakref.ref(root), weakref.ref(w)]
        del root, w
        gc.collect()
        self.assertEqual([r() for r in refs], [None, None])

    def test_self_loop_then_back_edge(self):
        self.check_freed(0)

    def test_back_edge_then_self_loop(self):
        self.check_freed(1)


if __name__ == "__main__":
    unittest.main()
