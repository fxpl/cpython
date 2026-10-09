# Porting the experiment's region tests to tracing regions

2026-10-09. Base commit: `0a6ec0a3d3` (Detect concurrent accesses via the bridge).

## Summary

The tests that check what closing a region means were ported from the other
implementation of regions in `~/w/python/experiment`. That suite is
`Lib/test/test_free_threading/test_region.py` and `test_cown.py`, about 200
tests. The ported tests are in
`Lib/test/test_freeze/test_tracing_region_ported.py`: 62 tests in all,
including 5 that check deliberate differences between the two designs.

The port found 6 bugs, all in closing regions and releasing cowns:

| # | Bug | Builds | Status |
|---|-----|--------|--------|
| 1 | A failed close leaves objects in the region's private GC list | GIL | **fixed** |
| 2 | Wrong assertion in `_PyTracingRegion_Close` crashes debug builds | GIL debug | **fixed** |
| 3 | `PyCown_clear` dereferences NULL when the GC has cleared the cown already | free-threaded | **fixed** |
| 4 | `_Py_CheckTracingFlag` never clears the flag, so concurrent accesses go undetected | free-threaded | open |
| 5 | A parent cannot close an open subregion whose bridge is owned by another thread | free-threaded | open |
| 6 | Closing waits on owners that have stopped servicing their merge queue, until the 1 s timeout | free-threaded | open |

With the fixes, every ported test passes on a GIL debug build. On a
free-threaded debug build, 4 ported tests fail because of the open bugs 4–6.

## What was ported

The experiment closes regions differently: it uses colours, claims, FREE
objects, grace periods and weak reference tags. Most of its tests check that
machinery and mean nothing here. Only tests that check behaviour, meaning which
regions can close and what happens around a close, were ported.

| Ported class | Tests | Source |
|---|---|---|
| `TestRegionStates` | 5 | open/close states; attribute access opens a region, `repr` does not |
| `TestRegionType` | 4 | not subclassable, not picklable or copyable, constructor arguments, collected in cycles |
| `TestIsolation` | 13 | the core rule: no references from outside into the region |
| `TestSubregions` | 13 | nesting, edges between regions, subregions with outside references, cowns inside regions |
| `TestRegionWeakrefs` | 8 | weak references into, out of, and inside a region |
| `TestConcurrentClose` | 7 | closes interleaved with other threads, the GC, and churn through `gc.get_referents` |
| `TestCownRegions` | 9 | releasing a cown closes its region, references to the bridge, moving regions between cowns |
| `TestCownStress` | 3 | threads handing regions around through cowns, with and without a concurrent GC |

These were not ported:

- **Implementation-specific:** `TestColours`, `TestClaims*`, `TestFreeObjects*`,
  `TestUntracking`, `TestRetracking`, `TestOverrides`, `TestTableCache`,
  `TestTagEpochs`, `TestRegionHandle`, `TestDecrementDuringValidation` and
  `TestDereferenceDuringTagging`.
- **Different concepts:**
  - `TestOwnership`: the experiment's open regions belong to a thread.
  - `TestWeakrefGate`: the experiment tags weak references instead of clearing them.
  - `TestExternalReferences`: the experiment's `region_close` returns a reference count.
  - Tests that force a region into a state through `region_set_state`.
- **Grace-period tests,** and tests that need the experiment's
  `spin_without_safe_point` helper.
- **The cown API itself** (`TestCown`, `TestCownThreads`). The two cown APIs
  differ: the experiment's cowns start out released and are context managers.

### API mapping

| experiment | here |
|---|---|
| `region_close(r, timeout)` | `_testinternalcapi.region_close(r)` (new) |
| `region_state(r)` | `_testinternalcapi.region_is_closed(r)` (new) |
| `check_window` / `region_set_check_pauses` | `_testinternalcapi.region_set_close_pause(s)` (new, debug builds) |
| `Cown()` starts released; `with c:` | `Cown(v)` starts acquired; the test uses an `acquired(c)` helper |
| a region has one `contents` slot | a region has any attributes; the tests use `contents` |

### Differences between the designs

Where the experiment expects something this implementation does differently on
purpose, the test keeps the experiment's expectation and is marked
`@diverges(reason)`. That makes it an expected failure, and an unexpected pass
is reported. All 5 fail as expected:

- **Subregions after a failed close.** A failed close leaves the subregions it
  closed along the way closed. The experiment restores them to open.
  - `test_subregion_with_external_reference_keeps_its_state`
  - `test_failure_restores_subregions`
- **Weak references from outside.** Closing clears them for good. The
  experiment only refuses them while the region is closed, so they work again
  once it is opened.
  - `test_weakref_into_subregion_after_opening`
  - `test_external_weakref_usable_after_opening`
  - `test_proxy_usable_after_opening`

`test_subregion_of_other_thread` was adapted rather than marked. The experiment
refuses the close while the thread that opened the subregion is alive. Here
regions have no owning thread, so the test expects the close to succeed (see
bug 5).

## Test hooks added

In `Modules/_testinternalcapi.c`:

- `region_close(r)` calls `_PyTracingRegion_Close()`. It raises `RuntimeError`
  if the region cannot be closed. Unlike `Cown.release()`, it does not check for
  references to the bridge.
- `region_is_closed(r)` calls `_PyTracingRegion_IsClosed()`.
- `region_set_close_pause(seconds)` exists in debug builds only. It makes every
  close pause after its trace and before the validation and commit. The thread
  is detached during the pause, which also releases the GIL. This lets a test
  interfere with a close at the point where it is vulnerable.

The pause is implemented as `_PyTracingRegion_SetTestPause()` in
`Objects/tracingregionobject.c` and declared in
`Include/internal/pycore_immutability.h`. All of it is under `#ifdef Py_DEBUG`.

## Results

Both builds were configured with `--with-pydebug`, one with `--disable-gil` and
one without.

| | GIL debug | free-threaded debug |
|---|---|---|
| `test_tracing_region_ported` | pass (5 expected failures, 2 skipped) | 4 failures (bugs 4–6) |
| `test_tracing_region` (existing) | pass | intermittent timeout in `test_close_objects_owned_by_blocked_thread` (bug 6) |
| `test_freeze`, `test_weakref`, `test_sys`, `test_capi`, `test_free_threading` | pass (1,971 tests) | pass apart from the failures above |

The free-threaded failures:

| Test | Bug |
|---|---|
| `TestConcurrentClose.test_reference_taken_and_dropped_during_close` | 4 |
| `TestSubregions.test_subregion_of_other_thread` | 5 or 6, depending on timing |
| `TestCownStress.test_pass_regions_around` | 5 and 6 |
| `TestCownStress.test_pass_regions_around_with_gc` | 5 and 6 |

To run the ported tests:

```
./python -m test test_freeze.test_tracing_region_ported -v
```

## Fixed bugs

### 1. A failed close leaves objects in the region's private GC list

`Objects/tracingregionobject.c`, in `_try_close_region()`, GIL builds
(`_Py_PYRONA_INTERPRETER_SHARING`).

A trace moves each object it visits into `region->gc_list`. Two failure paths
merge the list back into the interpreter's GC: the restart path and the
`external_rc > 0` path. The shared `error:` label did not. So any other error
left the region open with objects in its private list, hidden from the GC. Such
errors include an unmovable object, the parent-cycle error, a concurrent access
(`TRACING_FAILED`), and an allocation failure.

The next close then hit `assert(gc_list_is_empty(...))`, and so did the
region's finalizer. On a first attempt the bug is usually masked: freezing a
string sets `state->gc_list = NULL` before any objects are moved. On a retry,
the list is in use from the start.

It was found by `TestSubregions.test_edges_across_regions` (the `to_parent_region`
case) and `TestConcurrentClose.test_access_while_closing_fails_the_close`.

**Fix:** dissolve the list on the `error:` path as well. Dissolving a list that
is already empty does nothing.

### 2. Wrong assertion in `_PyTracingRegion_Close`

`_PyTracingRegion_Close()` asserted that `self->gc_list` is empty. A closed
region keeps its members in that list, so closing an already-closed region
tripped the assertion. That happens in the existing
`TestRegionOpening.test_release_closed_region`. The result was that the
existing suite crashed on every GIL debug build. Release builds compile the
assertion out.

**Fix:** `assert(!region_is_open(self) || gc_list_is_empty(&self->gc_list))`.

### 3. `PyCown_clear` dereferences NULL

`Objects/cownobject.c`, free-threaded builds, where cowns are tracked by the GC.

When a cown with an immutable value is part of cyclic garbage, the GC calls its
`tp_clear`, and `Py_CLEAR(self->value)` sets the value to NULL. When the cown
is deallocated afterwards, `cown_dealloc_owned()` calls `PyCown_clear()` again,
which passes NULL to `_PyImmutability_CanViewAsDeepImmutable()`. Reproducer:

```python
import gc
from immutable import Cown
c = Cown()
c.release()
l = [c]
l.append(l)
del c, l
gc.collect()   # segfault on free-threaded builds
```

**Fix:** return early from `PyCown_clear()` when `self->value` is NULL.

## Open bugs

### 4. `_Py_CheckTracingFlag` never clears the flag

`Python/pystate.c`:

```c
void _Py_CheckTracingFlag(PyObject *ob) {
    if ((_Py_OB_FLAGS_LOAD(ob) & _Py_REGION_TRACE_FLAG) == 0) {
        _Py_OB_FLAG_REMOVE(ob, _Py_REGION_TRACE_FLAG);
    }
}
```

This clears the flag only when it is already clear, so it never does anything.
As a result, the first check in `_validate_region_closed_visit()` cannot fail.
Suppose another thread takes a reference to an object in the region and drops
it again while the region is being traced. The reference count is then back to
the value the trace recorded, and the close succeeds. A reference that is kept
is still caught, by the reference count check.

The commit that added this function is titled "Let's benchmark RC's new slow
branch", so the condition may have been inverted on purpose to measure the cost
of the branch. If not, the condition should be `!= 0`.

**Test:** `TestConcurrentClose.test_reference_taken_and_dropped_during_close`.
It pauses the close and has another thread call `gc.get_referents(r)`, which
increments and decrements the region's `__dict__`. The close is expected to
fail but succeeds.

### 5. An open subregion whose bridge is owned by another thread cannot close with its parent

`try_close_region_tree()` and `_trace_visit_bridge_ref()` in
`Objects/tracingregionobject.c`. A parent containing such a subregion is traced
as follows:

1. **Attempt 1.** The parent's trace finds the subregion open. It queues the
   subregion and restarts. The open path never reaches `_move_obj()`, so the
   ownership of the subregion's bridge is not checked.
2. **The subregion closes.** Its own trace may need two attempts if its objects
   are non-local too.
3. **Attempt 2.** The parent's trace now finds the subregion closed and moves
   its bridge with `_move_obj()`. The bridge is owned by another thread, so the
   bridge is queued for a reference count merge and the trace restarts again.
4. **Attempt 3** would be needed, but `PER_REGION_TRACE_LIMIT` is 2, so the
   close fails with "the region … could not be closed after 2 tracing attempts".

Reproducer (deterministic):

```python
import threading, _testinternalcapi as t
from immutable import TracingRegion as Region
box, made, done = [], threading.Event(), threading.Event()
def make():
    r = Region()          # the bridge is owned by this thread
    r.contents = [1]
    box.append(r)
    made.set()
    done.wait()
th = threading.Thread(target=make); th.start(); made.wait()
outer = Region()
outer.contents = [box.pop()]
try:
    t.region_close(outer)   # RuntimeError: ... could not be closed after 2 tracing attempts
finally:
    done.set(); th.join()
```

Instrumenting the restarts during `test_pass_regions_around` shows this
sequence for every region that fails with this error. In each case the
subregion's bridge was created by the main thread and the parent was released
by a worker.

**Possible fixes:**

- Check the ownership of the bridge, and queue it, when `_trace_visit_bridge_ref()`
  first finds the subregion open. The second attempt then has nothing new to
  find.
- Don't count restarts that were caused only by non-local objects against the
  limit. Those restarts end once the objects are merged.

### 6. Closing waits on owners that have stopped servicing their merge queue

`_move_obj()` and `tree_trace_state_wait_non_local_objects()` in
`Objects/tracingregionobject.c`, and `_Py_brc_queue_object()` in `Python/brc.c`.

When a trace finds an object owned by another thread, it calls
`_Py_brc_queue_object()`. Then, before retrying, it waits up to
`NON_LOCAL_MERGE_TIMEOUT_MS` (1 s) for the owner to merge the object's
reference count.

`_Py_brc_queue_object()` has a fast path for owners that are detached at the
time of the call: it suspends the owner and merges the object itself. If the
owner is attached instead, the object goes onto the owner's queue, and the
owner merges it at its next eval breaker. If the owner blocks before then, it
is detached but never services its queue. Nobody merges the object, and the
closing thread times out with "timed out waiting for N non-local object(s) to
become shared".

Logging the stuck objects during `test_pass_regions_around` shows this pattern:

- the stuck objects are `Foo` instances with an unmerged reference count
  (`ob_ref_shared == 8`);
- their owners are detached the whole time;
- in the cases examined, the owner was blocked in `Cown.acquire()` on the cown
  that the closing thread was trying to release.

So the closer waits for the owner to merge, and the owner waits for the closer
to release the cown. Only the timeout ends the wait. The same race explains why
the existing `test_close_objects_owned_by_blocked_thread` sometimes times out:
the owner can still be attached when its objects are queued, just before it
blocks in `release_owner.wait()`.

Two explanations were tested and ruled out:

- **The cown lock keeps waiting threads attached.** It does not: `cown_lock()`
  wraps the blocking `_PyMutex_LockTimed()` in `Py_BEGIN_ALLOW_THREADS`. Changing
  it to `_PY_LOCK_DETACH` made no difference.
- **A waiting closer does not service its own merge queue.** Calling
  `_Py_brc_merge_refcounts()` in each iteration of the wait loop did not remove
  the timeouts.

**Possible fix:** in each iteration of the wait loop, find the owners of the
objects that are still pending. Suspend each one that is detached, as the fast
path in `_Py_brc_queue_object()` does, and merge its pending objects on its
behalf. The owner's queue would have to be cleared of those objects too, or
merging an object twice must be harmless.

## Changes to the repository

| File | Change |
|---|---|
| `Lib/test/test_freeze/test_tracing_region_ported.py` | new: the ported tests |
| `Modules/_testinternalcapi.c` | the test hooks `region_close`, `region_is_closed`, `region_set_close_pause` |
| `Include/internal/pycore_immutability.h` | declares `_PyTracingRegion_SetTestPause()` (debug builds) |
| `Objects/tracingregionobject.c` | the test pause (debug builds); fixes for bugs 1 and 2 |
| `Objects/cownobject.c` | fix for bug 3 |
| `report.md` | this report |

Nothing has been committed. The in-tree build (`--enable-optimizations`, GIL)
has not been rebuilt with these changes.
