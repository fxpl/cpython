#ifndef _Py_REFCOUNT_H
#define _Py_REFCOUNT_H
#ifdef __cplusplus
extern "C" {
#endif


/*
Immortalization:

The following indicates the immortalization strategy depending on the amount
of available bits in the reference count field. All strategies are backwards
compatible but the specific reference count value or immortalization check
might change depending on the specializations for the underlying system.

Proper deallocation of immortal instances requires distinguishing between
statically allocated immortal instances vs those promoted by the runtime to be
immortal. The latter should be the only instances that require
cleanup during runtime finalization.
*/

#if SIZEOF_VOID_P > 4
/*
In 64+ bit systems, any object whose 32 bit reference count is >= 2**31
will be treated as immortal.

Using the lower 32 bits makes the value backwards compatible by allowing
C-Extensions without the updated checks in Py_INCREF and Py_DECREF to safely
increase and decrease the objects reference count.

In order to offer sufficient resilience to C extensions using the stable ABI
compiled against 3.11 or earlier, we set the initial value near the
middle of the range (2**31, 2**32). That way the refcount can be
off by ~1 billion without affecting immortality.

Reference count increases will use saturated arithmetic, taking advantage of
having all the lower 32 bits set, which will avoid the reference count to go
beyond the refcount limit. Immortality checks for reference count decreases will
be done by checking the bit sign flag in the lower 32 bits.

To ensure that once an object becomes immortal, it remains immortal, the threshold
for omitting increfs is much higher than for omitting decrefs. Consequently, once
the refcount for an object exceeds _Py_IMMORTAL_MINIMUM_REFCNT it will gradually
increase over time until it reaches _Py_IMMORTAL_INITIAL_REFCNT.
*/
#define _Py_IMMORTAL_INITIAL_REFCNT (3ULL << 30)
#define _Py_IMMORTAL_MINIMUM_REFCNT (1ULL << 31)
#define _Py_STATIC_FLAG_BITS ((Py_ssize_t)(_Py_STATICALLY_ALLOCATED_FLAG | _Py_IMMORTAL_FLAGS))
#define _Py_STATIC_IMMORTAL_INITIAL_REFCNT (((Py_ssize_t)_Py_IMMORTAL_INITIAL_REFCNT) | (_Py_STATIC_FLAG_BITS << 48))

#else
/*
In 32 bit systems, an object will be treated as immortal if its reference
count equals or exceeds _Py_IMMORTAL_MINIMUM_REFCNT (2**30).

Using the lower 30 bits makes the value backwards compatible by allowing
C-Extensions without the updated checks in Py_INCREF and Py_DECREF to safely
increase and decrease the objects reference count. The object would lose its
immortality, but the execution would still be correct.

Reference count increases and decreases will first go through an immortality
check by comparing the reference count field to the minimum immortality refcount.
*/
#define _Py_IMMORTAL_INITIAL_REFCNT ((Py_ssize_t)(5L << 26))
#define _Py_IMMORTAL_MINIMUM_REFCNT ((Py_ssize_t)(1L << 28))
#define _Py_STATIC_IMMORTAL_INITIAL_REFCNT ((Py_ssize_t)(7L << 26))
#define _Py_STATIC_IMMORTAL_MINIMUM_REFCNT ((Py_ssize_t)(6L << 26))
#endif

/*
Immutability:
    The immutability status of an object lives in ob_flags, which only has spare
    bits on 64 bit builds. There is no 32 bit fallback.
Immutability Flags:
    1. Mutability:
       - 0: The object is mutable
       - 1: The object is immutable
    2. Depth:
       - 0: The object is shallow immutable, but it could be deeply immutable
       - 1: The object is deeply immutable
Immutability Convenience Flags:
    These are flags that we store in the object for convenience, but they could
    be stored somewhere else to regain these bits.
    3 & 4: The freezability of this specific object.
        - See _Py_freezable_status for values
    5: Has the freezability been explicitly set
        - 0: Not explicitly set
        - 1: Explicitly set
    6: Pre-Freeze Hook
        - 0: The pre-freeze hook did not run yet
        - 1: The pre-freeze hook did run
Sub-Interpreter/GIL-enabled Specific flags:
    7. SCC
        - 0 -> Direct: The object is immutable and it has the reference count
        - 1 -> Indirect: The object is immutable and is part of an SCC, and another
            object in the SCC carries the reference count.
    8. Atomic RC:
        - 0: The object is normally reference counted
        - 1: The object uses atomic reference counting (Or maybe SCC reference counting)
 */
#if SIZEOF_VOID_P <= 4
#  error "Immutability currently only works on 64bit platforms"
#endif

// ob_flags bits 4-9 are owned by the immutability system; see Include/object.h
// for the flags upstream keeps in bits 0-3.
#define _Py_IMM_FLAGS_SHIFT(x) (x + 3)
// Immutability Flags:
#define _Py_IMMUTABLE_FLAG (1 << _Py_IMM_FLAGS_SHIFT(1))
#define _Py_IMMUTABLE_DEPTH_FLAG (1 << _Py_IMM_FLAGS_SHIFT(2))

// Immutability Convenience Flags:
#define _Py_FREEZABLE_STATUS_SHIFT _Py_IMM_FLAGS_SHIFT(3)
#define _Py_FREEZABLE_STATUS_MASK (0x3 << _Py_FREEZABLE_STATUS_SHIFT)
#define _Py_FREEZABLE_SET_FLAG (1 << _Py_IMM_FLAGS_SHIFT(5))
#define _Py_PREFREEZE_RAN_FLAG (1 << _Py_IMM_FLAGS_SHIFT(6))

// Sub-Interpreter/GIL-enabled Specific flags:
#ifndef Py_GIL_DISABLED
#define _Py_PYRONA_INTERPRETER_SHARING
#endif

#ifdef _Py_PYRONA_INTERPRETER_SHARING
#define _Py_IMMUTABLE_SCC_FLAG (1 << _Py_IMM_FLAGS_SHIFT(7))
#define _Py_ATOMIC_RC_FLAG (1 << _Py_IMM_FLAGS_SHIFT(8))
#define _Py_IMMUTABLE_MASK (_Py_IMMUTABLE_FLAG | _Py_IMMUTABLE_SCC_FLAG)
// The immutability state proper. Cleared whenever an object stops being
// immutable, including when a partial freeze is rolled back.
#define _Py_IMMUTABLE_CLEAR_MASK \
    (_Py_IMMUTABLE_MASK | _Py_IMMUTABLE_DEPTH_FLAG | _Py_ATOMIC_RC_FLAG)
// Additionally drops the per-object freeze bookkeeping. Only valid once the
// object is dead; a rollback has to keep those bits, or the pre-freeze hook
// runs a second time on the next freeze attempt.
#define _Py_IMMUTABLE_RESET_MASK ( \
    _Py_IMMUTABLE_CLEAR_MASK | _Py_FREEZABLE_STATUS_MASK \
    | _Py_FREEZABLE_SET_FLAG | _Py_PREFREEZE_RAN_FLAG)
#define _Py_IMMUTABLE_DIRECT (_Py_IMMUTABLE_FLAG)
#define _Py_IMMUTABLE_INDIRECT (_Py_IMMUTABLE_FLAG | _Py_IMMUTABLE_SCC_FLAG)
#define _Py_IMMUTABLE_PENDING (_Py_IMMUTABLE_SCC_FLAG)

PyAPI_FUNC(int) _Py_DecRef_Immutable(PyObject *op);
PyAPI_FUNC(void) _Py_RefcntAdd_Immutable(PyObject *op, Py_ssize_t n);

#else // _Py_PYRONA_INTERPRETER_SHARING
// FIXME(immutability): These should probably be removed.
#define _Py_IMMUTABLE_MASK (_Py_IMMUTABLE_FLAG)
#define _Py_IMMUTABLE_CLEAR_MASK (_Py_IMMUTABLE_MASK | _Py_IMMUTABLE_DEPTH_FLAG)
#define _Py_IMMUTABLE_RESET_MASK ( \
    _Py_IMMUTABLE_CLEAR_MASK | _Py_FREEZABLE_STATUS_MASK \
    | _Py_FREEZABLE_SET_FLAG | _Py_PREFREEZE_RAN_FLAG)
#endif // _Py_PYRONA_INTERPRETER_SHARING


// Py_GIL_DISABLED builds indicate immortal objects using `ob_ref_local`, which is
// always 32-bits.
#ifdef Py_GIL_DISABLED
#define _Py_IMMORTAL_REFCNT_LOCAL UINT32_MAX
#endif


#ifdef Py_GIL_DISABLED
   // The shared reference count uses the two least-significant bits to store
   // flags. The remaining bits are used to store the reference count.
#  define _Py_REF_SHARED_SHIFT        2
#  define _Py_REF_SHARED_FLAG_MASK    0x3

   // The shared flags are initialized to zero.
#  define _Py_REF_SHARED_INIT         0x0
#  define _Py_REF_MAYBE_WEAKREF       0x1
#  define _Py_REF_QUEUED              0x2
#  define _Py_REF_MERGED              0x3

   // Create a shared field from a refcnt and desired flags
#  define _Py_REF_SHARED(refcnt, flags) \
              (((refcnt) << _Py_REF_SHARED_SHIFT) + (flags))
#endif  // Py_GIL_DISABLED

PyAPI_FUNC(uint16_t) _Py_LoadOpFlags(PyObject *);
PyAPI_FUNC(void) _Py_OpFlagsAdd(PyObject *, uint16_t);
PyAPI_FUNC(void) _Py_OpFlagsRmv(PyObject *, uint16_t);

#ifdef Py_LIMITED_API
#define _Py_OB_FLAG_ADD(op, flag) _Py_OpFlagsAdd(_PyObject_CAST(op), flag);
#define _Py_OB_FLAG_REMOVE(op, flag) _Py_OpFlagsRmv(_PyObject_CAST(op), flag);
#define _Py_OB_FLAGS_LOAD(op) _Py_LoadOpFlags(_PyObject_CAST(op))
#else // !Py_LIMITED_API
#define _Py_OB_FLAGS_LOAD(op) _Py_atomic_load_uint16_relaxed(&(op)->ob_flags)

#ifdef Py_GIL_DISABLED
#define _Py_OB_FLAG_ADD(op, flag) _Py_atomic_or_uint16(&_PyObject_CAST(op)->ob_flags, (flag))
#define _Py_OB_FLAG_REMOVE(op, flag) _Py_atomic_and_uint16(&_PyObject_CAST(op)->ob_flags, ~(flag))
#else // !Py_GIL_DISABLED
#define _Py_OB_FLAG_ADD(op, flag) _PyObject_CAST(op)->ob_flags |= (flag)
#define _Py_OB_FLAG_REMOVE(op, flag) _PyObject_CAST(op)->ob_flags &= ~(flag)
#endif // Py_GIL_DISABLED

#endif // Py_LIMITED_API


static inline Py_ALWAYS_INLINE int _Py_IsShallowImmutable(PyObject *op)
{
    return (_Py_OB_FLAGS_LOAD(op) & _Py_IMMUTABLE_MASK) != 0;
}
#define _Py_IsShallowImmutable(op) _Py_IsShallowImmutable(_PyObject_CAST(op))

static inline Py_ALWAYS_INLINE int _Py_IsDeepImmutable(PyObject *op)
{
    return (_Py_OB_FLAGS_LOAD(op) & _Py_IMMUTABLE_DEPTH_FLAG) != 0;
}
#define _Py_IsDeepImmutable(op) _Py_IsDeepImmutable(_PyObject_CAST(op))

#ifdef _Py_PYRONA_INTERPRETER_SHARING
static inline Py_ALWAYS_INLINE int _Py_NeedsAtomicRC(PyObject *op)
{
    return (_Py_OB_FLAGS_LOAD(op) & (_Py_ATOMIC_RC_FLAG | _Py_IMMUTABLE_DEPTH_FLAG)) != 0;
}
#define _Py_NeedsAtomicRC(op) _Py_NeedsAtomicRC(_PyObject_CAST(op))

static inline Py_ALWAYS_INLINE int _Py_NeedsImmutableRC(PyObject *op)
{
    return (_Py_OB_FLAGS_LOAD(op) & _Py_IMMUTABLE_DEPTH_FLAG) != 0;
}
#define _Py_NeedsImmutableRC(op) _Py_NeedsImmutableRC(_PyObject_CAST(op))

static inline Py_ALWAYS_INLINE int _Py_NeedsSlowRcBranch(PyObject *op)
{
    // TODO(xFrednet): _Py_IMMUTABLE_MASK should be removed from this
    return (_Py_OB_FLAGS_LOAD(op) & (_Py_IMMORTAL_FLAGS | _Py_ATOMIC_RC_FLAG | _Py_IMMUTABLE_DEPTH_FLAG)) != 0;
}
#define _Py_NeedsSlowRcBranch(op) _Py_NeedsSlowRcBranch(_PyObject_CAST(op))

#else // _Py_PYRONA_INTERPRETER_SHARING

static inline Py_ALWAYS_INLINE int _Py_NeedsSlowRcBranch(PyObject *op)
{
    // On Free-threading only immortal objects require a slow RC branch
    return (_Py_OB_FLAGS_LOAD(op) & _Py_IMMORTAL_FLAGS) != 0;
}
#define _Py_NeedsSlowRcBranch(op) _Py_NeedsSlowRcBranch(_PyObject_CAST(op))

#endif // _Py_PYRONA_INTERPRETER_SHARING

// Artifact[Implementation]: The definition of the `Py_CHECKWRITE` macro
// Check whether an object is writeable.
// This check will always succeed during runtime finalization.
#define Py_CHECKWRITE(op) ((op) && (!_Py_IsShallowImmutable(op) || _PyImmModule_Check(op) || Py_IsFinalizing()))
#define Py_REQUIREWRITE(op, msg) {if (Py_CHECKWRITE(op)) { _PyObject_ASSERT_FAILED_MSG(op, msg); }}

static inline Py_ALWAYS_INLINE void _Py_CLEAR_IMMUTABLE(PyObject *op)
{
    _Py_OB_FLAG_REMOVE(op, _Py_IMMUTABLE_CLEAR_MASK);
}

// Only for objects that are being deallocated: also drops the freeze
// bookkeeping so a recycled allocation starts from a clean slate.
static inline Py_ALWAYS_INLINE void _Py_RESET_IMMUTABLE(PyObject *op)
{
    _Py_OB_FLAG_REMOVE(op, _Py_IMMUTABLE_RESET_MASK);
}

// Py_REFCNT() implementation for the stable ABI
PyAPI_FUNC(Py_ssize_t) Py_REFCNT(PyObject *ob);

#if defined(Py_LIMITED_API) && Py_LIMITED_API+0 >= 0x030e0000
    // Stable ABI implements Py_REFCNT() as a function call
    // on limited C API version 3.14 and newer.
#else
    static inline Py_ssize_t _Py_REFCNT(PyObject *ob) {
    #if !defined(Py_GIL_DISABLED)
        return ob->ob_refcnt;
    #else
        uint32_t local = _Py_atomic_load_uint32_relaxed(&ob->ob_ref_local);
        if (local == _Py_IMMORTAL_REFCNT_LOCAL) {
            return _Py_IMMORTAL_INITIAL_REFCNT;
        }
        Py_ssize_t shared = _Py_atomic_load_ssize_relaxed(&ob->ob_ref_shared);
        return _Py_STATIC_CAST(Py_ssize_t, local) +
               Py_ARITHMETIC_RIGHT_SHIFT(Py_ssize_t, shared, _Py_REF_SHARED_SHIFT);
    #endif
    }
    #if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 < 0x030b0000
    #  define Py_REFCNT(ob) _Py_REFCNT(_PyObject_CAST(ob))
    #endif
#endif

#ifndef _Py_OPAQUE_PYOBJECT
static inline Py_ALWAYS_INLINE int _Py_IsImmortal(PyObject *op)
{
#if defined(Py_GIL_DISABLED)
    return (_Py_atomic_load_uint32_relaxed(&op->ob_ref_local) ==
            _Py_IMMORTAL_REFCNT_LOCAL);
#elif SIZEOF_VOID_P > 4
    return _Py_CAST(PY_INT32_T, op->ob_refcnt) < 0;
#else
    return op->ob_refcnt >= _Py_IMMORTAL_MINIMUM_REFCNT;
#endif
}
#define _Py_IsImmortal(op) _Py_IsImmortal(_PyObject_CAST(op))

static inline Py_ALWAYS_INLINE int _Py_IsStaticImmortal(PyObject *op)
{
#if defined(Py_GIL_DISABLED) || SIZEOF_VOID_P > 4
    return (_Py_OB_FLAGS_LOAD(op) & _Py_STATICALLY_ALLOCATED_FLAG) != 0;
#else
    return op->ob_refcnt >= _Py_STATIC_IMMORTAL_MINIMUM_REFCNT;
#endif
}
#define _Py_IsStaticImmortal(op) _Py_IsStaticImmortal(_PyObject_CAST(op))
#endif // !defined(_Py_OPAQUE_PYOBJECT)

// Py_SET_REFCNT() implementation for stable ABI
PyAPI_FUNC(void) _Py_SetRefcnt(PyObject *ob, Py_ssize_t refcnt);

static inline void Py_SET_REFCNT(PyObject *ob, Py_ssize_t refcnt) {
    assert(refcnt >= 0);
#if defined(Py_LIMITED_API) && Py_LIMITED_API+0 >= 0x030d0000
    // Stable ABI implements Py_SET_REFCNT() as a function call
    // on limited C API version 3.13 and newer.
    _Py_SetRefcnt(ob, refcnt);
#else
    // This immortal check is for code that is unaware of immortal objects.
    // The runtime tracks these objects and we should avoid as much
    // as possible having extensions inadvertently change the refcnt
    // of an immortalized object.
    if (_Py_NeedsSlowRcBranch(ob))
    {
        if (_Py_IsImmortal(ob)) {
            return;
        }
#ifdef _Py_PYRONA_INTERPRETER_SHARING
        if (_Py_NeedsImmutableRC(ob)) {
            // TODO This assertion is not valid as refcount overflows can trigger the
            // PyImmortalOrImmutable check to fire.
    
            // TODO(Immutable): It is dangerous to set the reference count of an
            // immutable object. The majority of calls appear to be where the rc
            // has reached 0 and a finalizer is running. This seems a reasonable
            // place to allow the refcnt to be set to 1, and clear the immutable flag.
    
            // TODO(Immutable): This assert does not hold should it.
            // assert(ob->ob_refcnt == 0);
    
            // TODO(Immutable): Do we need to clear the immutability state here?
            // TODO(Immutable): Is here even reachable?
    
            // TODO(Immutable): Care should be taken to make the whole SCC mutable
            // again if needed.
            assert(0);
        }
        if (_Py_NeedsAtomicRC(ob)) {
#ifndef Py_LIMITED_API
            _Py_atomic_store_uint32_relaxed(&ob->ob_refcnt, (PY_UINT32_T)refcnt);
#else
            // The atomics are not exposed under the limited API; _Py_SetRefcnt()
            // is compiled without it and performs the same atomic store.
            _Py_SetRefcnt(ob, refcnt);
#endif
            return;
        }
#endif
    }
#ifndef Py_GIL_DISABLED
#if SIZEOF_VOID_P > 4
    ob->ob_refcnt = (PY_UINT32_T)refcnt;
#else
    ob->ob_refcnt = refcnt;
#endif
#else
    if (_Py_IsOwnedByCurrentThread(ob)) {
        if ((size_t)refcnt > (size_t)UINT32_MAX) {
            // On overflow, make the object immortal
            ob->ob_tid = _Py_UNOWNED_TID;
            ob->ob_ref_local = _Py_IMMORTAL_REFCNT_LOCAL;
            ob->ob_ref_shared = 0;
        }
        else {
            // Set local refcount to desired refcount and shared refcount
            // to zero, but preserve the shared refcount flags.
            ob->ob_ref_local = _Py_STATIC_CAST(uint32_t, refcnt);
            ob->ob_ref_shared &= _Py_REF_SHARED_FLAG_MASK;
        }
    }
    else {
        // Set local refcount to zero and shared refcount to desired refcount.
        // Mark the object as merged.
        ob->ob_tid = _Py_UNOWNED_TID;
        ob->ob_ref_local = 0;
        ob->ob_ref_shared = _Py_REF_SHARED(refcnt, _Py_REF_MERGED);
    }
#endif  // Py_GIL_DISABLED
#endif  // Py_LIMITED_API+0 < 0x030d0000
}
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 < 0x030b0000
#  define Py_SET_REFCNT(ob, refcnt) Py_SET_REFCNT(_PyObject_CAST(ob), (refcnt))
#endif


/*
The macros Py_INCREF(op) and Py_DECREF(op) are used to increment or decrement
reference counts.  Py_DECREF calls the object's deallocator function when
the refcount falls to 0; for
objects that don't contain references to other objects or heap memory
this can be the standard function free().  Both macros can be used
wherever a void expression is allowed.  The argument must not be a
NULL pointer.  If it may be NULL, use Py_XINCREF/Py_XDECREF instead.
The macro _Py_NewReference(op) initialize reference counts to 1, and
in special builds (Py_REF_DEBUG, Py_TRACE_REFS) performs additional
bookkeeping appropriate to the special build.

We assume that the reference count field can never overflow; this can
be proven when the size of the field is the same as the pointer size, so
we ignore the possibility.  Provided a C int is at least 32 bits (which
is implicitly assumed in many parts of this code), that's enough for
about 2**31 references to an object.

XXX The following became out of date in Python 2.2, but I'm not sure
XXX what the full truth is now.  Certainly, heap-allocated type objects
XXX can and should be deallocated.
Type objects should never be deallocated; the type pointer in an object
is not considered to be a reference to the type object, to save
complications in the deallocation function.  (This is actually a
decision that's up to the implementer of each new type so if you want,
you can count such references to the type object.)
*/

#if defined(Py_REF_DEBUG) && !defined(Py_LIMITED_API)
PyAPI_FUNC(void) _Py_NegativeRefcount(const char *filename, int lineno,
                                      PyObject *op);
PyAPI_FUNC(void) _Py_INCREF_IncRefTotal(void);
PyAPI_FUNC(void) _Py_DECREF_DecRefTotal(void);
#endif  // Py_REF_DEBUG && !Py_LIMITED_API

PyAPI_FUNC(void) _Py_Dealloc(PyObject *);


/*
These are provided as conveniences to Python runtime embedders, so that
they can have object code that is not dependent on Python compilation flags.
*/
PyAPI_FUNC(void) Py_IncRef(PyObject *);
PyAPI_FUNC(void) Py_DecRef(PyObject *);

// Similar to Py_IncRef() and Py_DecRef() but the argument must be non-NULL.
// Private functions used by Py_INCREF() and Py_DECREF().
PyAPI_FUNC(void) _Py_IncRef(PyObject *);
PyAPI_FUNC(void) _Py_DecRef(PyObject *);

static inline Py_ALWAYS_INLINE void Py_INCREF(PyObject *op)
{
#if defined(Py_LIMITED_API) && (Py_LIMITED_API+0 >= 0x030c0000 || defined(Py_REF_DEBUG))
    // Stable ABI implements Py_INCREF() as a function call on limited C API
    // version 3.12 and newer, and on Python built in debug mode. _Py_IncRef()
    // was added to Python 3.10.0a7, use Py_IncRef() on older Python versions.
    // Py_IncRef() accepts NULL whereas _Py_IncRef() doesn't.
#  if Py_LIMITED_API+0 >= 0x030a00A7
    _Py_IncRef(op);
#  else
    Py_IncRef(op);
#  endif
#else
    // Non-limited C API and limited C API for Python 3.9 and older access
    // directly PyObject.ob_refcnt.
#if defined(Py_GIL_DISABLED)
    uint32_t local = _Py_atomic_load_uint32_relaxed(&op->ob_ref_local);
    uint32_t new_local = local + 1;
    if (new_local == 0) {
        _Py_INCREF_IMMORTAL_STAT_INC();
        // local is equal to _Py_IMMORTAL_REFCNT_LOCAL: do nothing
        return;
    }
    if (_Py_IsOwnedByCurrentThread(op)) {
        _Py_atomic_store_uint32_relaxed(&op->ob_ref_local, new_local);
    }
    else {
        _Py_atomic_add_ssize(&op->ob_ref_shared, (1 << _Py_REF_SHARED_SHIFT));
    }
#elif SIZEOF_VOID_P > 4
    // Using ob_refcnt_full allows us to check if a flag has been set for immutable too.
    Py_ssize_t cur_refcnt = op->ob_refcnt_full;
    if (_Py_NeedsSlowRcBranch(op)) {
        // the object is immortal
        if (_Py_IsImmortal(op))
        {
            _Py_INCREF_IMMORTAL_STAT_INC();
            return;
        }
#ifndef Py_LIMITED_API
#ifdef _Py_PYRONA_INTERPRETER_SHARING
        // Artifact[Implementation]: The atomic RC branch for immutable objects in Py_INCREF
        if (_Py_NeedsImmutableRC(op)) {
            _Py_RefcntAdd_Immutable(op, 1);
            return;
        }
        if (_Py_NeedsAtomicRC(op)) {
            _Py_atomic_add_uint32(&op->ob_refcnt, 1);
            return;
        }
#endif
#else
        // Immutable object in limited API: delegate to runtime function
        Py_IncRef(op);
        return;
#endif
    }
    op->ob_refcnt = (uint32_t)cur_refcnt + 1;
#else
    if (_Py_IsImmortal(op)) {
        _Py_INCREF_IMMORTAL_STAT_INC();
        return;
    }
    op->ob_refcnt++;
#endif
    _Py_INCREF_STAT_INC();
#ifdef Py_REF_DEBUG
    // Don't count the incref if the object is immortal.
    if (!_Py_IsImmortal(op)) {
        _Py_INCREF_IncRefTotal();
    }
#endif
#endif
}
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 < 0x030b0000
#  define Py_INCREF(op) Py_INCREF(_PyObject_CAST(op))
#endif

#if !defined(Py_LIMITED_API) && defined(Py_GIL_DISABLED)
// Implements Py_DECREF on objects not owned by the current thread.
PyAPI_FUNC(void) _Py_DecRefShared(PyObject *);
PyAPI_FUNC(void) _Py_DecRefSharedDebug(PyObject *, const char *, int);

// Called from Py_DECREF by the owning thread when the local refcount reaches
// zero. The call will deallocate the object if the shared refcount is also
// zero. Otherwise, the thread gives up ownership and merges the reference
// count fields.
PyAPI_FUNC(void) _Py_MergeZeroLocalRefcount(PyObject *);
#endif

#if defined(Py_LIMITED_API) && (Py_LIMITED_API+0 >= 0x030c0000 || defined(Py_REF_DEBUG))
// Stable ABI implements Py_DECREF() as a function call on limited C API
// version 3.12 and newer, and on Python built in debug mode. _Py_DecRef() was
// added to Python 3.10.0a7, use Py_DecRef() on older Python versions.
// Py_DecRef() accepts NULL whereas _Py_DecRef() doesn't.
static inline void Py_DECREF(PyObject *op) {
#  if Py_LIMITED_API+0 >= 0x030a00A7
    _Py_DecRef(op);
#  else
    Py_DecRef(op);
#  endif
}
#define Py_DECREF(op) Py_DECREF(_PyObject_CAST(op))

#elif defined(Py_GIL_DISABLED) && defined(Py_REF_DEBUG)
static inline void Py_DECREF(const char *filename, int lineno, PyObject *op)
{
    // FIXME(regions): This needs a _Py_NeedsSlowRcBranch for regions
    uint32_t local = _Py_atomic_load_uint32_relaxed(&op->ob_ref_local);
    if (local == _Py_IMMORTAL_REFCNT_LOCAL) {
        _Py_DECREF_IMMORTAL_STAT_INC();
        return;
    }
    _Py_DECREF_STAT_INC();
    _Py_DECREF_DecRefTotal();
    if (_Py_IsOwnedByCurrentThread(op)) {
        if (local == 0) {
            _Py_NegativeRefcount(filename, lineno, op);
        }
        local--;
        _Py_atomic_store_uint32_relaxed(&op->ob_ref_local, local);
        if (local == 0) {
            _Py_MergeZeroLocalRefcount(op);
        }
    }
    else {
        _Py_DecRefSharedDebug(op, filename, lineno);
    }
}
#define Py_DECREF(op) Py_DECREF(__FILE__, __LINE__, _PyObject_CAST(op))

#elif defined(Py_GIL_DISABLED)
static inline void Py_DECREF(PyObject *op)
{
    // FIXME(regions): This needs a _Py_NeedsSlowRcBranch for regions
    uint32_t local = _Py_atomic_load_uint32_relaxed(&op->ob_ref_local);
    if (local == _Py_IMMORTAL_REFCNT_LOCAL) {
        _Py_DECREF_IMMORTAL_STAT_INC();
        return;
    }
    _Py_DECREF_STAT_INC();
    if (_Py_IsOwnedByCurrentThread(op)) {
        local--;
        _Py_atomic_store_uint32_relaxed(&op->ob_ref_local, local);
        if (local == 0) {
            _Py_MergeZeroLocalRefcount(op);
        }
    }
    else {
        _Py_DecRefShared(op);
    }
}
#define Py_DECREF(op) Py_DECREF(_PyObject_CAST(op))

#elif defined(Py_REF_DEBUG)

static inline void Py_DECREF(const char *filename, int lineno, PyObject *op)
{
#if SIZEOF_VOID_P > 4
    /* If an object has been freed, it will have a negative full refcnt
     * If it has not it been freed, will have a very large refcnt */
    if (op->ob_refcnt_full <= 0 || op->ob_refcnt > (((PY_UINT32_T)-1) - (1<<20))) {
#else
    if (op->ob_refcnt <= 0) {
#endif
        _Py_NegativeRefcount(filename, lineno, op);
    }
    if (_Py_NeedsSlowRcBranch(op))
    {
        if (_Py_IsImmortal(op)) {
            _Py_DECREF_IMMORTAL_STAT_INC();
            return;
        }
#ifdef _Py_PYRONA_INTERPRETER_SHARING
        if (_Py_NeedsImmutableRC(op)) {
            if (_Py_DecRef_Immutable(op)) {
                _Py_Dealloc(op);
            }
            return;
        }
        if (_Py_NeedsAtomicRC(op)) {
            // A previous value of 1 means the new value is now 0
            uint32_t old = _Py_atomic_add_uint32(&op->ob_refcnt, -1);
            assert(old > 0);
            if (old == 1) {
                _Py_Dealloc(op);
            }
            return;
        }
#endif // _Py_PYRONA_INTERPRETER_SHARING
    }
    _Py_DECREF_STAT_INC();
    _Py_DECREF_DecRefTotal();
    if (--op->ob_refcnt == 0) {
        _Py_Dealloc(op);
    }
}
#define Py_DECREF(op) Py_DECREF(__FILE__, __LINE__, _PyObject_CAST(op))

#else

static inline Py_ALWAYS_INLINE void Py_DECREF(PyObject *op)
{
    // Non-limited C API and limited C API for Python 3.9 and older access
    // directly PyObject.ob_refcnt.
    if (_Py_NeedsSlowRcBranch(op))
    {
        if (_Py_IsImmortal(op)) {
            _Py_DECREF_IMMORTAL_STAT_INC();
            return;
        }
#ifdef _Py_PYRONA_INTERPRETER_SHARING
#ifndef Py_LIMITED_API
        // Artifact[Implementation]: The atomic RC branch for immutable objects in Py_DECREF
        if (_Py_NeedsImmutableRC(op))
        {
            if (_Py_DecRef_Immutable(op)) {
                _Py_Dealloc(op);
            }
            return;
        }
        if (_Py_NeedsAtomicRC(op)) {
            // A previous value of 1 means the new value is now 0
            uint32_t old = _Py_atomic_add_uint32(&op->ob_refcnt, -1);
            assert(old > 0);
            if (old == 1) {
                _Py_Dealloc(op);
            }
            return;
        }
#else
        // Immutable object in limited API: delegate to runtime function
        Py_DecRef(op);
        return;
#endif
#endif
    }
    _Py_DECREF_STAT_INC();
    if (--op->ob_refcnt == 0) {
        _Py_Dealloc(op);
    }
}
#define Py_DECREF(op) Py_DECREF(_PyObject_CAST(op))
#endif


/* Safely decref `op` and set `op` to NULL, especially useful in tp_clear
 * and tp_dealloc implementations.
 *
 * Note that "the obvious" code can be deadly:
 *
 *     Py_XDECREF(op);
 *     op = NULL;
 *
 * Typically, `op` is something like self->containee, and `self` is done
 * using its `containee` member.  In the code sequence above, suppose
 * `containee` is non-NULL with a refcount of 1.  Its refcount falls to
 * 0 on the first line, which can trigger an arbitrary amount of code,
 * possibly including finalizers (like __del__ methods or weakref callbacks)
 * coded in Python, which in turn can release the GIL and allow other threads
 * to run, etc.  Such code may even invoke methods of `self` again, or cause
 * cyclic gc to trigger, but-- oops! --self->containee still points to the
 * object being torn down, and it may be in an insane state while being torn
 * down.  This has in fact been a rich historic source of miserable (rare &
 * hard-to-diagnose) segfaulting (and other) bugs.
 *
 * The safe way is:
 *
 *      Py_CLEAR(op);
 *
 * That arranges to set `op` to NULL _before_ decref'ing, so that any code
 * triggered as a side-effect of `op` getting torn down no longer believes
 * `op` points to a valid object.
 *
 * There are cases where it's safe to use the naive code, but they're brittle.
 * For example, if `op` points to a Python integer, you know that destroying
 * one of those can't cause problems -- but in part that relies on that
 * Python integers aren't currently weakly referencable.  Best practice is
 * to use Py_CLEAR() even if you can't think of a reason for why you need to.
 *
 * gh-98724: Use a temporary variable to only evaluate the macro argument once,
 * to avoid the duplication of side effects if the argument has side effects.
 *
 * gh-99701: If the PyObject* type is used with casting arguments to PyObject*,
 * the code can be miscompiled with strict aliasing because of type punning.
 * With strict aliasing, a compiler considers that two pointers of different
 * types cannot read or write the same memory which enables optimization
 * opportunities.
 *
 * If available, use _Py_TYPEOF() to use the 'op' type for temporary variables,
 * and so avoid type punning. Otherwise, use memcpy() which causes type erasure
 * and so prevents the compiler to reuse an old cached 'op' value after
 * Py_CLEAR().
 */
#ifdef _Py_TYPEOF
#define Py_CLEAR(op) \
    do { \
        _Py_TYPEOF(op)* _tmp_op_ptr = &(op); \
        _Py_TYPEOF(op) _tmp_old_op = (*_tmp_op_ptr); \
        if (_tmp_old_op != NULL) { \
            *_tmp_op_ptr = _Py_NULL; \
            Py_DECREF(_tmp_old_op); \
        } \
    } while (0)
#else
#define Py_CLEAR(op) \
    do { \
        PyObject **_tmp_op_ptr = _Py_CAST(PyObject**, &(op)); \
        PyObject *_tmp_old_op = (*_tmp_op_ptr); \
        if (_tmp_old_op != NULL) { \
            PyObject *_null_ptr = _Py_NULL; \
            memcpy(_tmp_op_ptr, &_null_ptr, sizeof(PyObject*)); \
            Py_DECREF(_tmp_old_op); \
        } \
    } while (0)
#endif


/* Function to use in case the object pointer can be NULL: */
static inline void Py_XINCREF(PyObject *op)
{
    if (op != _Py_NULL) {
        Py_INCREF(op);
    }
}
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 < 0x030b0000
#  define Py_XINCREF(op) Py_XINCREF(_PyObject_CAST(op))
#endif

static inline void Py_XDECREF(PyObject *op)
{
    if (op != _Py_NULL) {
        Py_DECREF(op);
    }
}
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 < 0x030b0000
#  define Py_XDECREF(op) Py_XDECREF(_PyObject_CAST(op))
#endif

// Create a new strong reference to an object:
// increment the reference count of the object and return the object.
PyAPI_FUNC(PyObject*) Py_NewRef(PyObject *obj);

// Similar to Py_NewRef(), but the object can be NULL.
PyAPI_FUNC(PyObject*) Py_XNewRef(PyObject *obj);

static inline PyObject* _Py_NewRef(PyObject *obj)
{
    Py_INCREF(obj);
    return obj;
}

static inline PyObject* _Py_XNewRef(PyObject *obj)
{
    Py_XINCREF(obj);
    return obj;
}

// Py_NewRef() and Py_XNewRef() are exported as functions for the stable ABI.
// Names overridden with macros by static inline functions for best
// performances.
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 < 0x030b0000
#  define Py_NewRef(obj) _Py_NewRef(_PyObject_CAST(obj))
#  define Py_XNewRef(obj) _Py_XNewRef(_PyObject_CAST(obj))
#else
#  define Py_NewRef(obj) _Py_NewRef(obj)
#  define Py_XNewRef(obj) _Py_XNewRef(obj)
#endif


#ifdef __cplusplus
}
#endif
#endif   // !_Py_REFCOUNT_H
