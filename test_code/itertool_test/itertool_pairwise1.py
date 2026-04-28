from itertools import pairwise
from regions import Region, is_local
from immutable import freeze, register_freezable

class A: pass
freeze(A())
freeze(pairwise)
r = Region()
r.a = A()
r.b = A()
r.c = A()
r.d = A()
r.e = A()
r.f = A()

# r.arr = [r.a, r.b, r.c, r.d, r.e, r.f]
# print(f"Region r after creating arr: {r}")
# r.it_arr = iter(r.arr)
# print(f"Region r after creating iterator: {r}")
# obj = pairwise(r.it_arr)
# print(f"Region r after creating pairwise iterator: {r}")
# input("Press Enter to assign pairwise iterator to r.obj...")
# x = next(obj) # LRC +2 from tuple to 2 elements in the region, and +1 from obj to old (right element of tuple)
# print(f"Region r after getting next from pairwise iterator: {r}")
# print(f"x: {x}")
# y = next(obj) # LRC +2
# print(f"Region r after getting next from pairwise iterator: {r}")
# print(f"y: {y}")
# r.z = next(obj) # LRC +0
# print(f"Region r after getting next from pairwise iterator: {r}")
# print(f"z: {r.z}")
# x = None # LRC -2
# print(f"Region r after deleting x: {r}")
# y = None # LRC -2
# print(f"Region r after deleting y: {r}")
# r.z = None # LRC -0
# print(f"Region r after deleting z: {r}")
# obj = None # LRC -2 from deleting ref from obj to r.it and obj to old
# print(f"Region r after deleting obj: {r}")

# r.arr = [r.a, r.b, r.c, r.d, r.e, r.f]
# print(f"Region r after creating arr: {r}")
# r.it_arr = iter(r.arr)
# print(f"Region r after creating iterator: {r}")
# obj = pairwise(r.it_arr)
# print(f"Region r after creating pairwise iterator: {r}")
# input("Press Enter to assign pairwise iterator to r.obj...")
# x = next(obj) # LRC +3
# print(f"Region r after getting next from pairwise iterator: {r}")
# print(f"x: {x}")
# x = None
# print(f"Region r after deleting x: {r}")
# x = next(obj) # LRC +0 since everything is already set from the previous x = next(obj)
# print(f"Region r after getting next from pairwise iterator: {r}")
# print(f"x: {x}")
# y = next(obj) # LRC+2
# print(f"Region r after getting next from pairwise iterator: {r}")
# print(f"y: {y}")
# x = None # LRC -2
# print(f"Region r after deleting x: {r}")
# y = None # LRC -2
# print(f"Region r after deleting y: {r}")
# obj = None # LRC -2 from deleting ref from obj to r.it and obj to old
# print(f"Region r after deleting obj: {r}")

# r.arr = [r.a, r.b, r.c, r.d, r.e, r.f]
# print(f"Region r after creating arr: {r}")
# r.it_arr = iter(r.arr)
# print(f"Region r after creating iterator: {r}")
# # input("Press Enter to create pairwise iterator...")
# obj = pairwise(r.it_arr) # LRC +1
# print(f"Region r after creating pairwise iterator: {r}")
# # input("Press Enter to assign pairwise iterator to r.obj...")
# r.x = next(obj) # LRC +1 from obj to tuple, and +1 from obj->old to right element of tuple
# print(f"Region r after getting next from pairwise iterator: {r}")
# print(f"x: {r.x}")
# y = next(obj) # LRC +2 bacause of tuple to 2 elements in the region, -1 from deleting ref from obj->result to tuple
# print(f"Region r after getting next from pairwise iterator: {r}")
# print(f"y: {y}")
# r.x = None # LRC -0
# print(f"Region r after deleting x: {r}")
# y = None # LRC -2
# print(f"Region r after deleting y: {r}")
# obj = None # LRC -2 from deleting ref from obj to r.it and -1 from obj to old
# print(f"Region r after deleting obj: {r}")

r.arr = [r.a, r.b, r.c, r.d, r.e, r.f]
print(f"Region r after creating arr: {r}")
r.it_arr = iter(r.arr)
print(f"Region r after creating iterator: {r}")
r.obj = pairwise(r.it_arr)
print(f"Region r after creating pairwise iterator: {r}")