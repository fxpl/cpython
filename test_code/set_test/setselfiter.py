from regions import Region, is_local
from immutable import freeze, register_freezable

class A: pass
freeze(A())

r = Region()
print(f"Region r: {r}")
r.a = A()
r.b = A()
r.arr = [r.a, r.b]
print(f"Region r after creating arr: {r}")
r.s = set(r.arr)
print(f"Region r after creating set: {r}")
it = iter(r.s)
print(f"Region r after creating iterator for set: {r}")
r.it2 = iter(it)
print(f"Region r after creating iterator for iterator: {r}")