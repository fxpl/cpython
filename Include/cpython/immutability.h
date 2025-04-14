#ifndef Py_CPYTHON_IMMUTABILITY_H
#  error "this header file must not be included directly"
#endif

PyAPI_FUNC(PyObject *) _Py_Freeze(PyObject*);
#define Py_Freeze(op) _Py_Freeze(_PyObject_CAST(op))
