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
r.arr3 = [r.b, r.d, r.e]

s1 = set(r.arr1)
print(f"Region after creating set1: {r}")
print(f"{s1}")
s2 = set(r.arr2)
print(f"Region after creating set2: {r}")
print(f"{s2}")
input("Press Enter to create set intersection...")
# result = s1 & s2
result = s1.intersection(s2)
print(f"Region after creating set intersection: {r}")
print(f"Intersection result: {result}")

# s1 = set(r.arr1)
# print(f"Region after creating set1: {r}")
# print(f"{s1}")
# s2 = set(r.arr2)
# print(f"Region after creating set2: {r}")
# print(f"{s2}")
# s3 = set(r.arr3)
# print(f"Region after creating set3: {r}")
# print(f"{s3}")
# input("Press Enter to create set intersection...")
# result = s1.intersection(s2, s3)
# print(f"Region after creating set intersection: {r}")
# print(f"Intersection result: {result}")