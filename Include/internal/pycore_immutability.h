#ifndef Py_INTERNAL_IMMUTABILITY_H
#define Py_INTERNAL_IMMUTABILITY_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

#include "pycore_mutexobject.h"

struct _PyRegionRefMetadata;

PyAPI_DATA(PyTypeObject) _PyTracingRegion_Type;
PyAPI_FUNC(int) _PyTracingRegion_IsClosed(PyObject* region);
PyAPI_FUNC(void) _PyTracingRegion_Open(PyObject* region);
PyAPI_FUNC(int) _PyTracingRegion_Detach(PyObject* region);
PyAPI_FUNC(int) _PyTracingRegion_Attach(PyObject* region, _PyPyrona_owner_id_t owner);

/* Returns the region's metadata node, allocating it if this is the first
 * region reference the current close has found. Borrowed, and only valid while
 * the region stays closed. The caller must hold `_PyWeakref_Lock`. */
PyAPI_FUNC(struct _PyRegionRefMetadata*)
    _PyTracingRegion_MetaLockHeld(PyObject* region);

/* Records who owns a closed region, used when ownership changes hands, e.g. when
 * a mutex acquires, releases, or stores the region. Does nothing for an open region.
 * The owner is NOT necessarily the calling interpreter: a mutex is immutable and
 * may be deallocated by anyone holding a reference, including an interpreter
 * that never owned it. Pass `_Py_PYRONA_RELEASED_OWNER_ID` when nobody owns it. */
// FIXME: The deallocation will be fixed in a follow-up, then we can remove the
// owner argument and assert that it's always local.
PyAPI_FUNC(void) _PyTracingRegion_SetMetaOwner(
    PyObject* region, _PyPyrona_owner_id_t owner);

struct _Py_immutability_state {
    _PyOnceFlag init_once;
    // Guards immutable_by_construction_types and warned_types.
    PyMutex mutex;
    int late_init_done;  // Accessed atomically
    struct _Py_hashtable_t *immutable_by_construction_types;
    struct _Py_hashtable_t *warned_types;
#ifdef Py_DEBUG
    PyObject *traceback_func;  // For debugging purposes, can be NULL. Atomic.
#endif
};

#ifdef _Py_PYRONA_INTERPRETER_SHARING
static inline void _Py_EnableAtomicRC(PyObject *op)
{
    _Py_OB_FLAGS_ADD(op, _Py_ATOMIC_RC_FLAG);
}
#define _Py_EnableAtomicRC(op) _Py_EnableAtomicRC(_PyObject_CAST(op))
#endif

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_IMMUTABILITY_H */
