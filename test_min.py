import unittest
from immutable import freeze, NotFreezable, isfrozen

class TestDictMutation(unittest.TestCase):
    class C:
        def __init__(self):
            self.x = 0

        def set(self, x):
            d = self.__dict__
            d['x'] = x

    def test_dict_mutation(self):
        obj = TestDictMutation.C()
        d = obj.set(1)
#        self.assertEqual(obj.get(), 1)
        freeze(obj)
        # self.assertEqual(obj.get(), 1)
        # self.assertTrue(isfrozen(obj))
        self.assertRaises(TypeError, obj.set, 1)

    def test_dict_mutation2(self):
        obj = TestDictMutation.C()
        freeze(obj)
        # self.assertTrue(isfrozen(obj))
        # self.assertRaises(TypeError, obj.set, 1)
        # self.assertEqual(obj.get(), 0)


if __name__ == '__main__':
    unittest.main()
