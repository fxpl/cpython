from regions import Region, is_local, Cown

r = Region()
obj = {"key": "value"}
r.start = obj
c = Cown(r)
c.acquire()
print(f"{c.value.start['key']}")
r = None
obj = None
c.release()