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

typedef uint64_t _PyCown_ipid_t;
typedef uint64_t _PyCown_thread_id_t;

//PyAPI_FUNC(PyObject*) _PyCown_New();
PyAPI_FUNC(PyObject*) _PyCown_GetValue(_PyCownObject* self);
PyAPI_FUNC(int) _PyCown_SetValue(_PyCownObject* self, PyObject* value);
PyAPI_FUNC(_PyCown_ipid_t) _PyCown_ThisInterpreterId(void);
PyAPI_FUNC(_PyCown_thread_id_t) _PyCown_ThisThreadId(void);
PyAPI_FUNC(int) _PyCown_RegionOpen(_PyCownObject *self, _PyRegionObject* region, _PyCown_ipid_t ip);
PyAPI_FUNC(int) _PyCown_AcquireGC(_PyCownObject *self, Py_region_t *region);
PyAPI_FUNC(int) _PyCown_SwitchFromGcToIp(_PyCownObject *self);
PyAPI_FUNC(int) _PyCown_SwitchFromIpToGc(_PyCownObject *self, Py_region_t *contained_region);
PyAPI_FUNC(int) _PyCown_ReleaseGC(_PyCownObject *self);


#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_COWN_H */
