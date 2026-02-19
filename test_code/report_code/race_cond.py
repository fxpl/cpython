from regions import Region, is_local, Cown
import threading

r = Region()
# s_arr = np.empty(3, dtype=object)  # Array that holds Python objects
s_arr = list(range(3))  # Using a list instead of numpy array to avoid complications with numpy's internal optimizations
r.arr = s_arr
c = Cown(r)
# ask for the diagram drawing again from Fred
print(f"r.owns(s_arr): {r.owns(s_arr)}")  # True
# Create a local Python object
local_dict = {"key": "value", "count": 0}

# Assign it to the array
s_arr[0] = local_dict  # Internally writes PyObject* into data buffer

# WITHOUT BARRIER:
# Problem: local_dict is still local, but now referenced from arr inside region r
print(f"s_arr in region: {r.owns(s_arr)}")           # True
print(f"local_dict in region: {r.owns(local_dict)}")  # False - still local
print(f"local_dict is local: {is_local(local_dict)}") # True - didn't transfer! - PROBLEM!

print(f"Region r: {str(r)}")

# DANGER: Array contains reference to local object
# If another thread acquires Cown and modifies arr[0],
# but we still have local_dict reference - DATA RACE!

# c = Cown(r)

# print(f"{c.value.arr[0]}")
# c.release()

print("Starting threads that will cause race condition despite using Cown...")

def race_condition_despite_using_cown():
    c.acquire()
    r = c.value
    for _ in range(5000):
        # print(f"c.owned: {c.owned()}, c.owned_by_thread(); {c.owned_by_thread()}")
        # 1. READ
        val = r.arr[0]["count"] # work because val takes the copy of the PythonObject of number.
        # if "val" is an actual object, val=None is also required because it still makes the region opens (like r).
        
        # 2. DELAY (This forces the other thread to read the same old 'val')
        # Even a tiny computation creates the window for a race condition
        _ = [i**2 for i in range(10)] 
        
        # 3. WRITE
        r.arr[0]["count"] = val + 1
    r = None
    c.release()

# Launch two threads to increment the same spot
t1 = threading.Thread(target=race_condition_despite_using_cown)
t2 = threading.Thread(target=race_condition_despite_using_cown)

t1.start()
t2.start()

r = None
s_arr = None
c.release() 

t1.join()
t2.join()

c.acquire() #needed to print the data in the print statements, otherwise c.value is not accessible

print(f"Result: {c.value.arr[0]['count']}")
print(f"Expected: 10000")
print(f"Difference: {10000 - c.value.arr[0]['count']} (If this is > 0, you found the race!)")