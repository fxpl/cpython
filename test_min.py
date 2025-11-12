from immutable import freeze, NotFreezable, isfrozen
from gc import collect
from sys import getrefcount
class C:
    def __init__(self):
        self.x = 0

    def set(self, x):
        d = self.__dict__
        d['x'] = x

def test_dict_mutation():
    print("********************Start obj = C()   ***************getrefcount(C):", getrefcount(C))
    obj = C()
    print("********************End obj = C()   ***************getrefcount(C):", getrefcount(C))
    print("********************Start obj.set(1)  ***************getrefcount(C):", getrefcount(C))
    obj.set(1)
    print("********************End obj.set(1)  ***************getrefcount(C):", getrefcount(C))
    print("********************Start freeze(obj)  ***************getrefcount(C):", getrefcount(C))
    freeze(obj)
    print("********************End freeze(obj)  ***************getrefcount(C):", getrefcount(C))

if __name__ == '__main__':
    print("*******************************************************************Freeze C")
    print("***********************************getrefcount(C):", getrefcount(C))
    freeze(C)
    print("*******************************************************************Testing dict mutation")
    print("***********************************getrefcount(C):", getrefcount(C))
    test_dict_mutation()
    print("*******************************************************************Testing dict mutation 2")
    print("***********************************getrefcount(C):", getrefcount(C))
    test_dict_mutation()
    print("*******************************************************************Collecting")
    print("***********************************getrefcount(C):", getrefcount(C))
    collect()
    print("*******************************************************************Done")
    print("***********************************getrefcount(C):", getrefcount(C))
