from regions import Region, is_local
from immutable import freeze, register_freezable
import sys

r1 = Region()
r1.start = 2**10000
r1.stop = 2**20000
r1.step = 2**100

print(f"Initial Region: {r1}")

ra1 = range(r1.start, r1.stop, r1.step)
print(f"Region after creating range: {r1}")
# print(f"{ra1}")
print(f"r1.owns(ra1): {r1.owns(ra1)}")

r1.slicing_start = 2**200
r1.slicing_stop = 2**300
r1.slicing_step = 2**100
# Has a problem when r1.slicing_step is set to 2**40
input("Continue")

# res = ra1[r1.slicing_start:r1.slicing_stop
res = ra1[r1.slicing_start:r1.slicing_stop:r1.slicing_step]
print(f"Region after accessing ra1[:r1.slicing_stop]: {r1}") # LRC should not change since the slice object is destroyed.
# print(f"Result of slicing: {res[2]}")
# r1.result_from_idx1 = res
# print(f"Region after setting result_from_idx1: {r1}")

test_slic_obj = slice(r1.slicing_start, r1.slicing_stop, r1.slicing_step)
print(f"Region after creating test_slic_obj: {r1}") # LRC +3 since the slice object is still alive.

# res = None
# print(f"Region after deleting res: {r1}")

# print(f"is_local(ra1[0]): {is_local(ra1[0])}")
# print(f"is_local(ra1[1]): {is_local(ra1[1])}")
# print(f"is_local(ra1[2**75]): {is_local(ra1[2**75])}")
