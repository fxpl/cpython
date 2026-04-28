from regions import Region, is_local
from immutable import freeze, register_freezable
import sys

r1 = Region()
r1.start = 2**1000
r1.stop = 2**2000
r1.step = 2**100

print(f"Initial Region: {r1}")

ra1 = range(r1.start, r1.stop, r1.step)
print(f"Region after creating range: {r1}")
# print(f"{ra1}")
print(f"r1.owns(ra1): {r1.owns(ra1)}")

input("Continue")
h = hash(ra1)
print(f"Region after hashing ra1: {r1}")
print(f"Hash of ra1: {h}")
print(f"{r1.owns(ra1)}")
# print(f"{ra1}")