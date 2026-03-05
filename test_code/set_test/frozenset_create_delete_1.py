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
s1 = frozenset(r.arr1)
print(f"Region after creating frozenset s1: {r}")
r.set1 = s1
print(f"Region after setting set1: {r}")

# s2 = frozenset(s1)
# print(f"Region after creating frozenset s2: {r}")
# print(f"{s2 is r.set1}")

s2 = s1.copy()
print(f"Region after copying s1 to s2: {r}")
print(f"{s2 is r.set1}")