import queue
import unittest
from test import support
from test.support import import_helper

boc = import_helper.import_module("concurrent.boc")
Cown = boc.Cown
when = boc.when
interpreters = import_helper.import_module("concurrent.interpreters")
create_queue = interpreters.create_queue


class Tests(unittest.TestCase):
    def test_basic_when_block(self):
        pool_info = boc.interpreter_pool_info()
        self.assertGreaterEqual(pool_info.n_live, 0)
        self.assertGreater(pool_info.n_max_interpreters, 0)
        self.assertLessEqual(pool_info.n_live, pool_info.n_max_interpreters)

        cown = Cown(42)

        @when(cown)
        def foo(cown):
            cown.val *= 2
            return cown.val + 3

        pool_info = boc.interpreter_pool_info()
        self.assertGreater(pool_info.n_live, 0)
        self.assertEqual(pool_info.n_idle + pool_info.n_busy, pool_info.n_live)
        self.assertLessEqual(pool_info.n_live, pool_info.n_max_interpreters)

        self.assertEqual(boc.block_on_cown(cown), 84)
        self.assertEqual(boc.block_on_cown(foo), 87)

    def test_many_cowns(self):
        n = 100
        cowns = [Cown(i) for i in range(n)]

        @when(*cowns)
        def _(*cowns):
            import itertools

            for x, y in itertools.pairwise(cowns):
                y.val += x.val

        cown_vals = boc.block_on_cowns(*cowns)
        self.assertEqual(list(cown_vals), [i * (i + 1) / 2 for i in range(n)])

    def test_simultaneous_cowns(self):
        p = create_queue()
        q = create_queue()
        pcown = Cown(p)
        qcown = Cown(q)

        @when(pcown, qcown)
        def _(p, q):
            p.val.put(1)
            q.val.put(2)

        @when(pcown, qcown)
        def _(p, q):
            p.val.put(3)
            q.val.put(4)

        self.assertEqual(1, p.get())
        self.assertEqual(2, q.get())
        self.assertEqual(3, p.get())
        self.assertEqual(4, q.get())

    def test_many_when_blocks_on_cown(self):
        n = 100

        q1 = create_queue()
        q2 = create_queue()
        q1_cown = Cown(q1)
        q2_cown = Cown(q2)

        for i in range(n):
            icown = Cown(i)
            jcown = Cown(i * 2)

            @when(
                icown,
                q2_cown,
                jcown,
                q1_cown,
            )
            def _(i, q2, j, q1):
                q1.val.put(i.val)
                q2.val.put(j.val)

            pool_info = boc.interpreter_pool_info()
            self.assertGreater(pool_info.n_live, 0)
            self.assertEqual(
                pool_info.n_live, pool_info.n_idle + pool_info.n_busy
            )
            self.assertLessEqual(
                pool_info.n_live, pool_info.n_max_interpreters
            )

        for i in range(n):
            self.assertEqual(i, q1.get(timeout=support.SHORT_TIMEOUT))
            self.assertEqual(i * 2, q2.get(timeout=support.SHORT_TIMEOUT))
        with self.assertRaises(queue.Empty):
            q1.get_nowait()
            q2.get_nowait()
