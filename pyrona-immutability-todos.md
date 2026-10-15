# Pyrona immutability: open TODOs

Open items from the review of the PEP 795 implementation (`ec298101d75`) that
still need fixing or discussion. Line numbers are approximate; function names
are authoritative.

Legend:
- **Confirmed**: reproduced on a debug build.
- **High**: by code reading, high confidence.
- **Plausible**: needs further investigation.
- Builds: `[GIL]` = `_Py_PYRONA_INTERPRETER_SHARING`, `[FT]` = `Py_GIL_DISABLED`, `[both]`.

---

## General

- [ ] **Immortality detection is flag-only** Confirmed
  - `[FT]` `_Py_NeedsSlowRcBranch` (`Include/refcount.h`) tests `_Py_IMMORTAL_FLAGS`,
    which FT never sets (`_Py_SetImmortalUntracked` only sets `ob_ref_local`).
    - `Py_SET_REFCNT` no longer ignores immortals: `_Py_SetRefcnt(True, 3)` hits
      `assert(_Py_IsImmortal(res))` in `Objects/object.c`.
    - `_Py_RefcntAdd` (`pycore_object.h`) atomically adds to `ob_ref_shared` of
      `None`/small ints (contention).
  - `[GIL]` `Py_INCREF`/`Py_DECREF` dropped the `ob_refcnt >= 2^31` check. Objects made
    immortal by saturation, `Py_SET_REFCNT`, or old stable-ABI extensions can wrap and
    be freed while live. Loses the resilience documented at the top of `refcount.h`.
    ```python
    import ctypes, sys
    f = ctypes.pythonapi._Py_SetRefcnt
    f.argtypes = [ctypes.c_void_p, ctypes.c_ssize_t]; f.restype = None
    o = [1, 2, 3]
    f(id(o), 0xFFFFFFFF)   # upstream: treated as immortal from now on
    l = [o]; l2 = [o]; l3 = [o]
    del l2, l3, l
    print("alive", o)      # use-after-free here
    ```

- [ ] **Stable ABI / limited API break** Confirmed (compile + `nm`)
  - With `Py_LIMITED_API < 0x030d0000`, inline `Py_SET_REFCNT` references
    `_Py_LoadOpFlags`, `_Py_FatalErrorFunc`; `< 0x030c0000` also `Py_INCREF`/`Py_DECREF`.
    Neither symbol is in `Misc/stable_abi.toml` / `PC/python3dll.c`.
  - `PyDict_Clear` changed `void` → `int` (stable ABI); ~10 internal callers ignore the
    result (e.g. `Python/_warnings.c`, `Modules/_functoolsmodule.c`).

- [ ] **Debug reftotal drift; `-R` false positives** Confirmed
  - Slow paths skip `_Py_INCREF_IncRefTotal` / `_Py_DECREF_DecRefTotal`
    (`Py_INCREF`/`Py_DECREF`, `_Py_SlowDecRef`, `Py_DECREF_MORTAL`) but
    `_Py_DECREF_MORTAL_SPECIALIZED` counts them.
  - `./python -m test test_freeze.test_refcount -R 3:3` → "leaked [72, 72, 72] references"
    while allocated blocks stay flat.

- [ ] **Smaller refcount / GC items** Plausible
  - `_PyObject_GC_UNTRACK` silently accepts untracked objects in all GIL builds
    (`pycore_gc.h`), hiding real double-untrack bugs.
  - `_Py_RESET_IMMUTABLE` before `tp_dealloc`: an object resurrected by `__del__`
    comes back mutable.
  - `Py_SET_REFCNT` on a deep-immutable object is a fatal error in release builds.
  - Dead code: `_Py_PyronaIgnoreObj` in `Python/gc.c`.

- [ ] **`SharedField` cycles invisible to GC** Plausible
  - `sharedfield_traverse` / `sharedfield_reachable` (`Modules/_immutablemodule.c`) don't
    visit `value`; `t = deep_freeze((sf,)); sf.set(t)` → permanent leak.
  - Fix direction: on FT make sure the GC sees it; with interpreter sharing this is a
    known limitation.

- [ ] **Module state is returned as a borrowed reference** High
  - `_PyInterpreterState_GetModuleState` (`Python/pystate.c`) returns the mutable module
    kept alive only by `sys.mut_modules`. Deleting the entry while a caller still uses
    the state is a use-after-free. Fixing it means returning a strong reference and
    updating ~130 call sites (incl. `_PyModule_GetDict` / `_PyModule_GetState`).

- [ ] **Clearing `sys.mut_modules` after freezing `sys` aborts at shutdown** Confirmed
  - An exception is left set during finalization and `gc_collect_main` asserts
    `!_PyErr_Occurred(tstate)`:
    ```python
    import sys, _frozen_importlib_external, _thread, _warnings, _weakref
    from immutable import deep_freeze
    for m in (sys, _frozen_importlib_external, _thread, _warnings, _weakref):
        deep_freeze(m)
    sys.mut_modules.clear()
    ```

- [ ] **`Lib/_collections_abc.py`: `MAX = sys.maxsize`** High
  - Becomes a public `Set.MAX` on every Set/MutableSet subclass. The motivation doesn't
    hold (`_hash.__globals__` already holds `sys`; the TODO admits it).

- [ ] **Minor** High
  - `late_init` (`Python/immutability.c`) sets `late_init_done` before doing the work;
    concurrent FT freezes (and all freezes if the import fails) can freeze
    `_module_locks` / `_blocking_on`.
  - `nroots` passed as `int` from `Py_ssize_t`.
  - Unused `freeze_stack` field (`pycore_immutability.h`).
  - `set_freeze_location` (Py_DEBUG) calls user `__setattr__` mid-freeze.

---

## Freezability

- [ ] **Per-object `set_freezable` ignored when the class has `__freezable__`** Confirmed
  - `PyObject_GetOptionalAttr(obj, "__freezable__")` finds the class attribute, so the
    `ob_flags` fallback is never consulted.
    ```python
    import immutable as im
    class C:
        __slots__ = ()
    im.set_freezable(C, im.FREEZABLE_NO)
    c = C()
    im.set_freezable(c, im.FREEZABLE_YES)
    im.deep_freeze(c)   # TypeError, although c is marked YES
    ```

- [ ] **Invalid `__freezable__` values** Confirmed
  - -2 collides with the "lookup failed" code → error without exception (fatal in
    debug).
  - `(int)PyLong_AsLong` truncates: `2**32` → YES.
  - -1 is treated as "not set".
    ```python
    import immutable as im
    for v in (-2, 2**32, -1):
        class D: pass
        D.__freezable__ = v
        im.deep_freeze(D())
    ```

- [ ] **`__getattr__` catch-all objects can't be frozen** Confirmed
  - The `__freezable__` lookup goes through `__getattr__`; a non-int result aborts the
    freeze. User code also runs for every object visited.
    ```python
    import immutable as im
    class Proxy:
        def __getattr__(self, name): return "anything"
    im.deep_freeze(Proxy())   # fails
    ```

- [ ] **Function pre-freeze hook** `[both]` Confirmed (except leak)
  - `func_prefreeze_shadow_captures` (`Objects/funcobject.c`) copies only the function's
    own `co_names` into the shadow globals:
    - nested lambdas/defs/comprehensions using globals or builtins → `NameError`;
    - `import os` → `ImportError: __import__ not found`;
    - `warnings.warn` → `TypeError` (can't set `__warningregistry__`);
    - `global X; X = 1` → `TypeError`.
    ```python
    from immutable import deep_freeze
    def h():
        return (lambda: len)()
    deep_freeze(h)
    h()   # NameError: len
    ```
  - Sets `func_annotations = {}` when NULL → deferred annotations are lost:
    ```python
    def f(x: int) -> str: pass
    deep_freeze(f)
    f.__annotations__   # {}
    ```
  - Leaks: `PyDict_SetItem(shadow_globals, Py_NewRef(name), Py_NewRef(value))`,
    `PyDict_SetItemString(..., Py_NewRef(shadow_builtins))` (High).
  - Error handling: `PyDict_Contains == -1` treated as true, then a NULL from
    `PyDict_GetItem` goes into `Py_NewRef`; unchecked `PyCell_New`; borrowed
    `PyDict_GetItem` is unsafe on FT.

---

## Interpreters / SCC

- [ ] **`Py_REFCNT` / `sys.getrefcount` wrong for SCC members** `[GIL]` Confirmed
  - `scc_complete` leaves stale member counts. Frozen `a`↔`b` cycle plus 100 refs to `b`:
    `getrefcount(a) == 103`, `getrefcount(b) == 3`. Breaks `Py_REFCNT(x) == 1` fast paths.

- [ ] **Finalizer resurrection during SCC teardown** `[GIL]` Confirmed
  - `scc_unfreeze_and_finalize` runs finalizers, then `tp_clear` on all members without
    a resurrection check (cf. GC `handle_resurrected_objects`). The resurrected object
    has `__dict__ == {}` and is no longer frozen.
    ```python
    import immutable as im, sys, gc
    class R:
        def __del__(self): sys._box.append(self)
    class P: pass
    sys._box = []
    r = R(); r.data = "payload"; r.other = P(); r.other.back = r
    im.deep_freeze(r)
    del r; gc.collect()
    x = sys._box[0]
    print(im.is_deep_frozen(x), x.__dict__)   # False {}
    ```

- [ ] **Weakrefs live during SCC finalization** `[GIL]` Plausible
  - `scc_make_mutable` clears flags before `scc_call_finalizers`; `scc_clear_weakrefs`
    runs after. Another interpreter's plain weakref sees RC 1 (not the
    `_Py_TryIncref_Immutable` path) and can grab a soon-to-be-cleared object.
    Same for the single-object path (→ `_Py_Dealloc`).

- [ ] **Weakref-callback abort paths touch foreign weakrefs** `[GIL]` Plausible
  - If `_PyEval_AddPendingCall` fails (queue max 300, or target finalizing) or
    `PyMem_RawMalloc` fails, `weakref_decref_weakrefs` `Py_DECREF`s other-interpreter
    weakrefs under the wrong GIL, and callbacks are silently dropped.
    `_PyImmutability_ClearWeakRefsWithCallback` also `Py_INCREF`s foreign weakrefs.
  - Trigger without OOM: >300 frozen objects with callbacks registered in B dying in A.

- [ ] **Cross-interpreter refcount and finalizer details** `[GIL]` Plausible
  - `Py_INCREF` reads `ob_refcnt_full` non-atomically (`refcount.h`) while other
    interpreters RMW it (TSan will flag).
  - `__del__` of a frozen object runs in whichever interpreter dropped the last ref.

- [ ] **Frozen objects with lazily created dicts** `[GIL]` Plausible
  - `_PyObject_MaterializeManagedDict_LockHeld` and `ensure_nonmanaged_dict` create and
    freeze a new dict for frozen objects: cross-interpreter race (TODO acknowledges).

- [ ] **Frozen extension instances across interpreters** `[GIL]` Plausible
  - A frozen `decimal.Decimal` shared to interpreter B: `d + d` → "unsupported operand
    type(s)". Methods reaching module state from B touch A's mutable objects.
    ```python
    import immutable, _interpreters, decimal
    d = decimal.Decimal('1.5')
    immutable.deep_freeze(d)
    interp = _interpreters.create()
    _interpreters.set___main___attrs(interp, {'d': d})
    _interpreters.run_string(interp, 'print(d + d)')
    ```
