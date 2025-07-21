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

    if (obj->ob_region == _Py_LOCAL_REGION
        || obj->ob_region == _Py_IMMUTABLE_REGION
        || obj->ob_region == _Py_COWN_REGION
    ) {
        return obj->ob_region;
    }

    return _Py_RegionGetSlow(obj);
}

PyAPI_FUNC(int) _Py_RegionMoveToImmuable(PyObject *obj);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_REGION_H */
