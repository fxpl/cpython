#ifndef Py_INTERNAL_REGIONOBJECT_H
#define Py_INTERNAL_REGIONOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

#include "object.h"
#include "exports.h"
#include "pytypedefs.h"
#include "pycore_region.h"

struct _PyRegionObject {
    PyObject_HEAD
    /* The region value which will be updated and still filled when the
     * dealloc function of the object is called.
     */
    Py_region_t region;
    /** The name of the region or NULL */
    PyObject *name;
    PyObject *dict;
    /* A link in a list of regions to be garbage collected. */
    struct _PyRegionObject *next;
};
#define _PyRegionObject_CAST(op) _Py_CAST(_PyRegionObject*, op)

PyAPI_DATA(PyTypeObject) _PyRegion_Type;

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_REGIONOBJECT_H */
