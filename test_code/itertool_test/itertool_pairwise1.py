from itertools import pairwise
from regions import Region, is_local
from immutable import freeze, register_freezable

class A: pass
freeze(A())
# freeze(batched)
r = Region()
r.a = A()
r.b = A()
r.c = A()
r.d = A()
r.e = A()
r.f = A()

r.arr = [r.a, r.b, r.c, r.d, r.e, r.f]
print(f"Region r after creating arr: {r}")
r.it_arr = iter(r.arr)
print(f"Region r after creating iterator: {r}")
obj = pairwise(r.it_arr)
print(f"Region r after creating pairwise iterator: {r}")
input("Press Enter to assign pairwise iterator to r.obj...")
x = next(obj)
print(f"Region r after getting next from pairwise iterator: {r}")
print(f"x: {x}")
y = next(obj)
print(f"Region r after getting next from pairwise iterator: {r}")
print(f"y: {y}")
r.z = next(obj)
print(f"Region r after getting next from pairwise iterator: {r}")
print(f"z: {r.z}")
x = None
print(f"Region r after deleting x: {r}")
y = None
print(f"Region r after deleting y: {r}")
r.z = None
print(f"Region r after deleting z: {r}")
obj = None
print(f"Region r after deleting obj: {r}")

# r.arr = [r.a, r.b, r.c, r.d, r.e, r.f]
# print(f"Region r after creating arr: {r}")
# r.it_arr = iter(r.arr)
# print(f"Region r after creating iterator: {r}")
# input("Press Enter to create pairwise iterator...")
# obj = pairwise(r.it_arr)
# print(f"Region r after creating pairwise iterator: {r}")
# input("Press Enter to assign pairwise iterator to r.obj...")
# r.obj = obj
# print(f"Region r after assigning pairwise iterator to r.obj: {r}")
# r.x = next(r.obj)
# print(f"Region r after getting next from pairwise iterator: {r}")
# print(f"x: {r.x}")
# y = next(r.obj)
# print(f"Region r after getting next from pairwise iterator: {r}")
# print(f"y: {y}")
# r.x = None
# print(f"Region r after deleting x: {r}")
# y = None
# print(f"Region r after deleting y: {r}")
# obj = None
# print(f"Region r after deleting obj: {r}")

# r.arr = [r.a, r.b, r.c, r.d, r.e, r.f]
# print(f"Region r after creating arr: {r}")
# r.it_arr = iter(r.arr)
# print(f"Region r after creating iterator: {r}")
# input("Press Enter to create pairwise iterator...")
# obj = pairwise(r.it_arr)
# print(f"Region r after creating pairwise iterator: {r}")
# input("Press Enter to assign pairwise iterator to r.obj...")
# r.obj = obj
# print(f"Region r after assigning pairwise iterator to r.obj: {r}")
# x = next(r.obj)
# print(f"Region r after getting next from pairwise iterator: {r}")
# print(f"x: {x}")
# r.y = next(r.obj)
# print(f"Region r after getting next from pairwise iterator: {r}")
# print(f"y: {r.y}")
# x = None
# print(f"Region r after deleting x: {r}")
# r.y = None
# print(f"Region r after deleting y: {r}")
# obj = None
# print(f"Region r after deleting obj: {r}")