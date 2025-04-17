#ifndef Py_IMMUTABILITY_H
#define Py_IMMUTABILITY_H

#ifdef __cplusplus
extern "C" {
#endif

PyAPI_DATA(PyTypeObject) PyNotFreezable_Type;

PyAPI_FUNC(PyObject *) _Py_Freeze(PyObject*);
#define Py_Freeze(op) _Py_Freeze(_PyObject_CAST(op))

#ifdef __cplusplus
}
#endif
#endif /* !Py_IMMUTABILITY_H */
