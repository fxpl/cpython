from regions import Region, is_local
from immutable import freeze, register_freezable
import sys

r = Region()
# freeze(10)
r.stop = 44543543543553534554
print("sys.getrefcount(r.stop): ", sys.getrefcount(r.stop))
print(f"{r.owns(r.stop)}")

print(f"Initial Region: {r}")
#print(sys.getrefcount(r.stop))
# print(f"{r.stop}")
ra = range(r.stop)
print(f"{ra}")
print(f"{r.owns(ra)}, {is_local(ra)}")
# print("sys.getrefcount(r.stop): ", sys.getrefcount(r.stop))
#print(sys.getrefcount(r.stop))
print(f"Region after setting stop: {r}")
ra = None
print(f"Region after setting ra to None: {r}")