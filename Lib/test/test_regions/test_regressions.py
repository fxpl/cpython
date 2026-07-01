import unittest
from regions import Region
from immutable import freeze

class TestCrashesObject(unittest.TestCase):

    def ignore_test_exception_and_crash_on_freeze(self):
        r = Region()
        class A:
            def foo(self):
                r
        freeze(A)

    def test_barrier_in_optimized_opcode(self):
        class A: pass

        def build_region():
            r = Region()
            r.a = A()
            r.a.b = A()
            r.a = None
            return r

        r1 = build_region()
        # This second call will optimize the function to use
        # `_STORE_ATTR_INSTANCE_VALUE` bytecode that can't
        # handle region barriers correctly. We therefore need
        # to de-opt again if the objects are in separate regions
        # like in the example above.
        r2 = build_region()
        r3 = build_region()
