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
r2.d = A()
r2.e = A()
f = A()
g = A()

r.arr1 = [r.a, r.b, r.c]
r.arr2 = [r.a]
s1 = set(r.arr1)
print(f"Region after creating set1: {r}")
print(f"{s1}")
s2 = set(r.arr2)
print(f"Region after creating set2: {r}")
print(f"{s2}")
input("Press Enter to create set difference...")
s1 -= s2
print(f"Region after creating set difference: {r}")
print(f"{s1}")

# s1 = set(r.arr1)
# print(f"Region after creating set1: {r}")
# s2 = set(r.arr2)
# print(f"Region after creating set2: {r}")
# ss1 = set(s1)
# print(f"Region after creating set from set1: {r}")
# ss2 = set(s2)
# print(f"Region after creating set from set2: {r}")
# ss1 -= ss2
# print(f"Region after creating set difference of set: {r}")
