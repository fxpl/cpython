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

typedef struct _PyCownObject _PyCownObject;
#define _PyCownObject_CAST(op) _Py_CAST(_PyCownObject*, op)

PyAPI_DATA(PyTypeObject) _PyCown_Type;

typedef uintptr_t _PyCown_owner_id_t;


PyAPI_FUNC(_PyCown_owner_id_t) _PyCown_ThisOwnerId(void);


#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_COWN_H */