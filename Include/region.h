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
#define _PyRegion_GET(obj) _PyRegion_Get(_PyObject_CAST(obj))

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

#ifdef __cplusplus
}
#endif
#endif /* !Py_REGION_H */