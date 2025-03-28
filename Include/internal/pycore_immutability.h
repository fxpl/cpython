#ifndef Py_INTERNAL_IMMUTABILITY_H
#define Py_INTERNAL_IMMUTABILITY_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

struct _Py_immutability_state {
    PyObject *module_locks;
    PyObject *blocking_on;
    PyObject *freezable_types;
    PyObject *destroy_cb;
};

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_IMMUTABILITY_H */