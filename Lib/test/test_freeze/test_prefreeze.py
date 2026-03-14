import unittest

from immutable import NotFreezable, freeze, isfrozen


class TestPreFreezeHook(unittest.TestCase):
    def test_prefreeze_hook_is_called(self):
        class C:
            def __init__(self):
                self.hook_calls = 0

            def __pre_freeze__(self):
                self.hook_calls += 1

        obj = C()
        freeze(obj)

        self.assertEqual(obj.hook_calls, 1)
        self.assertTrue(isfrozen(obj))

    def test_prefreeze_hook_runs_before_object_is_frozen(self):
        class C:
            def __init__(self):
                self.was_frozen_inside_hook = None

            def __pre_freeze__(self):
                self.was_frozen_inside_hook = isfrozen(obj)

        obj = C()
        freeze(obj)

        self.assertIs(obj.was_frozen_inside_hook, False)
        self.assertTrue(isfrozen(obj))

    def test_prefreeze_hook_remains_called_after_failure(self):
        class C:
            def __init__(self):
                self.hook_calls = 0
                self.child = NotFreezable()

            def __pre_freeze__(self):
                self.hook_calls += 1

        obj = C()

        with self.assertRaises(TypeError):
            freeze(obj)
        with self.assertRaises(TypeError):
            freeze(obj)
        with self.assertRaises(TypeError):
            freeze(obj)

        self.assertEqual(obj.hook_calls, 1)
        self.assertFalse(isfrozen(obj))


    def test_nested_pre_freeze(self):
        class A:
            def __init__(self, field):
                self.field = field

            def __pre_freeze__(self):
                freeze(self.field)

        freeze(A)

        a = A(None)

        # Freezing A should succeed even with nested `freeze()` calls
        # freeze(a)

        # Objects frozen by nested freeze calls should remain frozen
        self.assertTrue(isfrozen(a))
        self.assertTrue(isfrozen(a.field))

if __name__ == "__main__":
    unittest.main()
