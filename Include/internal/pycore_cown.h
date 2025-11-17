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

// PyAPI_DATA(PyTypeObject) CownType;

//PyAPI_FUNC(PyObject*) _PyCown_New();

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_COWN_H */
