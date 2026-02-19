from regions import Region, is_local
from immutable import freeze, register_freezable
import sys

r1 = Region()
r2 = Region()
r3 = Region()
# freeze(10)
r1.start = 445435435435535340000000
r2.stop = 445435435435535345547898
r3.step = 445435435435550
# r1.start = 1
# r2.stop = 10
# r3.step = 2
# print("sys.getrefcount(r.start): ", sys.getrefcount(r1.start))
# print("sys.getrefcount(r.stop): ", sys.getrefcount(r2.stop))
# print("sys.getrefcount(r.step): ", sys.getrefcount(r3.step))
# print(f"{r1.owns(r1.start)}")
# print(f"{r2.owns(r2.stop)}")
# print(f"{r3.owns(r3.step)}")

print(f"Initial Region: {r1}")
print(f"Initial Region: {r2}")
print(f"Initial Region: {r3}")
#print(sys.getrefcount(r.stop))
# print(f"{r.stop}")
ra1 = range(r1.start, r2.stop, r3.step)[2]

print(f"{r1.owns(ra1)}")
print(f"{r2.owns(ra1)}")
print(f"{r3.owns(ra1)}")