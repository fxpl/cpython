import time
from concurrent.boc import Cown, block_on_cowns, when

c1 = Cown(0)


@when(c1)
def hej(x):
    print(f"The val is {x.val}")
    return 42


time.sleep(1)
print(block_on_cowns(hej, timeout=2))
