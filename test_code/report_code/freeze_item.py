from regions import Region, is_local
from immutable import freeze, register_freezable, isfrozen

r = Region()
class A: pass
obj = {"key": "value"}
print(f"Initial Region: {r}")
r.start = obj
print(f"Region after setting start: {r}")
print(f"Obj is in the local region: {is_local(obj)}")
print(f"Region owns obj: {r.owns(obj)}")
print(f"{isfrozen(obj)}")
print(f"{isfrozen(obj['key'])}")
freeze(obj)
print(f"Region after freezing obj: {r}")
print(f"Obj is in the local region: {is_local(obj)}")
print(f"Region owns obj: {r.owns(obj)}")
print(f"{isfrozen(obj)}")
print(f"{isfrozen(obj['key'])}")