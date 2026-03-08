from regions import Region, is_local
from immutable import freeze, register_freezable

r1 = Region()
r2 = Region()
r3 = Region()
r4 = Region()


class A: pass; 
freeze(A())

r1.a = A()
r1.b = A()
r1.c = A()
r1.d = A()
r1.e = A()
r1.f = A()
r1.arr1 = [r1.a, r1.b, r1.c]
r1.arr2 = [r1.b, r1.c, r1.f]
r1.arr3 = [r1.b, r1.d, r1.e]

# print(f"Initial Region: {r}")
# s1 = set(r.arr1)
# print(f"Region after creating set1: {r}")
# print(f"{s1}")
# s2 = set(r.arr2)
# print(f"Region after creating set2: {r}")
# print(f"{s2}")
# input("Press Enter to create set intersection...")
# # result = s1 & s2
# result = s1.intersection(s2)
# print(f"Region after creating set intersection: {r}")
# print(f"Intersection result: {result}")

# print(f"Initial Region: {r1}")
# s1 = set(r1.arr1)
# print(f"Region after creating set1: {r1}")
# print(f"{s1}")
# s2 = set(r1.arr2)
# print(f"Region after creating set2: {r1}")
# print(f"{s2}")
# s3 = set(r1.arr3)
# print(f"Region after creating set3: {r1}")
# print(f"{s3}")
# input("Press Enter to create set intersection...")
# result = s1.intersection(s2, s3)
# print(f"Region after creating set intersection: {r1}")
# print(f"Intersection result: {result}")

print(f"Initial Region: {r1}")
r1.s1 = set(r1.arr1)
print(f"Region after creating set1: {r1}")
print(f"{r1.s1}")
s2 = set(r1.arr2)
print(f"Region after creating set2: {r1}")
print(f"{s2}")
r1.s3 = set(r1.arr3)
print(f"Region after creating set3: {r1}")
print(f"{r1.s3}")
input("Press Enter to create set intersection...")
result = r1.s1.intersection(s2, r1.s3)
print(f"Region after creating set intersection: {r1}")
print(f"Intersection result: {result}")

# print(f"Initial Region: {r1}")
# print(f"Region r2: {r2}")
# print(f"Region r3: {r3}")
# print(f"Region r4: {r4}\n")
# r1.set1 = set([r2, r3])
# print(f"Region after creating set1: {r1}")
# print(f"Region r2: {r2}")
# print(f"Region r3: {r3}")
# print(f"Region r4: {r4}\n")
# r1.set2 = set([r2, r4])