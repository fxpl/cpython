import time
import threading
import _interpreters  # Python 3.12+
import sys

print(sys.version)

def cpu_task():
    total = 0
    for i in range(2_000_000):
        total += i ** 2
    return total

# --- METHOD A: threading (your current approach) ---
start = time.perf_counter()
t1 = threading.Thread(target=cpu_task)
t2 = threading.Thread(target=cpu_task)
t1.start(); t2.start()
t1.join(); t2.join()
thread_time = time.perf_counter() - start

# --- METHOD B: sequential (baseline) ---
start = time.perf_counter()
cpu_task()
cpu_task()
sequential_time = time.perf_counter() - start

print(f"Sequential : {sequential_time:.3f}s")
print(f"Threading  : {thread_time:.3f}s")
print(f"Speedup    : {sequential_time / thread_time:.2f}x")
print()
print("If speedup ≈ 1.0 → threads gave NO parallelism benefit (GIL bottleneck)")
print("If speedup ≈ 2.0 → true parallelism achieved")
