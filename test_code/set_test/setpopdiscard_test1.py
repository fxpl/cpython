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

# s1.pop()
# print(f"Region after popping from set s1: {r}")

s1.discard(r.a)
print(f"Region after discarding r.a from set s1: {r}")
s1.discard(r.b)
print(f"Region after discarding r.b from set s1: {r}")
s1.discard(r.c)
print(f"Region after discarding r.c from set s1: {r}")
