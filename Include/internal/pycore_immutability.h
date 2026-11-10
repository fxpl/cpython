#ifndef Py_INTERNAL_IMMUTABILITY_H
#define Py_INTERNAL_IMMUTABILITY_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

#include "pycore_mutexobject.h"

PyAPI_DATA(PyTypeObject) _PyTracingRegion_Type;
PyAPI_FUNC(int) _PyTracingRegion_IsClosed(PyObject* region);
PyAPI_FUNC(void) _PyTracingRegion_Open(PyObject* region);
PyAPI_FUNC(int) _PyTracingRegion_Detach(PyObject* region);
PyAPI_FUNC(int) _PyTracingRegion_Attach(PyObject* region, _PyPyrona_owner_id_t owner);

#ifdef Py_GIL_DISABLED
/* Visits `op` as a target of `n` edges. */
typedef int (*_PyRegion_countedvisitproc)(PyObject *op, Py_ssize_t n, void *arg);

/* Reports the edges of a closed bridge seen through its remset with their
 * multiplicity, instead of once per edge. Returns 1 if it did, and 0 if the
 * caller must use tp_traverse. Only safe while the world is stopped. */
PyAPI_FUNC(int) _PyTracingRegion_TraverseCounted(
    PyObject *op, _PyRegion_countedvisitproc visit, void *arg);
#endif

typedef struct _PyRegion_WrefInfo _PyRegion_WrefInfo;
PyAPI_FUNC(void) _PyRegion_WrefInfoDecRef(_PyRegion_WrefInfo*);
PyAPI_FUNC(void) _PyRegion_WrefInfoDecRefLockHeld(_PyRegion_WrefInfo*);
PyAPI_FUNC(void) _PyRegion_WrefInfoIncRefLockHeld(_PyRegion_WrefInfo*);
PyAPI_FUNC(PyObject*) _PyRegion_WrefTryIncRef(PyWeakReference*);


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
