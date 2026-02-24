from regions import Region, is_local
from immutable import freeze, register_freezable
import sys

r1 = Region()
# input("0: Press Enter to continue...")
r1.start = 2**100
r1.stop = 2**200
r1.step = 2**50
print(f"{sys.getrefcount(r1.start)}")
print(f"Initial Region: {r1}") #_lrc=3
# input("1: Press Enter to continue...")
r_large = range(r1.start, r1.stop, r1.step)
print(f"{r_large}")
print(f"{r1.owns(r_large)}")
print(f"Region 1 after setting start: {r1}") #_lrc=6
# input("2: Press Enter to continue...")
r1.range = r_large
print(f"Region 1 after moving range into the region: {r1}") #_lrc=4: -3 from subtracting start, stop, and end to the region since the range obj is moved into the region, +1 from r_large
print(f"{r1.owns(r_large)}")
input("3: Press Enter to continue...")
reversed_range = reversed(r_large) # Create "iter" object from "range"
print(f"{reversed_range}")
# print(f"{hex(id(reversed_range)), hex(id(r_large))}") # reversed_range is not the same object as r_large.
print(f"Region 1 after creating reversed_range: {r1}") 
input("continue")
reversed_range = None
print(f"Region 1 after deleting reversed_range: {r1}")
r_large = None
print(f"Region 1 after deleting r_large: {r1}")