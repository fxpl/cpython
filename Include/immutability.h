#ifndef Py_IMMUTABILITY_H
#define Py_IMMUTABILITY_H

#ifdef __cplusplus
extern "C" {
#endif

PyAPI_DATA(PyTypeObject) PyNotFreezable_Type;

#ifndef Py_LIMITED_API
#  define Py_CPYTHON_IMMUTABILITY_H
#  include "cpython/immutability.h"
#  undef Py_CPYTHON_IMMUTABILITY_H
#endif


#ifdef __cplusplus
}
#endif
#endif /* !Py_IMMUTABILITY_H */
