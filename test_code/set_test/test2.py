from regions import Region, is_local
from immutable import freeze, register_freezable

r = Region()
print(f"Initial Region: {r}")

class A: pass; 
freeze(A())

r.word=A()
r.word2=A()
r.word3=A()
r.word4=A()
r.arr = [r.word, r.word2, r.word3, r.word4]
print(f"Region after setting word: {r}") # LRC=2
s = set(r.arr)
print(f"Region after creating set: {r}") # LRC=6 since it borrows word, word2, word3, and word4. "s" does not matter here since set is not in the region yet.
print(f"{s}")
print(f"Region after printing set: {r}") # LRC=6
r.set = s
print(f"Region after moving set into the region: {r}") # LRC=3: -4 from subtracting word, word2, word3, and word4 to the region since the set obj is moved into the region, +1 from s
input("Press Enter to create set...")
s2 = set(r.set)
print(f"Region after creating set from set in the region: {r}")
# print(f"{s2}")
# print(f"Region after printing set2: {r}")

# print(f"{s}")

# ------ Uncomment this makes an error to LRC count ---------------
# for i in s:
#     print(hex(id(i)))
# print(f"Region after iterating through set: {r}")
# print(hex(id(r.word)), hex(id(r.word2)), hex(id(r.word3)))
# -----------------------------------------------------------------

s2 = None
print(f"Region after setting set to None: {r}")