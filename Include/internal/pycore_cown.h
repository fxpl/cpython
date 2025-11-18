#ifndef Py_INTERNAL_COWN_H
#define Py_INTERNAL_COWN_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

#include "object.h"
#include "exports.h"
#include "region.h"
#include "pycore_region.h"

typedef struct _PyCownObject _PyCownObject;

#define _PyCownObject_CAST(op) _Py_CAST(_PyCownObject*, op)

PyAPI_DATA(PyTypeObject) _PyCown_Type;

//PyAPI_FUNC(PyObject*) _PyCown_New();
// PyAPI_FUNC(int) _PyCown_SetValue(_PyCownObject* self, PyObject* value);
PyAPI_FUNC(uint64_t) _PyCown_ConcurrentUnitId(void);
PyAPI_FUNC(int) _PyCown_RegionOpen(_PyCownObject *self, _PyBridgeObject* region, uint64_t cuid);
PyAPI_FUNC(int) _PyCown_RegionClose(_PyCownObject *self, _PyBridgeObject* region, uint64_t cuid);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_COWN_H */
