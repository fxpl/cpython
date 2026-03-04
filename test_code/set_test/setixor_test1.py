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
r.arr1 = [r.a, r.b, r.c, r.f]
r.arr2 = [r.a, r.b, r.f]
s1 = set(r.arr1)
print(f"Region after creating set1: {r}") # +4
print(f"{s1}")
s2 = set(r.arr2)
print(f"Region after creating set2: {r}") # +3
print(f"{s2}")
input("Press Enter to create set xor...")
# s1 ^= s2
s1.symmetric_difference_update(s2) # -4 for derefs r.a, r.b, r.c, r.f, then +1 for r.c
print(f"Region after creating set xor: {r}")
print(f"Xor result: {s1}")

# r.a = A()
# r.b = A()
# r.c = A()
# r2.d = A()
# r2.e = A()
# r2.f = A()
# r2.g = A()
# arr1 = [r.a, r.b, r.c, r2.d]
# arr2 = [r.c, r2.d, r2.e, r2.f, r2.g]
# print(f"Region1 before creating sets: {r}")
# print(f"Region2 before creating sets: {r2}")
# s1 = set(arr1)
# print(f"Region1 after creating set1: {r}")
# print(f"Region2 after creating set1: {r2}")
# print(f"{s1}")
# s2 = set(arr2)
# print(f"Region1 after creating set2: {r}")
# print(f"Region2 after creating set2: {r2}")
# print(f"{s2}")
# input("Press Enter to create set xor...")
# s1 ^= s2
# # result = s1.symmetric_difference(s2)
# print(f"Region1 after creating set xor: {r}")
# print(f"Region2 after creating set xor: {r2}")
# print(f"Xor result: {s1}")