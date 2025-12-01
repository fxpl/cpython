#ifndef Py_REGION_H
#define Py_REGION_H
#ifdef __cplusplus
extern "C" {
#endif

#include "object.h"
#include "exports.h"

typedef Py_uintptr_t PyRegion_staged_ref_t;
#define PyRegion_staged_ref_ERR 0

PyAPI_FUNC(Py_region_t) _PyRegion_GetSlow(PyObject *obj);

/* Returns the region of the given object.
 */
static inline Py_region_t _PyRegion_Get(PyObject *obj) {
    if (obj == NULL) {
        return _Py_IMMUTABLE_REGION;
    }

    // Immutable objects can be shared across threads, it's not safe to access
    // the region information without synchronization.
    if (_Py_IsImmutable(obj)) {
        return _Py_IMMUTABLE_REGION;
    }

    // Fast path, almost every object should be in one of these regions
    if (obj->ob_region == _Py_LOCAL_REGION
        || obj->ob_region == _Py_COWN_REGION
    ) {
        return obj->ob_region;
    }

    return _PyRegion_GetSlow(obj);
}
#define _PyRegion_Get(obj) _PyRegion_Get(_PyObject_CAST(obj))

static inline int _Py_IsLocal(PyObject *obj) {
    return _PyRegion_Get(obj) == _Py_LOCAL_REGION;
}
#define _Py_IsLocal(obj) _Py_IsLocal(_PyObject_CAST(obj))


// Helper macros to count the number of arguments
#define _PyRegion__COUNT_ARGS(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N, ...) N
#define _PyRegion_COUNT_ARGS(...) _PyRegion__COUNT_ARGS(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1)
#define _PyRegion_MAX_ARG_COUNT 16

PyAPI_FUNC(PyRegion_staged_ref_t) _PyRegion_StageRef(PyObject *src, PyObject *tgt);
PyAPI_FUNC(void) PyRegion_ResetStagedRef(PyRegion_staged_ref_t staged_ref);
PyAPI_FUNC(void) PyRegion_CommitStagedRef(PyRegion_staged_ref_t staged_ref);
#define PyRegion_StageRef(src, tgt) _PyRegion_StageRef(_PyObject_CAST(src), _PyObject_CAST(tgt))

PyAPI_FUNC(int) _PyRegion_AddRef(PyObject *src, PyObject *tgt);
PyAPI_FUNC(int) _PyRegion_AddRefs(PyObject *src, int tgt_count, ...);
#define PyRegion_AddRef(src, tgt) _PyRegion_AddRef(_PyObject_CAST(src), _PyObject_CAST(tgt))
#define PyRegion_AddRefS(src, ...) _PyRegion_AddRefs(_PyObject_CAST(src), _PyRegion_COUNT_ARGS(__VA_ARGS__), __VA_ARGS__)

PyAPI_FUNC(int) _PyRegion_RemoveRef(PyObject *src, PyObject *tgt);
#define PyRegion_RemoveRef(src, tgt) _PyRegion_RemoveRef(_PyObject_CAST(src), _PyObject_CAST(tgt))

PyAPI_FUNC(int) _PyRegion_AddLocalRef(PyObject *tgt);
PyAPI_FUNC(int) _PyRegion_AddLocalRefs(int tgt_count, ...);
#define PyRegion_AddLocalRef(tgt) _PyRegion_AddLocalRef(_PyObject_CAST(tgt))
#define PyRegion_AddLocalRefs(...) _PyRegion_AddLocalRefs(_PyRegion_COUNT_ARGS(__VA_ARGS__), __VA_ARGS__)

PyAPI_FUNC(int) _PyRegion_RemoveLocalRef(PyObject *tgt);
#define PyRegion_RemoveLocalRef(tgt) _PyRegion_RemoveLocalRef(_PyObject_CAST(tgt))

static inline PyObject* _PyRegion_NewRef(PyObject* tgt) {
    if (PyRegion_AddLocalRef(tgt)) {
        return NULL;
    }
    return Py_NewRef(tgt);
}
static inline PyObject* _PyRegion_XNewRef(PyObject* tgt) {
    if (!tgt) {
        return NULL;
    }

    return _PyRegion_NewRef(tgt);
}
#define PyRegion_NewRef(tgt) _PyRegion_NewRef(_PyObject_CAST(tgt))
#define PyRegion_XNewRef(tgt) _PyRegion_XNewRef(_PyObject_CAST(tgt))

static inline int _PyRegion_TakeRef(PyObject *src, PyObject *tgt) {
    int res = _PyRegion_AddRef(src, tgt);
    if (res) {
        return res;
    }

    // Removing the local reference here is safe. There are three
    // interesting cases which can happen with this function:
    //
    // - src is local & tgt is in region Y
    //      In this case, Y will remain open, since the `AddRef` call above
    //      bumped the LRC, basically making this a no-op.
    // - src and tgt are in the same region
    //      This call will reduce the LRC, but the region will remain open
    //      since there is a remaining local reference to src
    // - src is in region X and tgt is the bridge object of Y
    //      Removing the local reference may close Y, but X as the new parent
    //      region of Y will remain open. Closing of Y will therefore only
    //      modify the OSC of X but not close X. This ensures that no cown is
    //      released or send off, while we still have remaining references into
    //      X and Y.
    return _PyRegion_RemoveLocalRef(tgt);
}

#define PyRegion_TakeRef(src, tgt) _PyRegion_TakeRef(_PyObject_CAST(src), _PyObject_CAST(tgt))

#ifdef __cplusplus
}
#endif
#endif /* !Py_REGION_H */