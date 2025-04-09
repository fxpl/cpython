from collections import deque

from . import BaseObjectTest

class TestDeque(BaseObjectTest):
    class C:
        pass

    def __init__(self, *args, **kwargs):
        obj = deque([self.C(), self.C(), 1, "two", None])
        BaseObjectTest.__init__(self, *args, obj=obj, **kwargs)

    def test_set_item(self):
        with self.assertRaises(NotWriteableError):
            self.obj[0] = None

    def test_append(self):
        with self.assertRaises(NotWriteableError):
            self.obj.append(TestDeque.C())

    def test_appendleft(self):
        with self.assertRaises(NotWriteableError):
            self.obj.appendleft(TestDeque.C())

    def test_extend(self):
        with self.assertRaises(NotWriteableError):
            self.obj.extend([TestDeque.C()])

    def test_extendleft(self):
        with self.assertRaises(NotWriteableError):
            self.obj.extendleft([TestDeque.C()])

    def test_insert(self):
        with self.assertRaises(NotWriteableError):
            self.obj.insert(0, TestDeque.C())

    def test_pop(self):
        with self.assertRaises(NotWriteableError):
            self.obj.pop()

    def test_popleft(self):
        with self.assertRaises(NotWriteableError):
            self.obj.popleft()

    def test_remove(self):
        with self.assertRaises(NotWriteableError):
            self.obj.remove(1)

    def test_delete(self):
        with self.assertRaises(NotWriteableError):
            del self.obj[0]

    def test_inplace_repeat(self):
        with self.assertRaises(NotWriteableError):
            self.obj *= 2

    def test_inplace_concat(self):
        with self.assertRaises(NotWriteableError):
            self.obj += [TestDeque.C()]

    def test_reverse(self):
        with self.assertRaises(NotWriteableError):
            self.obj.reverse()

    def test_rotate(self):
        with self.assertRaises(NotWriteableError):
            self.obj.rotate(1)

    def test_clear(self):
        with self.assertRaises(NotWriteableError):
            self.obj.clear()
