from regions import Region
from immutable import freeze

class A: pass
freeze(A())

r = Region()
r.a = A()
r.b = A()
r.c = A()

print(f"Initial Region: {r}")
input("Press Enter to create set from region attributes...")
arr = set([r.a, r.b, r.c])
print(f"Region after creating set: {r}")
input("Press Enter to set arr to region...")
r.arr = arr
print(f"Region after setting arr to set: {r}")