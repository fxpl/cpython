from regions import Region, is_local
from immutable import freeze, register_freezable
import sys

r1 = Region()
r2 = Region()
r3 = Region()
# freeze(10)
r1.start = 445435435435535340000000
r2.stop = 445435435435535345540000
r3.step = 4454354354355500000
# r1.start = 1
# r2.stop = 10
# r3.step = 2
print("sys.getrefcount(r1.start): ", sys.getrefcount(r1.start))
print("sys.getrefcount(r2.stop): ", sys.getrefcount(r2.stop))
print("sys.getrefcount(r3.step): ", sys.getrefcount(r3.step))
# print(f"{r1.owns(r1.start)}")
# print(f"{r2.owns(r2.stop)}")
# print(f"{r3.owns(r3.step)}")

print(f"Initial Region: {r1}")
print(f"Initial Region: {r2}")
print(f"Initial Region: {r3}")
#print(sys.getrefcount(r.stop))
# print(f"{r.stop}")
input("Continue")
ra = range(r1.start, r2.stop, r3.step)
print(f"{ra}")
print(f"Region 1 after setting stop: {r1}")
print(f"Region 2 after setting stop: {r2}")
print(f"Region 3 after setting step: {r3}")
ra = None
print(f"Region 1 after deleting ra: {r1}")
print(f"Region 2 after deleting ra: {r2}")
print(f"Region 3 after deleting ra: {r3}")