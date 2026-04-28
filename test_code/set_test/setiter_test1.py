# Test Case for testing setiterobject's dealloc and traverse functionsfrom regions import Region, is_local
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
r.word5=A()

r.arr = [r.word, r.word2, r.word3, r.word4, r.word5]
print(f"Region after setting word: {r}")
s = set(r.arr)
print(f"Region after creating set: {r}")

it = iter(s)
print(f"Region after creating set iterator: {r}")
# input("Press Enter to print set iterator...")
# r.it = it
# input("Press Enter to print set iterator...")
# print(f"Region after setting iterator: {r}") # iterator that "it" points to is moved into the region, so "s" as well. Since "s" is moved into the region, no borrowed references anymore from outside of the region. LRC is reset to the initial value. Then +2 from "it" and "s" variable itself.
# s=None
# it=None
# print(f"Region after setting set and iterator to None: {r}")

a1 = next(it)
print(f"Region after calling next on set iterator: {r}")
a2 = next(it)
print(f"Region after calling next on set iterator again: {r}")
r.a3 = next(it)
print(f"Region after calling next on set iterator again: {r}")
a4 = next(it)
print(f"Region after calling next on set iterator again: {r}")
it = None
print(f"Region after setting iterator to None: {r}")

# r.it = iter(s)
# print(f"Region after creating set iterator: {r}")
# print(f"{r.it}")
# print(f"Region after printing set iterator: {r}")
# a1 = next(r.it)
# print(f"Region after calling next on set iterator: {r}")
# a2 = next(r.it)
# print(f"Region after calling next on set iterator again: {r}")
# r.a3 = next(r.it)
# print(f"Region after calling next on set iterator again: {r}")
# a4 = next(r.it)
# print(f"Region after calling next on set iterator again: {r}")
# r.it = None
# print(f"Region after setting iterator to None: {r}")
