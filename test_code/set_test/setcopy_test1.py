from regions import Region, is_local
from immutable import freeze, register_freezable

r = Region()
print(f"Initial Region: {r}")
class A: pass
freeze(A())

r.a = A()
r.b = A()
r.c = A()
r.d = A()
r.e = A()
r.f = A()

r.arr1 = [r.a, r.b, r.c]
print(f"Region after setting arr1: {r}")
s1 = set(r.arr1)
print(f"Region after creating set s1: {r}")
s2 = s1.copy()
print(f"Region after copying s1 to s2: {r}")
