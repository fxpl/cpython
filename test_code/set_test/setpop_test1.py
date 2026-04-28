from regions import Region, is_local
from immutable import freeze, register_freezable

r = Region()
print(f"Initial Region: {r}")

class A: pass; 
freeze(A())

a = A()
b = A()
c = A()

arr = [a, b, c]
print(f"Region after creating arr: {r}")
r.s = set(arr)
print(f"Region after creating set: {r}")