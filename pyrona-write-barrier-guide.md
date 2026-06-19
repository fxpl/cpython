# Lungfish Write Barriers — Migration Guide

This document explains how to insert region write barriers into CPython C
code to make a type region-aware. It is written to be supplied to an LLM that
migrates a whole type autonomously, walking through it function by function.
The "Running a migration" section below is the top-level procedure; the rest of
the document is the reference it draws on.

It assumes the **Lungfish Region Model — Conceptual Reference** is provided
alongside it. Terms like *local region*, *borrow*, *LRC*, *OSC*, *dirty*,
*bridge object*, *immutable*, and *closed* are defined there and are not
re-explained here.

This document is the operational reference. For anything not covered here,
ask rather than infer — do not invent runtime APIs, exception types, or
barrier behavior that is not stated.

---

## Running a migration

This section is the top-level procedure. Given a Python type name (e.g.
`"set"`), migrate the entire type autonomously, then stop. Do not pause for
approval between functions; the human reviews the finished type.

### The loop
1. **Find the spec** (`PyTypeObject` or `PyType_Spec`) for the type, as in
   "The type-level workflow" below.
2. **Movability check.** Inspect the object struct named in `sizeof(...)`. If
   it holds non-movable pointers (`PyThreadState*`, `PyInterpreterState*`,
   frame pointers, or other non-`PyObject*` pointers into runtime/stack
   state), **stop and report it for human review** with the specific fields,
   then wait. Otherwise continue.
3. **Set the flag now.** Add `.tp_flags2 = Py_TPFLAGS2_REGION_AWARE` at the
   start, not the end — otherwise dispatching to the type during testing marks
   regions dirty and the tests can't observe correct behavior. Setting it early
   does not change runtime correctness.
4. **Walk the spec with the moving `// TODO`**, top to bottom, descending into
   sub-structs. For each function the TODO lands on, run the per-function cycle
   below, then advance the TODO. Continue until the TODO reaches the end of the
   spec and all sub-structs.
5. **Run the tests once, at the end** (see "Tests"). The type is not complete
   until the full `test_regions` package passes. If it fails, decide whether
   the test or the migration is wrong, fix, and re-run.
6. **Produce the final output** (below).

### Per-function cycle
For the function currently under the TODO:
1. Migrate it to its Definition of Done, following "Migrating a single
   function" and writing its tests as you go.
2. **Review it with an independent sub-agent — not yourself.** Self-review
   shares the blind spots that produced any error, so it is not trusted. Hand
   the *original* function, the *migrated* function, and this guide to a
   separate review agent with the instruction in "The review step" below.
3. If the review reports violations, fix them and re-review. Repeat until the
   review is clean.
4. Advance the TODO to the next function.

### The review step
Give the review sub-agent this instruction:

> You did not write this code. Review the migrated function **and its tests**
> against the attached Migration Guide. First decide whether the migration is
> even the right shape; then check, specifically: every `Py_DECREF`/`Py_XDECREF`
> on a stack-held value has a preceding `RemoveLocalRef`/`CLEARLOCAL`; every
> exit path (`return`, `goto`) is symmetric; every barrier failure is handled in
> the function's own error-handling style; if the function is in a critical
> section, every failure path exits it; observable behavior is unchanged; every
> skipped barrier is justified with an `assert`; and no value is given a local
> ref only to immediately `TakeRef` it (the new-owning-ref vs transfer
> anti-pattern). Then review the tests: do they exercise the barrier shapes this
> function actually used (neutral / returning-borrow / transfer), assert
> *relative* counter changes rather than absolute values, keep returned
> references alive to observe the borrow and clear them to prove symmetry, and
> cover the relevant failure path — not just the happy path? Report every
> violation specifically. A review that only finds cosmetic issues while a
> barrier is missing or misplaced, or that passes shallow happy-path-only tests,
> is a failed review.

Note: the independent review reduces how many errors reach the test suite, but
it is not a guarantee — it still shares model-level blind spots with the
migrating agent. Compilation and the `test_regions` run at the end are the
checks that do not depend on model judgment, and are the real gate.

### When to stop and ask the human (only these)
Proceed autonomously except in these cases, where you stop and ask rather than
guess (guessing is a failure, not a time-saver):
- The movability check is positive (non-movable pointers found).
- A staged-reference shape is required (see "Staged references") — flag, do not
  implement.
- A public function's signature would need to change to migrate it — flag, do
  not implement.
- Something is genuinely ambiguous and this guide does not resolve it — an
  unclear source region, new-ref vs transfer you cannot determine, or a callee
  whose RC/region behavior you cannot establish.

### Final output
When the whole type is migrated:
- The migrated type: all functions, plus `.tp_flags2 = Py_TPFLAGS2_REGION_AWARE`.
- The tests added.
- The `test_regions` run result.
- A list of everything flagged for human review (movability, staged refs,
  public-signature changes, unresolved questions).

---

## What a write barrier is, in one paragraph

CPython tracks object lifetimes with reference counts (`Py_INCREF` /
`Py_DECREF`). Lungfish additionally tracks *which region owns each object* and
*how many borrows point into each region*. A write barrier is the extra
bookkeeping call placed next to a reference-count change so the region
system's counters (LRC, OSC, ownership) stay correct. Migrating a function
means: for every reference-count change, add the matching region barrier — or
add an `assert` documenting why it can be safely skipped.

---

## Two facts that drive every decision

1. **Stack-held values are borrows (local references).** A value in a C local
   variable — including a value held in a `_PyStackRef` slot — is a borrow
   into the local region. A borrow can be transferred into an owning
   reference stored on a heap object using `TakeRef(src, tgt)`, which adds the
   owning edge from `src` and then removes the local borrow.

2. **References stored into a heap object are owning references and are not
   reversible.** Once an owning reference is added (`AddRef` / `TakeRef`), it
   cannot be cheaply undone. This is why the `Add*` / `Take*` / `XSET*`
   barriers can *fail* and must be checked: the failure is reported *before*
   the irreversible store, so you can still bail out cleanly.

**Default to proper barriers with full error handling.** Skipping a barrier in
favour of an `assert` is an optimization you may apply only when you are
certain it is safe (e.g. the target is immutable, or the reference is into a
known-local object). **If uncertain, always use proper barriers with error
handling over hoping for the best.**

---

## The type-level workflow

You migrate a type by walking its specification top to bottom, migrating one
referenced function at a time.

### Step 1 — Find the type specification
This is usually a constant `PyTypeObject`:
```c
PyTypeObject PySet_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "set",                              /* tp_name */
    sizeof(PySetObject),                /* tp_basicsize */
    ...
    set_dealloc,                        /* tp_dealloc */
    ...
};
```
It may instead be a `PyType_Spec` with a `.slots` array:
```c
PyType_Spec Dialect_Type_spec = {
    .name = "_csv.Dialect",
    .basicsize = sizeof(DialectObj),
    .flags = (...),
    .slots = Dialect_Type_slots,
};
```

### Step 2 — Movability check (flag for human review)
Look at the object struct named in `sizeof(...)` (the `tp_basicsize` /
`.basicsize` argument — e.g. `PySetObject`). If that struct contains pointers
that cannot be moved between sub-interpreters — for example
`_PyInterpreterFrame *`, `PyFrameObject *`, `PyThreadState *`,
`PyInterpreterState *`, or other non-`PyObject *` pointers into runtime/stack
state — the type is likely **non-movable**.

Marking a type non-movable (done by adding it to `_PyRegion_GetMoveability`)
is **always a human decision**. As an LLM, **flag the type for review with the
specific fields that prompted the concern**, and continue with the migration
regardless of the outcome — the movability decision does not block migrating
the functions.

### Step 3 — Walk the spec with a moving `// TODO`
Place a `// TODO` comment at the top of the spec, just before the first
function slot. Migrate the function under the TODO; once it reaches the
Definition of Done (below), move the TODO down to the next function slot.
Repeat to the end of the struct.

```c
PyTypeObject PySet_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "set",                              /* tp_name */
    sizeof(PySetObject),                /* tp_basicsize */
    0,                                  /* tp_itemsize */
    // TODO   <-- migrate set_dealloc, then move this line down
    set_dealloc,                        /* tp_dealloc */
    0,                                  /* tp_vectorcall_offset */
    ...
};
```

Some slots point to sub-structs (`tp_as_number`, `tp_as_sequence`,
`tp_methods`, `tp_getset`, ...). When the TODO reaches one, descend into that
struct and apply the same moving-TODO walk through its function pointers
before returning to the parent spec:
```c
static PyNumberMethods set_as_number = {
    0,                                  /*nb_add*/
    // TODO   <-- migrate set_sub, then move down
    set_sub,                            /*nb_subtract*/
    ...
};
```

This walk defines the order in which functions are migrated, and guarantees
every function reachable from the type's spec is covered.

### Step 4 — The region-aware flag
Mark the type region-aware by adding:
```c
.tp_flags2 = Py_TPFLAGS2_REGION_AWARE
```
Set this **at the start of the migration, not the end** (see "Running a
migration"). Until the flag is set, dispatching to the type marks regions dirty
(safe, but it defeats the purpose during testing — the tests can't observe
correct borrow behavior). Setting it does not change runtime correctness; it
only stops the dirty-marking. It is set early precisely so the per-function
tests are meaningful, and the finished type is reviewed by a human before
production.

---

## Migrating a single function

When the TODO lands on a function, open its definition and work through it.

### Walk the call graph as needed
If the function calls another function that is **not yet migrated**, check
whether that callee does any reference-count work:
- If the callee does **no** RC work (e.g. it only returns a raw C pointer with
  no RC change, like a `set_next` iterator helper), you do not need to migrate
  it to proceed.
- If the callee **does** RC work, migrate it first (descend into it), then
  return.
- If the callee is **already migrated / region-aware** (e.g. `list`, `dict`),
  you can rely on it without rechecking.
- If the call **dispatches through a type slot** to code that may not be
  region-aware, guard it with `PyRegion_NotifyTypeUse(type)` (see reference).

### Classify each reference-count site
For every RC operation in the function, pick the barrier:

| Situation | Barrier |
|-----------|---------|
| Returning a `PyObject*` to the caller (getter, constructed result, field) | `PyRegion_NewRef` / `PyRegion_XNewRef` |
| `Py_DECREF` / `Py_XDECREF` on a stack-held value | `PyRegion_RemoveLocalRef` then `Py_DECREF`, or `PyRegion_CLEARLOCAL` |
| Storing a value you **received or are also keeping** into a heap object (you create a new owning reference; RC is incremented) | `PyRegion_AddRef` before the store, then `Py_NewRef` |
| Storing several such new references in one logical step | `PyRegion_AddRefs` (fixed count) or `PyRegion_AddRefsArray` (runtime count) |
| Handing off a reference you **already own on the stack** into a heap object — you are not keeping it and the RC does not change (a transfer) | `PyRegion_TakeRef` before the store |
| Replacing a field, stealing the value | `PyRegion_XSETREF` |
| Replacing a field with a **new owning reference** to the value | `PyRegion_XSETNEWREF` |
| `Py_SETREF(field, val)` — old value non-null | `PyRegion_XSETREF` (the X-prefix is harmless on a non-null old value) |
| Clearing a field on a heap object | `PyRegion_CLEAR` |
| `Py_DECREF` on a field in `dealloc` | `PyRegion_RemoveRef(self, field)` before `Py_DECREF` |
| Dispatching through a type slot | `PyRegion_NotifyTypeUse(type)` before the dispatch |
| Reusing/recycling an object as if newly allocated | `PyRegion_RecycleObject(obj)` |

**`AddRef` vs `TakeRef` — decide by what you hold:**
- You received a borrowed argument, or you are storing a value you also keep a
  reference to → you are creating a **new owning reference**: `PyRegion_AddRef`
  (or `PyRegion_XSETNEWREF` for a field replace), followed by `Py_NewRef`.
- You already own a reference on the stack (you created it, or were handed
  ownership) and you are transferring it into a heap object without keeping it
  → `PyRegion_TakeRef`. That is the *only* situation `TakeRef` is for.

**Anti-pattern — never create a local ref just to transfer it.** Do not write
`NewRef`/`AddLocalRef` to make a borrow and then immediately `TakeRef` it:
```c
// WRONG: pointless round-trip — makes a local ref, then converts it
PyObject *newkey = PyRegion_NewRef(key);
if (newkey == NULL) return -1;
if (PyRegion_TakeRef(so, newkey)) {
    PyRegion_RemoveLocalRef(newkey);
    Py_DECREF(newkey);
    return -1;
}
```
If `key` is a value you are storing into `so` as a new owning reference, go
straight there:
```c
// RIGHT: a new owning reference, no local ref at all
if (PyRegion_AddRef(so, key)) {
    return -1;
}
so->slot = Py_NewRef(key);
```
Reach for `TakeRef` only when you genuinely already own the stack reference and
are giving it away.

### The known-local optimization (use carefully)
When you can prove a value is local — for example a list you just created with
`PyList_New` — a reference from it is always allowed, even into an object that
lives in a region. In that case you may replace a full barrier with
`PyRegion_AddLocalRef` plus an `assert`, and replace a barrier-requiring
`Py_DECREF` of a value you *received from a callee* with an
`assert(!PyRegion_NeedsReadBarrier(x))`.

This illustrates how `set_repr_lock_held` is migrated (it shows the intended
result of the migration, which may be ahead of any given checkout of the
source):
```c
while (set_next(so, &pos, &entry)) {
    int res = PyRegion_AddLocalRef(entry->key);
    // entry->key is borrowed via so; so is borrowed, so its region is open,
    // therefore this AddLocalRef always succeeds here.
    assert(res == 0 && "entry->key is borrowed via so; its region is open");
    (void) res;
    PyList_SET_ITEM(keys, idx++, Py_NewRef(entry->key));
}
...
listrepr = PyObject_Repr(keys);
Py_DECREF(keys);   // keys was created here and is always local; no barrier needed
...
listrepr = PyUnicode_Substring(listrepr, 1, PyUnicode_GET_LENGTH(listrepr)-1);
// listrepr came from a callee; assert it does not need region tracking
// (strings are immutable) to document why no RemoveRef is used:
assert(!PyRegion_NeedsReadBarrier(listrepr));
Py_DECREF(listrepr);
```
Two things to note. First, only assert `!PyRegion_NeedsReadBarrier(x)` on
values you *received* (e.g. a callee's return value), not on objects you just
created yourself — a list from `PyList_New` is trivially local, so the assert
would be vacuous and teaches the wrong habit. Second, the `assert` message must
reason about the object the barrier acts on (`entry->key`), not the container.

The fully-safe alternative is always acceptable and is what you should pick if
there is any doubt. Note that when `keys` is local, `PyRegion_AddRef(keys,
entry->key)` is *equivalent* to `PyRegion_AddLocalRef(entry->key)` — adding a
reference from a local object is exactly a local borrow — so this is a valid
barrier here, not an owning cross-region edge:
```c
while (set_next(so, &pos, &entry)) {
    if (PyRegion_AddRef(keys, entry->key)) {  // keys local => same as AddLocalRef
        result = NULL;
        goto done;                            // use the function's own cleanup
    }
    PyList_SET_ITEM(keys, idx++, Py_NewRef(entry->key));
}
```
The failure path uses `goto done` rather than `return NULL`, because the real
`set_repr_lock_held` centralizes cleanup at the `done:` label (it must run
`Py_ReprLeave` before returning). Returning directly would skip that cleanup —
exactly the failure-path symmetry this guide tells you to preserve.

### Handle failure the way the function already does
Every `Add*` / `Take*` / `XSET*` barrier can fail (nonzero, or NULL for the
`NewRef` family). On failure you must **undo what you have done so far and
propagate the error in the same style the function already uses for other
failures** — there is no single universal cleanup. The common shape is: undo
any borrow added (`PyRegion_RemoveLocalRef` / `CLEARLOCAL`), `Py_DECREF`
anything this function incremented, then `return -1` / `return NULL` /
`goto error` as the surrounding code does.

If the function is inside a `Py_BEGIN_CRITICAL_SECTION`, every failure path
must still exit the critical section — prefer `goto error` (which runs the
`Py_END_CRITICAL_SECTION`) over an early `return`.

### Definition of Done for a function
A function is done when **all** of the following hold:

- [ ] Every reference-count operation has a corresponding region barrier,
      **or** an `assert` documenting why the barrier can be skipped.
- [ ] Every exit path (every early `return` and `goto`) is symmetric: borrows
      added are removed, owning references added before a later failure are
      accounted for, and the region counters are left correct.
- [ ] Every called function is already migrated/region-aware, does no RC work,
      or — for dynamic dispatch — is guarded by `NotifyTypeUse`.
- [ ] If inside a critical section, every failure path exits it.
- [ ] No staged-ref pattern was needed without being flagged for human review.

If you cannot satisfy this — ambiguous barrier choice, a function with no error
channel that needs a failing barrier, or an apparent need for staged refs —
**stop and flag the function for human review** with a short explanation rather
than guessing.

---

## Reference: the barriers

All barriers come from `region.h`. `src` is the object the reference is stored
*into*; `tgt` is the object referred *to*.

### Creating references into a heap object

**`PyRegion_AddRef(src, tgt)`** — record a **new** reference from `src` to
`tgt`. Call *before* the `Py_NewRef`/`Py_INCREF`. Returns nonzero on failure.
```c
if (PyRegion_AddRef(self, v)) {
    // cleanup as the function does elsewhere
    return -1;
}
self->value = Py_NewRef(v);
```

**`PyRegion_AddRefs(src, tgt1, tgt2, ...)`** — variadic form for a **fixed**
number of new references added together: succeeds only if a reference to all
targets can be created, otherwise nothing changes. Because owning references
are not reversible, references that must be added as one logical step should
be added with a single call so the group is all-or-nothing.

**`PyRegion_AddRefsArray(src, count, array)`** — same all-or-nothing semantics
as `AddRefs`, but for a count known only at **runtime**: it succeeds if a
reference can be created to every object in `array`, and on failure leaves
everything unchanged. Use this when you need to add a runtime-determined number
of references atomically.

**`PyRegion_TakeRef(src, tgt)`** — transfer an **existing owned** reference
(typically a stack-held borrow) into `src`. Use when the code stores into a
field *without* changing the RC — the reference is handed off, not duplicated.
Returns nonzero on failure.
```c
if (PyRegion_TakeRef(self, newitem)) {
    PyRegion_RemoveLocalRef(newitem);
    Py_DECREF(newitem);
    return -1;
}
self->item = newitem;
```

**`PyRegion_TakeRefs(src, tgt1, ...)`** — variadic, all-or-nothing form of
`TakeRef`. On **success** it adds the owning reference to `src` for *every*
target **and** removes the corresponding local borrow for each — the success
path consumes the local borrows for all targets, so you must not remove them
again. On **failure** it leaves all local refs in place and makes no change,
so the caller must still remove/decref them in its ordinary error path.
```c
if (PyRegion_TakeRefs(self, a, b)) {
    // failure: a and b are still local borrows we own — clean them up
    PyRegion_RemoveLocalRef(a);
    PyRegion_RemoveLocalRef(b);
    Py_DECREF(a);
    Py_DECREF(b);
    return -1;
}
// success: the local borrows for a and b were consumed; do NOT remove them
self->a = a;
self->b = b;
```

### Removing references

**`PyRegion_RemoveRef(src, tgt)`** — remove a reference from `src` to `tgt`.
Call *before* the matching `Py_DECREF`. Does not fail. This is the barrier for
`dealloc` and for hand-written field replacement.
```c
PyObject *old = self->value;
self->value = NULL;
PyRegion_RemoveRef(self, old);
Py_DECREF(old);
```

### Field replacement helpers

**`PyRegion_XSETREF(src, field, val)`** — region-aware `Py_XSETREF` that
**steals** `val`; internally takes the reference to the new value and
removes + `Py_XDECREF`s the old value. Can fail. It is all-or-nothing: on
failure the field and both the old and new references are left unchanged, so
the operation either completes fully or has no side effects.
```c
if (PyRegion_XSETREF(self, self->args, seq)) {
    return -1;
}
```

**`PyRegion_XSETNEWREF(src, field, val)`** — like `XSETREF` but creates a
**new** owning reference to `val` instead of stealing it. Replaces the
`Py_XSETREF(field, Py_NewRef(val))` idiom. `val` may be `NULL` (it internally
uses `Py_XNewRef`). Can fail.
```c
if (PyRegion_XSETNEWREF(self, self->value, value)) {
    return -1;
}
```

**`PyRegion_CLEAR(src, field)`** — region-aware `Py_CLEAR`; removes the
reference and NULLs the field. Does not fail.
```c
PyRegion_CLEAR(self, self->traceback);
```

### Local (stack) reference management

**`PyRegion_NewRef(tgt)` / `PyRegion_XNewRef(tgt)`** — replacements for
`Py_NewRef` / `Py_XNewRef` for the **return/getter** case: a reference being
handed back to the caller. They add a borrow (increment the target region's
LRC) and return a new strong reference. `NewRef` returns NULL on failure, so
where a function has cleanup to do, check it:
```c
return PyRegion_NewRef(self->value);   // simple getter

PyObject *result = PyRegion_NewRef(self->value);
if (!result) {
    // cleanup as the function does elsewhere
    return NULL;
}
return result;
```

Use `NewRef`/`XNewRef` **only for returns.** It bundles "add local ref +
incref" into one call, which is convenient when returning but obscures intent
elsewhere. When you are not returning, keep the two steps visible with
`PyRegion_AddLocalRef` + an explicit `Py_NewRef`/`Py_INCREF` — it reads more
clearly and avoids the temptation to pair `NewRef` with `TakeRef` (see the
anti-pattern under the classification table).

Which to use, in practice:
- `PyRegion_NewRef` — only when returning a reference to the caller.
- `PyRegion_AddLocalRef` + `Py_NewRef` — when storing into an ephemeral local
  container you built (e.g. a freshly created list).
- `PyRegion_AddLocalRef` + `Py_INCREF` — inside macros like `PyList_SET_ITEM`
  that incref without a barrier.

**`PyRegion_AddLocalRef(tgt)`** — add a borrow (increment LRC) without creating
the strong reference for you; use when you `Py_INCREF` manually or via
`Py_NewRef` in a macro like `PyList_SET_ITEM`. Can fail.

**`PyRegion_AddLocalRefs(tgt1, tgt2, ...)`** — variadic, all-or-nothing form
for several local borrows established as one logical step (e.g. a key/value
pair). Prefer it over individual `AddLocalRef` calls when multiple stack refs
are added together: it succeeds only if a borrow can be taken for all targets,
otherwise nothing changes — which avoids awkward partial-failure cleanup where
the first borrow succeeded and the second failed.

**`PyRegion_RemoveLocalRef(tgt)`** — remove a borrow (decrement LRC). Call
*before* the matching `Py_DECREF` on a stack-held value. Does not fail.

**`PyRegion_CLEARLOCAL(local)`** — clears a stack-held local reference (removes
the borrow and DECREFs). Prefer it over a manual `RemoveLocalRef` + `Py_DECREF`
pair where it fits.

### Local replacement helpers

For replacing a value held in a **stack-local** slot (as opposed to a heap
field owned by `src`), use the local replacement helpers rather than
open-coding `RemoveLocalRef` + `Py_XDECREF`:

**`PyRegion_XSETLOCALREF(dst, val)`** — replace local slot `dst` with `val`,
**stealing** `val` (it becomes the new local borrow), and removing the old
local borrow + `Py_XDECREF`ing the old value.

**`PyRegion_XSETLOCALNEWREF(dst, val)`** — like the above but creates a **new**
local borrow to `val` instead of stealing it.

### Skipping a barrier safely

**`PyRegion_NeedsReadBarrier(obj)`** — true if `obj` is in a region and would
need tracking (it is `!(IsLocal || IsImmutable)`). Use it in an `assert` to
document a deliberate skip — on a value you *received*, not one you created:
```c
assert(!PyRegion_NeedsReadBarrier(listrepr));  // strings are immutable
Py_DECREF(listrepr);
```
An assert documents the assumption *and* checks it in debug builds — strictly
better than a comment that can silently rot.

**Immutable values never need a barrier.** This covers:
- Singletons returned via `Py_RETURN_NONE`, `Py_RETURN_TRUE`,
  `Py_RETURN_FALSE`, and `Py_RETURN_NOTIMPLEMENTED` — `Py_None`, `Py_True`,
  `Py_False`, `Py_NotImplemented` are immutable, so no barrier is needed on
  these return paths and they should not be flagged for review.
- Immutable fields in `dealloc`. If a field is known immutable (e.g. an
  interned string), `PyRegion_RemoveRef` is not required before its
  `Py_DECREF`, because immutable objects cannot transfer regions. Skip the
  barrier and optionally assert it: `assert(!PyRegion_NeedsReadBarrier(field))`.

### Dispatch sites

**`PyRegion_NotifyTypeUse(type)`** — call before dispatching through a type
slot to code that may not be region-aware. It internally checks the type's
region-aware flag and, if needed, marks open regions dirty. You do not check
the flag yourself; a redundant call is harmless.

It only needs to guard the dispatch itself; surrounding setup code does not
need to be inside it. Place it immediately before the slot call:
```c
PyRegion_NotifyTypeUse(Py_TYPE(obj));
result = Py_TYPE(obj)->tp_repr(obj);   // the dispatch
```
Common cases that *are* dispatches needing a guard: direct slot calls
(`tp_repr`, `tp_richcompare`, `mp_ass_subscript`, `sq_ass_item`, `tp_iternext`,
etc.). When you call a higher-level helper API instead of a slot directly
(e.g. `PyObject_Repr`, `PyObject_RichCompare`), the helper performs the
dispatch internally; if you cannot determine whether such a helper already
notifies, flag for review rather than guessing — a missed notification is a
correctness bug, and a redundant one is only mildly wasteful.

### Object recycling

**`PyRegion_RecycleObject(obj)`** — call when the runtime reuses an existing
object and wants it to behave as if newly allocated (reset to local-region
state). It appears on object-reuse paths, often alongside GC re-tracking. For
example, the tuple recycle helper:
```c
static inline void
_PyTuple_Recycle(PyObject *op)
{
    _PyTuple_RESET_HASH_CACHE(op);
    if (!_PyObject_GC_IS_TRACKED(op)) {
        _PyObject_GC_TRACK(op);
    }
    PyRegion_RecycleObject(op);
}
```
which is used where a result tuple is reused in place (e.g. `enumerate`'s
fast path) rather than freshly allocated.

---

## Staged references (flag for human review)

`region.h` exposes a staged-reference API: `PyRegion_StageRef(s)` /
`PyRegion_StageRefs(...)`, `PyRegion_CommitStagedRef(...)`, and
`PyRegion_ResetStagedRef(...)`.

These exist for the rare case where several owning references must be added as
one atomic step but cannot be expressed with a single `AddRefs` /
`AddRefsArray` call — for example when the references to add are discovered
across branching control flow, and all must be validated as legal before any
is committed (because owning references are not reversible). Staging lets you
provisionally record the intended references, then either commit them all or
reset (abort) them all.

Staged references are easy to get wrong and are needed only in a small number
of places. **Do not insert them yourself.** Flag the function for human review
if you encounter this shape:
- multiple branches (if/else chains) that conditionally discover references to
  add,
- all discovered references must be validated legal before *any* is committed,
- neither `AddRefs` nor `AddRefsArray` fits because the set of references is
  conditionally determined,
- commonly: a multi-field update where each field's value is conditionally
  computed and all fields must succeed together.

```c
// shape that needs staged refs — do NOT implement, flag for review:
if (cond1) { ref1 = ...; }
if (cond2) { ref2 = ...; }
// both must be validated before either is committed
```
When you see this and a single `AddRefs` / `AddRefsArray` call cannot express
it, flag for human review with a short note explaining why.

---

## Tests

A migration is only trustworthy with tests. The goal is not "write an LRC
test" but "choose tests that correspond to each barrier shape the migration
touched." A type can pass a single success example and still be wrong for
deletes, replacements, cached iterator results, copy paths, default-return
paths, or cleanup after dirtying.

Tests use the `Region` object and its inspection API. Assert on **relative**
counter changes — record before and after the operation under test, never
absolute values. Useful inspection points: `_lrc`, `_osc`, `is_open`,
`is_dirty`, and the helpers `owns(obj)`, `is_local(obj)`, `get_region(obj)`.

### Map tests to the barriers the function used
For each migrated function, look at what it did and test the matching
invariant:

- **`AddRef` / `TakeRef` stores** — assert the destination region owns the
  inserted object afterward (`r.owns(x)`), or that the source region's LRC
  rises when the destination stays local.
- **Replacement / delete / clear paths** — record the target region's `_lrc`,
  perform the replace/delete/clear, and assert the LRC drops by the expected
  relative amount.
- **Getters / returned references** — assert `_lrc` rises while the returned
  local reference is *still alive*, and returns to baseline after it is
  cleared.
- **Iterator / view paths** — assert iterator/view creation cost and per-yield
  cost separately; they can differ, and some iterators cache their result.

### Three operation shapes to distinguish
Final LRC neutrality is not the only thing to check. Be explicit about which
shape an operation has:

- **Neutral** — LRC returns to baseline before the operation returns (e.g.
  `repr`). Example below.
- **Returning-borrow** — LRC stays elevated until the *returned* local
  reference dies. Record a baseline, call the accessor, assert LRC rose while
  the result is held, then clear the result and assert LRC returns to baseline.
- **Transfer** — a local object stops being local and becomes owned by the
  destination region (`assert r.owns(x)` and `assert not is_local(x)`).

### Success-path example (neutral operation)
Records the LRC during `__repr__` to confirm `str(set)` borrows correctly and
leaves the region's LRC unchanged afterward:
```python
from regions import Region

class RecordLrcInRepr:
    def __init__(self, r: Region):
        self.r = r
    def __repr__(self) -> str:
        self.repr_lrc = self.r._lrc
        return "RecordLrcInRepr"

def test_set_repr1(self):
    r = Region()
    x = RecordLrcInRepr(r)
    s = {x}

    base_lrc = r._lrc
    str(s)

    # neutral: LRC back to baseline after the operation completes
    self.assertEqual(r._lrc, base_lrc)
    # but it was elevated during repr, from the borrow on the self reference
    self.assertGreater(x.repr_lrc, r._lrc)
```

### Failure-path example (state unchanged after a failed operation)
Do not test only the happy path. A failed operation must leave ownership,
locality, parent links, dirty flags, and LRC/OSC unchanged or restored:
```python
def test_unchanged_region_after_failure(self):
    r1 = Region()
    r2 = Region()
    a = self.A()
    a.b = self.A()
    a.b.c = self.A()

    r1.c = a.b.c                       # move a.b.c into r1
    self.assertTrue(is_local(a))
    self.assertTrue(is_local(a.b))
    self.assertTrue(r1.owns(a.b.c))

    with self.assertRaises(RuntimeError) as e:  # moving a into r2 fails
        r2.a = a
    self.assertEqual(e.exception.source, a.b)
    self.assertEqual(e.exception.target, a.b.c)

    self.assertTrue(is_local(a))       # failed move changed nothing
    self.assertTrue(is_local(a.b))
```
Match the failure test to the barrier's cleanup risk: a failing `AddRef` /
`TakeRef` *before* a store should leave ownership unchanged; a failure *after*
temporary local refs were created should still return LRC to baseline after the
exception; a failure inside a critical section or repr/iterator helper should
leave the public operation retryable and not leave dirty/open state behind
(unless dirtying is the expected behavior).

### Cover object shapes, not just one value
For container-like types, the barrier behavior differs by element kind. Cover
at least:
- mutable local elements (should transfer into a region),
- already region-owned elements (should become local borrows when returned),
- bridge/subregion references when the operation can nest or un-nest regions.

### Iterators and views
Iterators and views are easy places to get local refs wrong. Record the
baseline before creating the iterator/view, assert the creation-time LRC cost,
assert the per-yield LRC cost, and check what happens when the yielded value
and the iterator are released. Note some iterators intentionally do not reset
fully when the loop variable is cleared because they cache the result object
(e.g. dict item iteration caching its tuple) — assert the behavior the
implementation actually intends.

### Running tests
Use focused tests while migrating, then run the whole package before marking
the type region-aware:
```bash
# whole region test package — run before marking a type done
./python.exe -m test test_regions

# a single module while working on a type
./python.exe -m unittest Lib.test.test_regions.test_dict

# a single test while iterating
./python.exe -m unittest Lib.test.test_regions.test_core.TestInterRegionRelations.test_unchanged_region_after_failure
```
If tests fail, consider both possibilities: the test may be wrong, or the
migration may be. A type is only done when the full `test_regions` package
passes with the type marked `Py_TPFLAGS2_REGION_AWARE`.
