from regions import Region, is_local
from immutable import freeze, register_freezable, isfrozen

r = Region()
print(f"Initial Region: {r}")

class A: pass
freeze(A())
r.word={A(): "value", A(): "value2"}
print(f"Region after setting word: {r}")
s = set(r.word)
print(f"Region after creating set: {r}")
print(f"{s}")

print("\n")
r2 = Region()
print(f"New Region: {r2}")
r2.word = {"key": "value", "key2": "value2"}
print(f"isfrozen(r2.word): {isfrozen(r2.word["key"])}")
print(f"New Region after setting word: {r2}")
s2 = set(r2.word)
print(f"New Region after creating set: {r2}")
print(f"{s2}")