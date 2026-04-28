from regions import Region, is_local

r = Region()
child_obj = {"child_key": "child_value"}
obj = {"key": "value", "child": child_obj}
print(f"Initial Region: {r}")
print(f"Obj is in the local region: {is_local(obj)}") # True
print(f"Child obj is in the local region: {is_local(child_obj)}") # True
r.start = obj
print(f"Region after setting start: {r}")
print(f"Obj is in the local region: {is_local(obj)}") # False
print(f"Child obj is in the local region: {is_local(child_obj)}") # False
print(f"Region owns obj: {r.owns(obj)}") # True
print(f"Region owns child obj: {r.owns(child_obj)}") # True

obj["key"] = "new_value"
print(f"obj after modification: {obj}")
print(f"Region after modifying obj: {r}")