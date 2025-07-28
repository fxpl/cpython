#ifndef Py_INTERNAL_REGION_H
#define Py_INTERNAL_REGION_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

#include "object.h"

/* Macros for readability */
#define NULL_REGION 0

PyAPI_FUNC(Py_region_t) _PyRegion_GetSlow(PyObject *obj);

/* Returns the region of the given object.
 */
static inline Py_region_t _PyRegion_Get(PyObject *obj) {
    assert(obj);

    // Immutable objects can be shared across threads, it's not save to access
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

static inline int _Py_IsLocal(PyObject *obj) {
    return _PyRegion_Get(obj) == _Py_LOCAL_REGION;
}
#define _Py_IsLocal(obj) _Py_IsLocal(_PyObject_CAST(obj))

PyAPI_FUNC(Py_region_t) _PyRegion_New(PyObject *bridge);
PyAPI_FUNC(void) _PyRegion_DecRc(Py_region_t region);

PyAPI_FUNC(int) _PyRegion_IsDirty(Py_region_t region);
PyAPI_FUNC(int) _PyRegion_IsParent(Py_region_t child, Py_region_t parent);

PyAPI_FUNC(PyObject*) _PyRegion_GetBridge(PyObject *obj);

PyAPI_FUNC(int) _PyRegion_SignalImmutable(PyObject *obj);

PyAPI_FUNC(int) _PyRegion_AddRef(PyObject *src, PyObject *tgt);
#define _PyRegion_ADDREF(src, tgt) _PyRegion_AddRef(_PyObject_CAST(src), _PyObject_CAST(tgt))

PyAPI_FUNC(int) _PyRegion_RemoveRef(PyObject *src, PyObject *tgt);
#define _PyRegion_REMOVEREF(src, tgt) _PyRegion_RemoveRef(_PyObject_CAST(src), _PyObject_CAST(tgt))

PyAPI_FUNC(int) _PyRegion_AddLocalRef(PyObject *tgt);
#define _Py_REGIONADDLOCALREF(tgt) _PyRegion_AddLocalRef(_PyObject_CAST(tgt))

PyAPI_FUNC(int) _PyRegion_RemoveLocalRef(PyObject *tgt);
#define _Py_REGIONREMOVELOCALREF(tgt) _PyRegion_RemoveLocalRef(_PyObject_CAST(tgt))

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_REGION_H */
