from regions import Region, is_local
from immutable import freeze, register_freezable

class A: pass
freeze(A())

r = Region()
print(f"Initial Region: {r}")
r.a = A()
r.b = A()
r.c = A()
r.d = A()

arr = [r.a, r.b, r.c, r.d]
print(f"Region r after creating arr: {r}")
it_arr = iter(arr)
print(f"Region r after creating iterator: {r}")
r.it_arr = it_arr
print(f"Region r after assigning iterator to region: {r}")