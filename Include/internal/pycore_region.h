#ifndef Py_INTERNAL_REGION_H
#define Py_INTERNAL_REGION_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

#include "object.h"


PyAPI_FUNC(Py_region_t) _Py_RegionGetSlow(PyObject *obj);

/* Returns the region of the given object.
 */
static inline Py_ssize_t _Py_Region(PyObject *obj) {
    assert(obj);

    // Fast path, almost every object should be in one of these regions
    if (obj->ob_region == _Py_LOCAL_REGION
        || obj->ob_region == _Py_IMMUTABLE_REGION
        || obj->ob_region == _Py_COWN_REGION
    ) {
        return obj->ob_region;
    }

    return _Py_RegionGetSlow(obj);
}

PyAPI_FUNC(PyObject*) _Py_RegionBridge(PyObject *obj);

PyAPI_FUNC(int) _Py_RegionMoveToImmuable(PyObject *obj);
PyAPI_FUNC(int) _Py_RegionRemoveFromImmuable(PyObject *obj);

PyAPI_FUNC(int) _Py_RegionAddRef(PyObject *src, PyObject *tgt);
#define _Py_REGIONADDREF(src, tgt) _Py_RegionAddRef(_PyObject_CAST(src), _PyObject_CAST(tgt))

PyAPI_FUNC(int) _Py_RegionRemoveRef(PyObject *src, PyObject *tgt);
#define _Py_REGIONREMOVEREF(src, tgt) _Py_RegionRemoveRef(_PyObject_CAST(src), _PyObject_CAST(tgt))

PyAPI_FUNC(int) _Py_RegionAddLocalRef(PyObject *tgt);
#define _Py_REGIONADDLOCALREF(tgt) _Py_RegionAddLocalRef(_PyObject_CAST(tgt))

PyAPI_FUNC(int) _Py_RegionRemoveLocalRef(PyObject *tgt);
#define _Py_REGIONREMOVELOCALREF(tgt) _Py_RegionRemoveLocalRef(_PyObject_CAST(tgt))
 
#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_REGION_H */
