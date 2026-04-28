from regions import Region, is_local
from immutable import freeze, register_freezable

r = Region()
print(f"Initial Region: {r}")
class A: pass
freeze(A())

r.word=A()
r.word2=A()
r.word3=A()
r.word4=A()

r.arr = [r.word, r.word2, r.word3, r.word4]
print(f"Before for loop: {r}")
s = set(r.arr)
print(f"Before for loop: {r}")

for i in r.arr:
    print(hex(id(i)))
i = None

print(f"After for loop: {r}")