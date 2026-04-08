#ifndef Py_INTERNAL_IMMUTABILITY_H
#define Py_INTERNAL_IMMUTABILITY_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

PyAPI_DATA(PyTypeObject) _PyTracingRegion_Type;
PyAPI_FUNC(int) _PyTracingRegion_Close(PyObject* region);
PyAPI_FUNC(int) _PyTracingRegion_IsClosed(PyObject* region);

struct _Py_immutability_state {
    // FIXME(immutability): We probably need to lock any reads and writes. And
    // we probably want a read write lock for this.
    int late_init_done;
    struct _Py_hashtable_t *immutable_by_construction_types;
    struct _Py_hashtable_t *warned_types;
    // FIXME(immutability): This stack can be removed after the rewrite.
    //
    // With the pre-freeze hook it can happen that freeze calls are
    // nested. This is stack of the enclosing freeze states.
    struct FreezeState *freeze_stack;
#ifdef Py_DEBUG
    PyObject *traceback_func;  // For debugging purposes, can be NULL
#endif
};

#ifdef _Py_PYRONA_INTERPRETER_SHARING
static inline void _Py_EnableAtomicRC(PyObject *op)
{
    _Py_OB_FLAG_ADD(op, _Py_ATOMIC_RC_FLAG);
}
#define _Py_EnableAtomicRC(op) _Py_EnableAtomicRC(_PyObject_CAST(op))
#endif

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_IMMUTABILITY_H */