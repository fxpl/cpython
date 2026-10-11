#ifndef Py_INTERNAL_MODULEOBJECT_H
#define Py_INTERNAL_MODULEOBJECT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

extern void _PyModule_Clear(PyObject *);
extern void _PyModule_ClearDict(PyObject *);
extern int _PyModuleSpec_IsInitializing(PyObject *);
extern int _PyModuleSpec_GetFileOrigin(PyObject *, PyObject **);
extern int _PyModule_IsPossiblyShadowing(PyObject *);

extern int _PyModule_IsExtension(PyObject *obj);

extern int _PyModule_InitModuleDictWatcher(PyInterpreterState *interp);

typedef int (*_Py_modexecfunc)(PyObject *);

typedef struct {
    PyObject_HEAD
    // For immutable modules to find the mutable state and
    // for logging purposes after md_dict is cleared
    PyObject *md_name;
    int md_frozen;

    // *******************************************************
    // Module state, only available on mutable module objects
    // *******************************************************
    PyObject *md_dict;
    void *md_state;
    PyObject *md_weaklist;
    bool md_token_is_def;  /* if true, `md_token` is the PyModuleDef */
#ifdef Py_GIL_DISABLED
    bool md_requires_gil;
#endif
    Py_ssize_t md_state_size;
    traverseproc md_state_traverse;
    inquiry md_state_clear;
    freefunc md_state_free;
    void *md_token;
    _Py_modexecfunc md_exec;  /* only set if md_token_is_def is true */
} PyModuleObject;

PyAPI_FUNC(PyModuleObject*) _PyInterpreterState_GetModuleState(PyObject *mod);
PyAPI_FUNC(PyModuleObject*) _PyInterpreterState_LookupModuleState(PyObject *mod);

#define _PyModule_CAST(op) \
    (assert(PyModule_Check(op)), _Py_CAST(PyModuleObject*, (op)))

static inline PyModuleDef *_PyModule_GetDefOrNull(PyObject *arg) {
    PyModuleObject *mod = _PyInterpreterState_GetModuleState(arg);
    if (mod == NULL) {
        return NULL;
    }
    if (mod->md_token_is_def) {
        return (PyModuleDef *)mod->md_token;
    }
    return NULL;
}

// Get md_token. Used in _DuringGC functions; must have no side effects.
static inline PyModuleDef *_PyModule_GetToken(PyObject *arg) {
    PyModuleObject *mod = _PyModule_CAST(arg);
    return (PyModuleDef *)mod->md_token;
}

// Get md_state. Used in _DuringGC functions; must have no side effects.
static inline void* _PyModule_GetState(PyObject* mod) {
    PyModuleObject *state = _PyInterpreterState_GetModuleState(mod);
    if (state == NULL) {
        return NULL;
    }
    return state->md_state;
}

static inline PyObject* _PyModule_GetDict(PyObject *mod) {
    assert(PyModule_Check(mod));
    PyModuleObject *state = _PyInterpreterState_GetModuleState(mod);
    if (state == NULL) {
        return NULL;
    }
    PyObject *dict = state->md_dict;
    // _PyModule_GetDict(mod) must not be used after calling module_clear(mod)
    assert(dict != NULL);
    return dict;  // borrowed reference
}

extern PyObject * _PyModule_GetFilenameObject(PyObject *);
extern Py_ssize_t _PyModule_GetFilenameUTF8(
        PyObject *module,
        char *buffer,
        Py_ssize_t maxlen);

PyObject* _Py_module_getattro_impl(PyModuleObject *m, PyObject *name, int suppress);
PyObject* _Py_module_getattro(PyObject *m, PyObject *name);

extern int _Py_module_freeze_hook(PyObject *m);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_MODULEOBJECT_H */
