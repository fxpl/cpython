from regions import Region, is_local
from immutable import freeze, register_freezable

r = Region()
r2 = Region()
print(f"Initial Region: {r}")

class A: pass; 
freeze(A())

r.a = A()
r.b = A()
r.c = A()
r.d = A()
r.e = A()
r.f = A()
r.arr1 = [r.a, r.b, r.c]
r.arr2 = [r.b, r.c, r.f]

# s1 = set(r.arr1)
# print(f"Region after creating set1: {r}")
# print(f"{s1}")
# s2 = set(r.arr2)
# print(f"Region after creating set2: {r}")
# print(f"{s2}")
# input("Press Enter to create set or...")
# s1 |= s2
# # s1.update(s2)
# print(f"Region after creating set or: {r}")
# print(f"Or result: {s1}")

r.s1 = set(r.arr1)
print(f"Region after creating set1: {r}")
print(f"{r.s1}")
s2 = set(r.arr2)
print(f"Region after creating set2: {r}")
print(f"{s2}")
input("Press Enter to create set or...")
# r.s1 |= s2
r.s1.update(s2)
print(f"Region after creating set or: {r}")
print(f"Or result: {r.s1}")