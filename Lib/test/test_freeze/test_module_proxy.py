import os
import sys
import unittest
from test.support import import_helper, os_helper
from test.support.script_helper import assert_python_ok

from immutable import deep_freeze, is_deep_frozen, ImmutableModule

class TestModuleProxy(unittest.TestCase):
    def setUp(self):
        sys.modules.pop("random", None)
        sys.mut_modules.pop("random", None)

    def test_freeze_function_with_random_module_creates_proxy(self):
        import random
        self.assertFalse(is_deep_frozen(random))

        def coin():
            return random.random()

        deep_freeze(coin)

        captured_random = coin.__closure__[0].cell_contents
        self.assertTrue(is_deep_frozen(captured_random))
        self.assertTrue(is_deep_frozen(random))
        self.assertIsInstance(captured_random, ImmutableModule)
        self.assertIsInstance(random, ImmutableModule)

        self.assertIn("random", sys.mut_modules)
        mut_random = sys.mut_modules["random"]
        self.assertIsNot(mut_random, captured_random)
        self.assertFalse(is_deep_frozen(mut_random))
        self.assertIsInstance(mut_random, sys.__class__)

    def test_random_state_remains_mutable_via_proxy(self):
        import random
        self.assertFalse(is_deep_frozen(random))

        def coin():
            return random.random()

        deep_freeze(coin)

        random.seed(42)
        first = coin()
        second = coin()

        random.seed(42)
        self.assertEqual(first, coin())
        self.assertEqual(second, coin())

    def test_proxy_attribute_writes_delegate_to_mutable_module(self):
        import random
        self.assertFalse(is_deep_frozen(random))

        def coin():
            return random.random()

        deep_freeze(coin)

        proxy_random = coin.__closure__[0].cell_contents
        mut_random = sys.mut_modules["random"]
        attr = "_freeze_proxy_canary"

        had_original = hasattr(mut_random, attr)
        original = getattr(mut_random, attr, None)
        try:
            proxy_random._freeze_proxy_canary = 42
            self.assertEqual(mut_random._freeze_proxy_canary, 42)
        finally:
            if had_original:
                setattr(mut_random, attr, original)
            elif hasattr(mut_random, attr):
                delattr(mut_random, attr)

    def test_proxy_reimport(self):
        import random
        self.assertFalse(is_deep_frozen(random))

        deep_freeze(random)

        # Unimport "random"
        old_mut_random = sys.mut_modules["random"]
        del sys.mut_modules["random"]

        # This should reimport the module
        random.random()

    def _make_once_module(self, name):
        # A module that can only be imported once.
        d = self.enterContext(os_helper.temp_dir())
        with open(os.path.join(d, f"{name}.py"), "w") as f:
            f.write("import sys\n"
                    f"if getattr(sys, '_{name}_once', False):\n"
                    "    raise ImportError('reimport fails')\n"
                    f"sys._{name}_once = True\n"
                    "x = 1\n")
        self.enterContext(import_helper.DirsOnSysPath(d))
        self.addCleanup(import_helper.unload, name)
        self.addCleanup(sys.mut_modules.pop, name, None)
        self.addCleanup(lambda: sys.__dict__.pop(f"_{name}_once", None))
        return __import__(name)

    def test_load_attr_failed_reimport(self):
        mod = self._make_once_module("_freeze_c9_once")
        deep_freeze(mod)
        del sys.mut_modules[mod.__name__]

        def f():
            return mod.x

        # Specializing LOAD_ATTR used to dereference NULL here.
        for _ in range(20):
            with self.assertRaisesRegex(ImportError, "reimport fails"):
                f()

    def test_load_attr_module_after_unimport(self):
        mod = self._make_once_module("_freeze_c9_unimport")
        deep_freeze(mod)

        def f():
            return mod.x

        for _ in range(20):
            self.assertEqual(f(), 1)
        del sys.mut_modules[mod.__name__]
        # The specialized LOAD_ATTR_MODULE must deopt instead of
        # swallowing the ImportError.
        for _ in range(20):
            with self.assertRaisesRegex(ImportError, "reimport fails"):
                f()

    def test_failed_reimport_does_not_leak_sys_modules(self):
        mod = self._make_once_module("_freeze_r5_leak")
        deep_freeze(mod)
        del sys.mut_modules[mod.__name__]

        def f():
            return mod.x

        def attempt():
            with self.assertRaisesRegex(ImportError, "reimport fails"):
                f()

        attempt()
        before = sys.getrefcount(sys.modules)
        for _ in range(20):
            attempt()
        self.assertEqual(sys.getrefcount(sys.modules), before)

    def test_import_after_freezing_sys(self):
        # importlib must not derive the module type from `type(sys)`.
        code = """if 1:
            import sys, types, _frozen_importlib_external
            import _thread, _warnings, _weakref
            from immutable import deep_freeze
            for m in (sys, _frozen_importlib_external,
                      _thread, _warnings, _weakref):
                deep_freeze(m)
            import json
            assert type(json) is types.ModuleType, type(json)
        """
        assert_python_ok("-c", code)

if __name__ == '__main__':
    unittest.main()
