from regions import Region, is_local
from immutable import freeze, register_freezable

r = Region()
r2 = Region()
print(f"Initial Region: {r}")

class A: pass; 
freeze(A())

aa=A()
ab=A()
ac=A()
r2.word4=A()
# TODO: Test undo when 3 are in local, 1 is in another region
arr = [aa, ab, ac, r2.word4]
print(f"Region after setting word: {r}") # LRC=2
s = set(arr)
print(f"Region after creating set: {r}") # LRC=6 since it borrows word, word2, word3, and word4. "s" does not matter here since set is not in the region yet.
try:
    r.set = s
except Exception as e:
    print(f"Error when moving set into the region: {e}")
    print(f"{is_local(aa)}, {is_local(ab)}, {is_local(ac)}")
    print(f"Region after moving set into the region: {r}") 
