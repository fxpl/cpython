import time
from concurrent import boc
from concurrent.boc import Cown, when

c1 = Cown(0)
c2 = Cown(1)


for i in range(10):

    @when(c1, c2)
    def fib(x, y):
        import time

        temp = x.val
        x.val = y.val
        y.val = temp + y.val
        print(f"Fib: {x.val}, {y.val}")
        time.sleep(0.1)

    print(f"Scheduled fib number {i}")


for i in range(3):

    @when(c1, c2, Cown(i))
    def swap(x, y, i):
        temp = x.val
        x.val = y.val
        y.val = temp
        print(f"Executed swap number {i.val}")

    print(f"Scheduled swap number {i}")

# If you finish here, the Python interpreter will block until all behaviours are finished before
# exiting.

# This call blocks on the values of c1 and c2. It is scheduled in the same way as when blocks, so
# all when blocks scheduled before this call on any of these cowns will be exected before.
final_vals = boc.block_on_cowns(c1, c2, timeout=30)
print(f"The final values of c1 and c2 are {final_vals}")
assert final_vals, (84, 55)
