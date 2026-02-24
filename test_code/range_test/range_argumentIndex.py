from regions import Region, is_local
from immutable import freeze, register_freezable
import sys

r1 = Region()
# freeze(10)
r1.start = 2**100
r1.stop = 2**200
r1.step = 1
# r1.start = 1
# r2.stop = 10
# r3.step = 2

# print(f"{r1.owns(r1.start)}")

print(f"Initial Region: {r1}")

#print(sys.getrefcount(r.stop))
# print(f"{r.stop}")
ra1 = range(r1.start, r1.stop, r1.step)
print(f"Region after creating range: {r1}")
print(f"{ra1}")
print(f"r1.owns(ra1): {r1.owns(ra1)}")

r1.idx1 = 2**105
idx2 = 2**80
input("Continue")

res = ra1[r1.idx1]
print(f"Region after accessing ra1[r1.idx1]: {r1}")
# r1.result_from_idx1 = res
# print(f"Region after setting result_from_idx1: {r1}")

# res = ra1[idx2]
# print(f"Region after accessing ra1[idx2]: {r1}") # I guess PyRegion_NewRef does nothing in case that idx2 is not in the region.

res = None
print(f"Region after deleting res: {r1}")

# print(f"is_local(ra1[0]): {is_local(ra1[0])}")
# print(f"is_local(ra1[1]): {is_local(ra1[1])}")
# print(f"is_local(ra1[2**75]): {is_local(ra1[2**75])}")