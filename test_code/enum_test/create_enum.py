from regions import Region, is_local
from immutable import freeze, register_freezable
from enum import Enum

class A: pass
freeze(A())

r = Region()
input("Press Enter to create objects...")
print(f"Region r: {r}")
r.a = A()
r.b = A()
r.c = A()
r.d = A()
r.e = A()

# arr = [r.a, r.b, r.c, r.d, r.e]
# print(f"Region r after creating arr: {r}")
# r.arr = arr
# print(f"Region r after moving arr: {r}")

#------------------Problem with LRC should not be increases------------------
# print(f"Region r: {r}")
# r.arr = [r.a, r.b]
# print(f"Region r after creating arr: {r}")
# # input("Press Enter to create enum...")
# r.it_arr = iter(r.arr)
# print(f"Region r after creating iterator: {r}")
# # input("Press Enter to create enum...")
# obj = enumerate(r.it_arr) # LRC +1 since obj points to r.it_arr
# print(f"Region r after creating enum: {r}")
# input("Press Enter to create enum...")
# x = next(obj)
# print(f"Region r after getting next from enum: {r}")
# input("Press Enter to move enum...")
# r.re2 = x
# print(f"Region r after getting next from enum: {r}")
# r.y = next(obj)
# print(f"Region r after getting next from enum: {r}")
# x = None
# print(f"Region r after deleting re1: {r}")
# r.y = None
# print(f"Region r after deleting re2: {r}")
# obj = None
# print(f"Region r after deleting obj: {r}")

#------------------Problem with LRC not being decreased------------------
# print(f"Region r: {r}")
# r.arr = [r.a, r.b]
# print(f"Region r after creating arr: {r}")
# # input("Press Enter to create enum...")
# r.it_arr = iter(r.arr)
# print(f"Region r after creating iterator: {r}")
# # input("Press Enter to create enum...")
# obj = enumerate(r.it_arr) # LRC +1 since obj points to r.it_arr
# print(f"Region r after creating enum: {r}")
# # input("Press Enter to create enum...")
# re1 = next(obj)
# print(f"{is_local(obj)}")
# print(f"Region r after getting next from enum: {r}")
# # input("Press Enter to create enum...")
# re2 = next(obj)
# print(f"Region r after getting next from enum: {r}")
# re1 = None # PROBLEM: LRC does not decrease, which should not be since we remove ref.
# print(f"Region r after deleting re1: {r}")
# re2 = None
# print(f"Region r after deleting re2: {r}")
# obj = None
# print(f"Region r after deleting obj: {r}")

#------------------Problem with GC------------------
# print(f"Region r: {r}")
# r.arr = [r.a, r.b]
# print(f"Region r after creating arr: {r}")
# # input("Press Enter to create enum...")
# r.it_arr = iter(r.arr)
# print(f"Region r after creating iterator: {r}")
# input("Press Enter to create enum...")
# r.obj = enumerate(r.it_arr) # LRC +1 since obj points to r.it_arr
# print(f"Region r after creating enum: {r}")
# input("Press Enter to create enum...")
# re1 = next(r.obj)
# print(f"{is_local(r.obj)}")
# print(f"Region r after getting next from enum: {r}")
# input("Press Enter to create enum...")
# r.re2 = next(r.obj)
# print(f"Region r after getting next from enum: {r}")


print(f"Region r: {r}")
r.arr = [r.a, r.b, r.c, r.d, r.e]
print(f"Region r after creating arr: {r}")
# input("Press Enter to create enum...")
r.it_arr = iter(r.arr)
print(f"Region r after creating iterator: {r}")
# input("Press Enter to create enum...")
obj = enumerate(r.it_arr) # LRC +1 since obj points to r.it_arr
print(f"Region r after creating enum: {r}")
input("Press Enter to create enum...")
re = next(obj)
print(f"Region r after getting next from enum: {r}")
r.re2 = next(obj)
print(f"Region r after getting next from enum: {r}")
re3 = next(obj)
print(f"Region r after getting next from enum: {r}")
r.re4 = next(obj)
print(f"Region r after getting next from enum: {r}")