"""Write barriers on paths that bypass the abstract layer: specialized
opcodes and type slots called directly (e.g. ``list.__setitem__``)."""
import sys
import threading
import unittest
from immutable import deep_freeze, is_deep_frozen


class TestListSlotBypass(unittest.TestCase):
    def test_unbound_setitem(self):
        lst = [1, 2, 3]
        deep_freeze(lst)
        with self.assertRaises(TypeError):
            list.__setitem__(lst, 0, 999)
        self.assertEqual(lst, [1, 2, 3])

    def test_unbound_delitem(self):
        lst = [1, 2, 3]
        deep_freeze(lst)
        with self.assertRaises(TypeError):
            list.__delitem__(lst, 0)
        self.assertEqual(lst, [1, 2, 3])

    def test_unbound_setitem_slice(self):
        lst = [1, 2, 3]
        deep_freeze(lst)
        with self.assertRaises(TypeError):
            list.__setitem__(lst, slice(0, 1), [9])
        self.assertEqual(lst, [1, 2, 3])

    def test_init_does_not_clear(self):
        lst = [1, 2, 3]
        deep_freeze(lst)
        with self.assertRaises(TypeError):
            list.__init__(lst)
        self.assertEqual(lst, [1, 2, 3])


class TestStoreSubscrSpecialization(unittest.TestCase):
    def test_specialized_store_subscr_list_int(self):
        # Specialize STORE_SUBSCR first.
        def store(seq, i, v):
            seq[i] = v

        warm = [0, 0, 0]
        for k in range(1000):
            store(warm, 0, k)

        frozen = [1, 2, 3]
        deep_freeze(frozen)
        with self.assertRaises(TypeError):
            store(frozen, 0, 999)
        self.assertEqual(frozen, [1, 2, 3])


class TestSetSlotBypass(unittest.TestCase):
    def test_inplace_or(self):
        s = {1, 2}
        deep_freeze(s)
        with self.assertRaises(TypeError):
            set.__ior__(s, {9})
        self.assertEqual(s, {1, 2})

    def test_inplace_and(self):
        s = {1, 2}
        deep_freeze(s)
        with self.assertRaises(TypeError):
            set.__iand__(s, {1})
        self.assertEqual(s, {1, 2})

    def test_inplace_sub(self):
        s = {1, 2}
        deep_freeze(s)
        with self.assertRaises(TypeError):
            set.__isub__(s, {1})
        self.assertEqual(s, {1, 2})

    def test_inplace_xor(self):
        s = {1, 2}
        deep_freeze(s)
        with self.assertRaises(TypeError):
            set.__ixor__(s, {2, 3})
        self.assertEqual(s, {1, 2})


class TestByteArrayBarriers(unittest.TestCase):
    def _frozen(self):
        ba = bytearray(b"abc")
        deep_freeze(ba)
        return ba

    def test_unbound_setitem(self):
        ba = self._frozen()
        with self.assertRaises(TypeError):
            bytearray.__setitem__(ba, 0, ord("z"))
        self.assertEqual(bytes(ba), b"abc")

    def test_unbound_delitem(self):
        ba = self._frozen()
        with self.assertRaises(TypeError):
            bytearray.__delitem__(ba, 0)
        self.assertEqual(bytes(ba), b"abc")

    def test_inplace_concat(self):
        ba = self._frozen()
        with self.assertRaises(TypeError):
            bytearray.__iadd__(ba, b"z")
        self.assertEqual(bytes(ba), b"abc")

    def test_append(self):
        ba = self._frozen()
        with self.assertRaises(TypeError):
            ba.append(ord("z"))
        self.assertEqual(bytes(ba), b"abc")

    def test_extend(self):
        ba = self._frozen()
        with self.assertRaises(TypeError):
            ba.extend(b"z")
        self.assertEqual(bytes(ba), b"abc")

    def test_insert(self):
        ba = self._frozen()
        with self.assertRaises(TypeError):
            ba.insert(0, ord("z"))
        self.assertEqual(bytes(ba), b"abc")

    def test_pop(self):
        ba = self._frozen()
        with self.assertRaises(TypeError):
            ba.pop()
        self.assertEqual(bytes(ba), b"abc")

    def test_remove(self):
        ba = self._frozen()
        with self.assertRaises(TypeError):
            ba.remove(ord("a"))
        self.assertEqual(bytes(ba), b"abc")

    def test_reverse(self):
        ba = self._frozen()
        with self.assertRaises(TypeError):
            ba.reverse()
        self.assertEqual(bytes(ba), b"abc")

    def test_clear(self):
        ba = self._frozen()
        with self.assertRaises(TypeError):
            ba.clear()
        self.assertEqual(bytes(ba), b"abc")

    def test_setslice(self):
        ba = self._frozen()
        with self.assertRaises(TypeError):
            ba[0:1] = b"zz"
        self.assertEqual(bytes(ba), b"abc")


class TestDictMergeFastPath(unittest.TestCase):
    # An empty destination dict takes dict_dict_merge's clone fast path.
    def test_ior_empty_frozen(self):
        d = {}
        deep_freeze(d)
        with self.assertRaises(TypeError):
            d |= {1: 2}
        self.assertEqual(d, {})

    def test_ior_nonempty_frozen(self):
        d = {0: 0}
        deep_freeze(d)
        with self.assertRaises(TypeError):
            d |= {1: 2}
        self.assertEqual(d, {0: 0})


class TestMemoryViewBarrier(unittest.TestCase):
    def test_memoryview_write_blocked(self):
        ba = bytearray(b"abc")
        deep_freeze(ba)
        # Whether the view was created before or after freezing.
        with self.assertRaises((TypeError, ValueError)):
            mv = memoryview(ba)
            mv[0] = ord("z")
        self.assertEqual(bytes(ba), b"abc")


class TestStoreAttrSpecialization(unittest.TestCase):
    # Warm up STORE_ATTR on a mutable instance, then store to a frozen one.
    def _check(self, cls, init):
        def store(o, v):
            o.a = v

        frozen = cls()
        init(frozen)
        deep_freeze(frozen)
        warm = cls()
        init(warm)
        for k in range(1000):
            store(warm, k)

        with self.assertRaisesRegex(TypeError, "is immutable"):
            store(frozen, 999)
        self.assertEqual(frozen.a, 1)
        return frozen

    def test_instance_value(self):
        class C:
            pass
        def init(o):
            o.a = 1
        self._check(C, init)

    def test_with_hint(self):
        class C:
            pass
        def init(o):
            # Enough attributes to give up on inline values.
            for i in range(40):
                setattr(o, f"_{i}", None)
            o.a = 1
        self._check(C, init)

    def test_slot(self):
        class C:
            __slots__ = ('a',)
        def init(o):
            o.a = 1
        self._check(C, init)

    def test_slot_releases_lock(self):
        class L(list):
            __slots__ = ('a',)
        def init(o):
            o.a = 1
        frozen = self._check(L, init)
        # list.copy() locks the object; it must not have been left locked.
        t = threading.Thread(target=frozen.copy, daemon=True)
        t.start()
        t.join(timeout=10)
        self.assertFalse(t.is_alive())


class TestFrameLocalsCell(unittest.TestCase):
    def test_write_to_frozen_cell(self):
        def make():
            x = 1
            def inner():
                sys._getframe().f_locals['x'] = 5
                return x
            return inner
        f = make()
        deep_freeze(f)
        with self.assertRaisesRegex(TypeError, "is immutable"):
            f()


GLOBAL_TO_DELETE = 1


class TestDeleteGlobal(unittest.TestCase):
    def test_delete_global_in_frozen_function(self):
        def delete():
            global GLOBAL_TO_DELETE
            del GLOBAL_TO_DELETE
        deep_freeze(delete)
        with self.assertRaisesRegex(TypeError, "is immutable"):
            delete()


class TestListAppendSpecialization(unittest.TestCase):
    def test_specialized_call_list_append(self):
        def append(seq, v):
            seq.append(v)

        warm = []
        for k in range(1000):
            append(warm, k)
        import dis
        self.assertIn("CALL_LIST_APPEND",
                      [i.opname for i in dis.get_instructions(append, adaptive=True)])

        frozen = [1, 2]
        deep_freeze(frozen)
        with self.assertRaisesRegex(TypeError, "is immutable"):
            append(frozen, 3)
        self.assertEqual(frozen, [1, 2])


class TestListBarriers(unittest.TestCase):
    def setUp(self):
        self.lst = [3, 1, 2]
        deep_freeze(self.lst)

    def tearDown(self):
        self.assertEqual(self.lst, [3, 1, 2])

    def test_extend_iterables(self):
        for it in ((x for x in [1]), {1}, frozenset({1}), {1: 2},
                   {1: 2}.keys(), {1: 2}.values(), {1: 2}.items()):
            with self.subTest(it=type(it)):
                with self.assertRaisesRegex(TypeError, "is immutable"):
                    self.lst.extend(it)

    def test_inplace_repeat_zero(self):
        with self.assertRaisesRegex(TypeError, "is immutable"):
            self.lst *= 0

    def test_heapq(self):
        import heapq
        for f in (lambda: heapq.heappush(self.lst, 0),
                  lambda: heapq.heappop(self.lst),
                  lambda: heapq.heapify(self.lst),
                  lambda: heapq.heapreplace(self.lst, 0),
                  lambda: heapq.heappushpop(self.lst, 5),
                  lambda: heapq.heappush_max(self.lst, 0),
                  lambda: heapq.heappop_max(self.lst),
                  lambda: heapq.heapify_max(self.lst),
                  lambda: heapq.heapreplace_max(self.lst, 0),
                  lambda: heapq.heappushpop_max(self.lst, 0)):
            with self.assertRaisesRegex(TypeError, "is immutable"):
                f()


class TestSetMethodBarriers(unittest.TestCase):
    def setUp(self):
        self.s = {1, 2}
        deep_freeze(self.s)

    def tearDown(self):
        self.assertEqual(self.s, {1, 2})

    def test_update_methods(self):
        for f in (self.s.difference_update, self.s.intersection_update,
                  self.s.symmetric_difference_update):
            with self.subTest(f=f.__name__):
                with self.assertRaisesRegex(TypeError, "is immutable"):
                    f({1})

    def test_init(self):
        with self.assertRaisesRegex(TypeError, "is immutable"):
            self.s.__init__()
        with self.assertRaisesRegex(TypeError, "is immutable"):
            self.s.__init__([3])


class TestDescriptorBarriers(unittest.TestCase):
    def test_function_getset(self):
        import types
        def f(a=1, *, b=2):
            pass
        def g():
            pass
        deep_freeze(f)
        values = {"__code__": g.__code__, "__defaults__": (5,),
                  "__kwdefaults__": {}, "__name__": "x", "__qualname__": "x",
                  "__doc__": "x", "__type_params__": (), "__module__": "x"}
        for name, value in values.items():
            with self.subTest(name=name):
                with self.assertRaisesRegex(TypeError, "is immutable"):
                    types.FunctionType.__dict__[name].__set__(f, value)
        self.assertEqual(f.__name__, "f")
        self.assertEqual(f.__defaults__, (1,))

    def test_member_descriptor(self):
        class S:
            __slots__ = ("a",)
        o = S()
        o.a = 1
        deep_freeze(o)
        with self.assertRaisesRegex(TypeError, "is immutable"):
            S.__dict__["a"].__set__(o, 2)
        with self.assertRaisesRegex(TypeError, "is immutable"):
            S.__dict__["a"].__delete__(o)
        self.assertEqual(o.a, 1)

    def test_property(self):
        p = property(lambda self: 1)
        deep_freeze(p)
        with self.assertRaisesRegex(TypeError, "is immutable"):
            p.__init__(None)
        with self.assertRaisesRegex(TypeError, "is immutable"):
            p.__set_name__(object, "other")
        self.assertIsNotNone(p.fget)
        self.assertEqual(p.__name__, "<lambda>")

    def test_frozen_property_reused_under_same_name(self):
        class A:
            @property
            def p(self):
                return 1
        deep_freeze(A)
        class B:
            p = A.p
        self.assertEqual(B().p, 1)

    def test_exception(self):
        e = ValueError(1)
        deep_freeze(e)
        with self.assertRaisesRegex(TypeError, "is immutable"):
            e.__init__(2)
        with self.assertRaisesRegex(TypeError, "is immutable"):
            e.with_traceback(None)
        self.assertEqual(e.args, (1,))

    def test_memoryview_release(self):
        m = memoryview(b"abc")
        deep_freeze(m)
        with self.assertRaisesRegex(TypeError, "is immutable"):
            m.release()
        self.assertEqual(m.tobytes(), b"abc")


class TestBufferExports(unittest.TestCase):
    def test_frozen_bytearray_exports_readonly(self):
        import io
        ba = bytearray(b"abc")
        deep_freeze(ba)
        self.assertTrue(memoryview(ba).readonly)
        with self.assertRaises(TypeError):
            io.BytesIO(b"zzz").readinto(ba)
        self.assertEqual(ba, b"abc")
        # Read-only operations that temporarily export the buffer still work.
        self.assertEqual(ba.find(b"b"), 1)
        self.assertEqual(ba.count(b"b"), 1)
        self.assertEqual(ba.split(), [b"abc"])

    def test_cannot_freeze_bytearray_with_exports(self):
        class B(bytearray):
            pass
        for ba in (bytearray(b"x"), B(b"x")):
            with self.subTest(type=type(ba)):
                m = memoryview(ba)
                with self.assertRaises(BufferError):
                    deep_freeze(ba)
                m.release()
                deep_freeze(ba)

    def test_cannot_freeze_writable_memoryview(self):
        with self.assertRaises(BufferError):
            deep_freeze(memoryview(bytearray(b"x")))
        deep_freeze(memoryview(b"x"))


class TestFrozenBaseSubclasses(unittest.TestCase):
    def test_subclass_of_frozen_base_not_recorded(self):
        # The subclass dict of a frozen (possibly shared) base is not written.
        class Base:
            pass
        class Before(Base):
            pass
        deep_freeze(Base)
        class After(Base):
            pass
        self.assertEqual(Base.__subclasses__(), [Before])
        self.assertTrue(issubclass(After, Base))
        self.assertIsInstance(After(), Base)


class TestResultTupleRecycling(unittest.TestCase):
    # Iterators reuse their result tuple when uniquely referenced. A reused
    # frozen tuple holds new (mutable) items, so it must not stay frozen.
    def check(self, it):
        first = next(it)
        deep_freeze(first)
        values = tuple(first)
        del first
        second = next(it)
        self.assertNotEqual(tuple(second), values)
        return second

    def test_iterators_with_fresh_items(self):
        import collections
        import itertools
        cases = {
            "zip": lambda: zip([1, 2], [[], []]),
            "enumerate": lambda: enumerate([[], []]),
            "dict.items": lambda: iter({1: [], 2: []}.items()),
            "reversed dict.items": lambda: reversed({1: [], 2: []}.items()),
            "OrderedDict.items": lambda: iter(
                collections.OrderedDict({1: [], 2: []}).items()),
            "zip_longest": lambda: itertools.zip_longest([1, 2], [[], []]),
        }
        for name, make in cases.items():
            with self.subTest(name):
                second = self.check(make())
                self.assertFalse(is_deep_frozen(second))

    def test_iterators_with_pooled_items(self):
        # Items come from a pool, so the frozen items can show up again.
        import itertools
        cases = {
            "product": lambda: itertools.product([1, 2], [3]),
            "combinations": lambda: itertools.combinations([1, 2, 3], 2),
            "permutations": lambda: itertools.permutations([1, 2]),
            "combinations_with_replacement":
                lambda: itertools.combinations_with_replacement([1, 2], 2),
            "pairwise": lambda: itertools.pairwise([1, 2, 3]),
        }
        for name, make in cases.items():
            with self.subTest(name):
                self.check(make())

    def test_frozen_tuple_is_reused(self):
        it = zip([1, 2], [[], []])
        first = next(it)
        deep_freeze(first)
        first_id = id(first)
        del first
        self.assertEqual(id(next(it)), first_id)


if __name__ == "__main__":
    unittest.main()
