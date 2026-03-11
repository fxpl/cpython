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

# r.arr1 = [r.a, r.b, r.c]
# r.arr2 = [r.b, r.c, r.f]
# s1 = set(r.arr1)
# print(f"Region after creating set1: {r}") # +3
# print(f"{s1}")
# s2 = set(r.arr2)
# print(f"Region after creating set2: {r}") # +3
# print(f"{s2}")
# input("Press Enter to create set intersection...")
# # s1 &= s2
# s1.intersection_update(s2)
# print(f"Region after creating set intersection: {r}") # -3: eliminate all points from s1, then +2: add the reference from s1 to the result
# print(f"Intersection result: {s1}")

# r.arr1 = [r.a, r.b, r.c]
# r.arr2 = [r.f]
# s1 = set(r.arr1)
# print(f"Region after creating set1: {r}") # +3
# print(f"{s1}")
# s2 = set(r.arr2)
# print(f"Region after creating set2: {r}") # +3
# print(f"{s2}")
# input("Press Enter to create set intersection...")
# s1 &= s2
# # s1.intersection_update(s2)
# print(f"Region after creating set intersection: {r}") # -3: eliminate all points from s1
# print(f"Intersection result: {s1}")

r.arr1 = [r.a, r.b, r.c, r.d, r.e]
arr2 = [r.a, r.b, r.c, r.f]
print(f"Region after creating arr1 and arr2: {r}") # +3
r.s1 = set(r.arr1)
print(f"Region after creating set1: {r}") # +0
print(f"{r.s1}")
s2 = set(arr2)
print(f"Region after creating set2: {r}") # +3
print(f"{s2}")
input("Press Enter to create set intersection...")
# r.s1 &= s2
r.s1.intersection_update(s2)
print(f"Region after creating set intersection: {r}") 
print(f"Intersection result: {r.s1}")
print(f"s2: {s2}")

# r.arr1 = [r.a, r.b, r.c]
# arr2 = [r.a, r.b, r.f]
# r.arr3 = [r.a]
# print(f"Region after creating arr1 and arr2: {r}") # +3
# r.s1 = set(r.arr1)
# print(f"Region after creating set1: {r}") # +0
# print(f"{r.s1}")
# s2 = set(arr2)
# print(f"Region after creating set2: {r}") # +3
# print(f"{s2}")
# r.s3 = set(r.arr3)
# print(f"Region after creating set3: {r}") # +0
# print(f"{r.s3}")
# input("Press Enter to create set intersection...")
# r.s1.intersection_update(s2, r.s3)
# print(f"Region after creating set intersection: {r}") # Wrong: LRC should not be changed since s1 is in the region
# print(f"Intersection result: {r.s1}")

# arr1 = [r.a, r.b, r.c]
# r.arr2 = [r.a, r.b, r.f]
# print(f"Region after creating arr1 and arr2: {r}") # +3
# s1 = set(arr1)
# print(f"Region after creating set1: {r}") # +3
# print(f"{s1}")
# r.s2 = set(r.arr2)
# print(f"Region after creating set2: {r}") # +0
# print(f"{r.s2}")
# input("Press Enter to create set intersection...")
# # s1 &= r.s2
# s1.intersection_update(r.s2)
# print(f"Region after creating set intersection: {r}")
# print(f"Intersection result: {s1}")