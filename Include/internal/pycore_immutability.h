#ifndef Py_FREEZE_H
#define Py_FREEZE_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

PyObject* _Py_Freeze(PyObject*);
#define Py_Freeze(op) _Py_Freeze(_PyObject_CAST(op))

#ifdef __cplusplus
}
#endif
#endif /* !Py_FREEZE_H */