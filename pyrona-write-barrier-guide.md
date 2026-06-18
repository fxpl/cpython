# Lungfish Write Barriers — Migration Guide

This document explains how to insert region write barriers into CPython C
code to make a type region-aware. It is written to be supplied to an LLM that
migrates a type, working through it **one function at a time**.

It assumes the **Lungfish Region Model — Conceptual Reference** is provided
alongside it. Terms like *local region*, *borrow*, *LRC*, *OSC*, *dirty*,
*bridge object*, *immutable*, and *closed* are defined there and are not
re-explained here.

This document is the operational reference. For anything not covered here,
ask rather than infer — do not invent runtime APIs, exception types, or
barrier behavior that is not stated.

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
   into the local region. Borrows are reversible and can later be turned into
   an owning reference with `TakeRef`.

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
Place a `// TODO` comment at the top of the spec, just under the first
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

### Step 4 — Mark the type done
Once every function in the spec (and its sub-structs) reaches DOD, mark the
type region-aware by adding:
```c
.tp_flags2 = Py_TPFLAGS2_REGION_AWARE
```
Until this flag is set, dispatching to the type marks regions dirty (safe, but
defeats the purpose during testing). Setting the flag does not change runtime
correctness — it only stops the dirty-marking — so it is fine to set during
development as long as the finished result is reviewed by a human before
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
| Storing a **new** reference into a heap object (RC incremented) | `PyRegion_AddRef` before the store |
| Storing several new references in one logical step | `PyRegion_AddRefs` (fixed count) or `PyRegion_AddRefsArray` (runtime count) |
| Storing an **owned** reference into a heap object (RC unchanged — transfer) | `PyRegion_TakeRef` before the store |
| Replacing a field, stealing the value | `PyRegion_XSETREF` |
| Replacing a field, borrowing the value (new ref) | `PyRegion_XSETNEWREF` |
| Clearing a field on a heap object | `PyRegion_CLEAR` |
| `Py_DECREF` on a field in `dealloc` | `PyRegion_RemoveRef(self, field)` before `Py_DECREF` |
| Dispatching through a type slot | `PyRegion_NotifyTypeUse(type)` before the dispatch |
| Reusing/recycling an object as if newly allocated | `PyRegion_RecycleObject(obj)` |

### The known-local optimization (use carefully)
When you can prove a value is local — for example a list you just created with
`PyList_New` — a reference from it is always allowed, even into an object that
lives in a region. In that case you may replace a full barrier with
`PyRegion_AddLocalRef` plus an `assert`, and replace a barrier-requiring
`Py_DECREF` with an `assert(!PyRegion_NeedsReadBarrier(x))`.

This is exactly what the migrated `set_repr_lock_held` does:
```c
while (set_next(so, &pos, &entry)) {
    int res = PyRegion_AddLocalRef(entry->key);
    assert(res == 0 && "keys is local, this should always succeed");
    (void) res;
    PyList_SET_ITEM(keys, idx++, Py_NewRef(entry->key));
}
...
listrepr = PyObject_Repr(keys);
assert(!PyRegion_NeedsReadBarrier(keys));   // keys is local
Py_DECREF(keys);
```
The fully-safe alternative is always acceptable and is what you should pick if
there is any doubt:
```c
while (set_next(so, &pos, &entry)) {
    if (PyRegion_AddRef(keys, entry->key)) {
        PyRegion_CLEARLOCAL(keys);
        return NULL;
    }
    PyList_SET_ITEM(keys, idx++, Py_NewRef(entry->key));
}
```

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

**`PyRegion_TakeRefs(src, tgt1, ...)`** — variadic form of `TakeRef`.

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
removes + `Py_XDECREF`s the old value. Can fail.
```c
if (PyRegion_XSETREF(self, self->args, seq)) {
    return -1;
}
```

**`PyRegion_XSETNEWREF(src, field, val)`** — like `XSETREF` but creates a
**new** reference to `val` instead of stealing it. Replaces the
`Py_XSETREF(field, Py_NewRef(val))` idiom. Can fail.
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
`Py_NewRef` / `Py_XNewRef` when the new reference is **kept on the C stack or
returned to the caller**. They add a borrow (increment the target region's
LRC) and return a new strong reference. `NewRef` returns NULL on failure.
```c
return PyRegion_NewRef(self->value);   // getter returning a field
```

**`PyRegion_AddLocalRef(tgt)`** — add a borrow (increment LRC) without creating
the strong reference for you; use when you `Py_INCREF` manually or via
`Py_NewRef` in a macro like `PyList_SET_ITEM`. Can fail.

**`PyRegion_RemoveLocalRef(tgt)`** — remove a borrow (decrement LRC). Call
*before* the matching `Py_DECREF` on a stack-held value. Does not fail.

**`PyRegion_CLEARLOCAL(local)`** — clears a stack-held local reference (removes
the borrow and DECREFs). Prefer it over a manual `RemoveLocalRef` + `Py_DECREF`
pair where it fits.

### Skipping a barrier safely

**`PyRegion_NeedsReadBarrier(obj)`** — true if `obj` is in a region and would
need tracking. Use it in an `assert` to document a deliberate skip:
```c
assert(!PyRegion_NeedsReadBarrier(listrepr));  // strings are immutable
Py_DECREF(listrepr);
```
An assert documents the assumption *and* checks it in debug builds — strictly
better than a comment that can silently rot.

### Dispatch sites

**`PyRegion_NotifyTypeUse(type)`** — call before dispatching through a type
slot to code that may not be region-aware. It internally checks the type's
region-aware flag and, if needed, marks open regions dirty. You do not check
the flag yourself; a redundant call is harmless.

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
of places. **Do not insert them yourself.** If a function appears to genuinely
require staged references — typically a multi-field update where the values are
gathered conditionally and must all succeed together, and neither `AddRefs` nor
`AddRefsArray` fits — **flag the function for human review** with a short note
explaining why.

---

## Tests

A migration is only trustworthy with tests. Tests use the `Region` object and
its `_lrc` counter, and they assert on **relative** LRC changes — record the
LRC before and after the operation under test, never absolute values.

### Success-path test (LRC behaves as expected)
This records the LRC during `__repr__` to confirm that converting a set to a
string borrows correctly and leaves the region's LRC unchanged afterward:
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

    # LRC is unchanged after the operation completes
    self.assertEqual(r._lrc, base_lrc)
    # LRC was higher during repr, because of the borrow taken on the self ref
    self.assertGreater(x.repr_lrc, r._lrc)
```

### Failure-path test (state unchanged after a failed operation)
Do not test only the happy path. Failed operations must leave region state
untouched:
```python
def test_unchanged_region_after_failure(self):
    r1 = Region()
    r2 = Region()
    a = self.A()
    a.b = self.A()
    a.b.c = self.A()

    # Move a.b.c into r1
    r1.c = a.b.c
    self.assertTrue(is_local(a))
    self.assertTrue(is_local(a.b))
    self.assertTrue(r1.owns(a.b.c))

    # Moving a into r2 fails: a.b.c is already in another region
    with self.assertRaises(RuntimeError) as e:
        r2.a = a
    self.assertEqual(e.exception.source, a.b)
    self.assertEqual(e.exception.target, a.b.c)

    # a and a.b must remain local — the failed move changed nothing
    self.assertTrue(is_local(a))
    self.assertTrue(is_local(a.b))
```

### Running tests
Once all functions of the type are migrated, the tests should pass. If they
fail, consider both possibilities: the test may be wrong, or the migration may
be. Always cover failure paths, not just success.
