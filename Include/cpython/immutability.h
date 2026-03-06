#ifndef Py_CPYTHON_IMMUTABLE_H
#  error "this header file must not be included directly"
#endif

PyAPI_DATA(PyTypeObject) _PyNotFreezable_Type;

PyAPI_FUNC(int) _PyImmutability_Freeze(PyObject*);
PyAPI_FUNC(int) _PyImmutability_FreezeMany(PyObject *const *, Py_ssize_t);
PyAPI_FUNC(int) _PyImmutability_RegisterFreezable(PyTypeObject*);
PyAPI_FUNC(int) _PyImmutability_RegisterShallowImmutable(PyTypeObject*);
PyAPI_FUNC(int) _PyImmutability_CanViewAsImmutable(PyObject*);
PyAPI_FUNC(int) _PyImmutability_SetFreezable(PyObject *, int);
PyAPI_FUNC(int) _PyImmutability_GetFreezable(PyObject *);
