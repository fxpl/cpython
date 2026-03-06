#ifndef Py_INTERNAL_IMMUTABILITY_H
#define Py_INTERNAL_IMMUTABILITY_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

typedef struct _Py_hashtable_t _Py_hashtable_t;

typedef enum {
    _Py_FREEZABLE_YES = 0,
    _Py_FREEZABLE_NO = 1,
    _Py_FREEZABLE_EXPLICIT = 2,
    _Py_FREEZABLE_PROXY = 3,
} _Py_freezable_status;

struct _Py_immutability_state {
    int late_init_done;
    PyObject *freezable_types;
    _Py_hashtable_t *shallow_immutable_types;
    PyObject *destroy_cb;
    _Py_hashtable_t *warned_types;
#ifdef Py_DEBUG
    PyObject *traceback_func;  // For debugging purposes, can be NULL
#endif
};

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_IMMUTABILITY_H */