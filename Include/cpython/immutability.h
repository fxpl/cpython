#ifndef Py_CPYTHON_IMMUTABLE_H
#  error "this header file must not be included directly"
#endif

PyAPI_DATA(PyTypeObject) PyNotFreezable_Type;

PyAPI_FUNC(PyObject *) _PyImmutability_Freeze(PyObject*);
PyAPI_FUNC(PyObject *) _PyImmutability_RegisterFreezable(PyObject*);
