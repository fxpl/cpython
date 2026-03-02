from regions import Region, is_local

r1 = Region()
r2 = Region()
r3 = Region()
r2.obj = {"key": "value"}

r1.a = r2
# r1.a = r2.obj # RuntimeError: References to objects in other regions are forbidden
# r1.b = r2 # RuntimeError: Regions are not allowed to have multiple parents
# r3.c = r2 # RuntimeError: Regions are not allowed to have multiple parents