#ifndef Py_INTERNAL_WEAKREF_H
#define Py_INTERNAL_WEAKREF_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_critical_section.h" // Py_BEGIN_CRITICAL_SECTION()
#include "pycore_lock.h"             // PyMutex_LockFlags()
#include "pycore_object.h"           // _Py_REF_IS_MERGED()
#include "pycore_pyatomic_ft_wrappers.h"

// This lock protects the internal state of weakrefs.
//
// The per-object locking Free-threading does is not sufficient to protect
// the region metadata shared across multiple weakrefs. For sub-interpreter
// sharing we need this to avoid race conditions across interpreters.
//
// FIXME(regions): In the future we may want to favour a lock-free design or at
// least use a RWLock, but let's optimize when we have better benchmarks and data.
extern PyMutex _PyPyrona_Lock;

#ifdef Py_GIL_DISABLED

#define WEAKREF_LIST_LOCK(obj) \
    _PyInterpreterState_GET()  \
        ->weakref_locks[((uintptr_t)obj) % NUM_WEAKREF_LIST_LOCKS]

// Lock using the referenced object
#define LOCK_WEAKREFS(obj) \
    PyMutex_LockFlags(&WEAKREF_LIST_LOCK(obj), _Py_LOCK_DONT_DETACH)
#define UNLOCK_WEAKREFS(obj) PyMutex_Unlock(&WEAKREF_LIST_LOCK(obj))

// Lock using a weakref
#define LOCK_WEAKREFS_FOR_WR(wr) \
    PyMutex_LockFlags(wr->weakrefs_lock, _Py_LOCK_DONT_DETACH)
#define UNLOCK_WEAKREFS_FOR_WR(wr) PyMutex_Unlock(wr->weakrefs_lock)

#define FT_CLEAR_WEAKREFS(obj, weakref_list)    \
    do {                                        \
        assert(Py_REFCNT(obj) == 0);            \
        PyObject_ClearWeakRefs(obj);            \
    } while (0)

#else

#define LOCK_WEAKREFS(obj) PyMutex_LockFlags(&_PyPyrona_Lock, _Py_LOCK_DONT_DETACH)
#define UNLOCK_WEAKREFS(obj) PyMutex_Unlock(&_PyPyrona_Lock)

#define LOCK_WEAKREFS_FOR_WR(wr) PyMutex_LockFlags(&_PyPyrona_Lock, _Py_LOCK_DONT_DETACH)
#define UNLOCK_WEAKREFS_FOR_WR(wr) PyMutex_Unlock(&_PyPyrona_Lock)

#define FT_CLEAR_WEAKREFS(obj, weakref_list)        \
    do {                                            \
        assert(Py_REFCNT(obj) == 0);                \
        if (weakref_list != NULL) {                 \
            PyObject_ClearWeakRefs(obj);            \
        }                                           \
    } while (0)

#endif

static inline int _is_dead(PyObject *obj)
{
    // Explanation for the Py_REFCNT() check: when a weakref's target is part
    // of a long chain of deallocations which triggers the trashcan mechanism,
    // clearing the weakrefs can be delayed long after the target's refcount
    // has dropped to zero.  In the meantime, code accessing the weakref will
    // be able to "see" the target object even though it is supposed to be
    // unreachable.  See issue gh-60806.
#if defined(Py_GIL_DISABLED)
    Py_ssize_t shared = _Py_atomic_load_ssize_relaxed(&obj->ob_ref_shared);
    return shared == _Py_REF_SHARED(0, _Py_REF_MERGED);
#else
#ifdef _Py_PYRONA_INTERPRETER_SHARING
    if (_Py_NeedsImmutableRC(obj)) {
        return _Py_IsDead_Immutable(obj);
    }
    if (_Py_NeedsAtomicRC(obj)) {
        Py_ssize_t rc = _Py_atomic_load_uint32(&obj->ob_refcnt);
        return rc == 0;
    }
#endif // _Py_PYRONA_INTERPRETER_SHARING
    return (Py_REFCNT(obj) == 0);
#endif
}

#ifdef _Py_PYRONA_INTERPRETER_SHARING
static inline PyObject* _PyWeakref_get_ref_lock_held(PyWeakReference *ref, PyObject *obj)
{
    if (_Py_NeedsAtomicRC(obj) || _Py_NeedsImmutableRC(obj)) {
        // Need to check again because the object could have been deallocated
        if (ref->wr_object == Py_None) {
            // clear_weakref() was called
            return NULL;
        }

        if (_Py_NeedsImmutableRC(obj)) {
            if (_Py_TryIncref_Immutable(obj)) {
                return obj;
            } else {
                return NULL;
            }
        }

        if (_Py_NeedsAtomicRC(obj)) {
            uint32_t current = 1; // Initial guess, the first loop will load the real value
            while (true) {
                uint32_t desired = current + 1;
                if (_Py_atomic_compare_exchange_uint32(&obj->ob_refcnt, &current, desired)) {
                    return obj;
                }
                if (current == 0) {
                    return NULL;
                }
            };
        }
    }

    if (_Py_TryIncref(obj)) {
        return obj;
    }

    return NULL;
}
#endif

static inline PyObject* _PyWeakref_GET_REF(PyObject *ref_obj)
{
    assert(PyWeakref_Check(ref_obj));
    PyWeakReference *ref = _Py_CAST(PyWeakReference*, ref_obj);

    PyObject *obj = _Py_atomic_load_ptr(&ref->wr_object);
    if (obj == Py_None) {
        // clear_weakref() was called
        return NULL;
    }

    if (_Py_atomic_load_ptr(&ref->wr_region_info) != NULL) {
        return _PyRegion_WrefTryIncRef(ref);
    }

#ifdef Py_GIL_DISABLED
    if (!_Py_TryIncrefCompare(&ref->wr_object, obj)) {
        return NULL;
    }

    // The referenced object was in a region that was closed in the mean time
    // we retry using the Region version of TryIncRef
    if (_Py_atomic_load_ptr(&ref->wr_region_info) != NULL) {
        Py_DECREF(obj);
        return _PyRegion_WrefTryIncRef(ref);
    }

    return obj;
#else // !Py_GIL_DISABLED
#ifdef _Py_PYRONA_INTERPRETER_SHARING
    LOCK_WEAKREFS(obj);
    // The node is installed with this lock held, but the region path takes it too
    if (ref->wr_region_info != NULL) {
        UNLOCK_WEAKREFS(obj);
        return _PyRegion_WrefTryIncRef(ref);
    }
    PyObject* result = _PyWeakref_get_ref_lock_held(ref, obj);
    UNLOCK_WEAKREFS(obj);
    return result;
#else // !_Py_PYRONA_INTERPRETER_SHARING
    PyObject *obj = ref->wr_object;
    if (obj == Py_None) {
        // clear_weakref() was called
        return NULL;
    }
    if (_Py_TryIncref(obj)) {
        return obj;
    }
    return NULL;
#endif
#endif
}

static inline int _PyWeakref_IS_DEAD(PyObject *ref_obj)
{
    assert(PyWeakref_Check(ref_obj));
    int ret = 0;
    PyWeakReference *ref = _Py_CAST(PyWeakReference*, ref_obj);
    PyObject *obj = FT_ATOMIC_LOAD_PTR(ref->wr_object);
    if (obj == Py_None) {
        // clear_weakref() was called
        ret = 1;
    }
    else {
        LOCK_WEAKREFS(obj);
        // Immutable objects and free-threaded builds require a new check
        // See _PyWeakref_GET_REF() for the rationale of this test
        ret = (ref->wr_object == Py_None) || _is_dead(obj);
        UNLOCK_WEAKREFS(obj);
    }
    return ret;
}

extern Py_ssize_t _PyWeakref_GetWeakrefCount(PyObject *obj);

// Clear all the weak references to obj but leave their callbacks uncalled and
// intact.
extern void _PyWeakref_ClearWeakRefsNoCallbacks(PyObject *obj);

#ifdef _Py_PYRONA_INTERPRETER_SHARING
PyAPI_FUNC(void) _PyWeakref_OnObjectFreeze(PyObject *object);
PyAPI_FUNC(void) _PyImmutability_ClearWeakRefsWithCallback(PyObject *object, PyWeakReference **callbacks);
#endif

PyAPI_FUNC(int) _PyWeakref_IsDead(PyObject *weakref);
PyAPI_FUNC(int) _PyWeakref_HasWeakrefs(PyObject *obj);
PyAPI_FUNC(void) _PyWeakref_InstallRegionInfo_InfoLockHeld(PyObject *obj, _PyRegion_WrefInfo* info);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_WEAKREF_H */
