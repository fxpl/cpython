from regions import Region, is_local
from immutable import freeze, register_freezable

r = Region()
print(f"Initial Region: {r}")
# r.word=2**1000
# r.word2=2**2000
# r.word3=2**3000
class A: pass; 
freeze(A())

r.word=A()
r.word2=A()


r.arr = [r.word, r.word2]
print(f"Region after setting word: {r}")
# input("Press Enter to create set...")
s = set(r.arr)
print(f"Region after creating set: {r}")
print(f"{s}")
print(f"Region after printing set: {r}")

print("----------------------------------------------------------")
r2 = Region()
r2.word=A()
r2.word2=A()
word3=A()
word4=A()
print(f"New Region: {r2}")
r2.arr = [r2.word, r2.word2, word3, word4] # obj in word3 and word4 are added to the region r2, and word3 and word4 points to its object.
print(f"Region after setting word in new region: {r2}")
s2 = set(r2.arr) # Now, the set points to all objs in the region r2
print(f"Region after creating set in new region: {r2}")

# ------ Uncomment this makes an error to LRC count ---------------
# for i in s:
    # print(hex(id(i)))

# print(hex(id(r.word)), hex(id(r.word2)), hex(id(r.word3)))
# -----------------------------------------------------------------

s = None
print(f"Region after setting set to None: {r}")