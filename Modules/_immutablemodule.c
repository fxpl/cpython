/* _immutable module */

#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#define MODULE_VERSION "1.0"

#include "Python.h"
#include <stdbool.h>
#include "pycore_object.h"

/*[clinic input]
module _immutable
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=292286c0a14cb0ff]*/

#include "clinic/_immutablemodule.c.h"

typedef struct {
    PyObject *not_freezable_error_obj;
} immutable_state;

static struct PyModuleDef _immutablemodule;

static inline immutable_state*
get_immutable_state(PyObject *module)
{
    void *state = PyModule_GetState(module);
    assert(state != NULL);
    return (immutable_state *)state;
}

static int
immutable_clear(PyObject *module)
{
    immutable_state *module_state = PyModule_GetState(module);
    Py_CLEAR(module_state->not_freezable_error_obj);
    return 0;
}

static int
immutable_traverse(PyObject *module, visitproc visit, void *arg)
{
    immutable_state *module_state = PyModule_GetState(module);
    Py_VISIT(module_state->not_freezable_error_obj);
    return 0;
}

static void
immutable_free(void *module)
{
   immutable_clear((PyObject *)module);
}

/*[clinic input]
_immutable.register_freezable
    obj: object
    /

Register a type as freezable.
[clinic start generated code]*/

static PyObject *
_immutable_register_freezable(PyObject *module, PyObject *obj)
/*[clinic end generated code: output=7a68ab35ee36a572 input=48ad5294977fe780]*/
{
    if(!PyType_Check(obj)){
        PyErr_SetString(PyExc_TypeError, "Expected a type");
        return NULL;
    }

    if(_PyImmutability_RegisterFreezable((PyTypeObject *)obj) < 0){
        return NULL;
    }

    Py_RETURN_NONE;
}

/*[clinic input]
_immutable.freeze
    obj: object
    /

Freeze an object and its graph.
[clinic start generated code]*/

static PyObject *
_immutable_freeze(PyObject *module, PyObject *obj)
/*[clinic end generated code: output=7612b209b2d604ab input=3e8ad29453cf365a]*/
{
    if(_PyImmutability_Freeze(obj) < 0){
        return NULL;
    }

    Py_RETURN_NONE;
}

/*[clinic input]
_immutable.isfrozen
    obj: object
    /

Check if an object is frozen.
[clinic start generated code]*/

static PyObject *
_immutable_isfrozen(PyObject *module, PyObject *obj)
/*[clinic end generated code: output=5857a038e2a68ed7 input=8dc5ebd880c4c8b2]*/
{
    if(_Py_IsImmutable(obj)){
        Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
}

static PyType_Slot not_freezable_error_slots[] = {
    {0, NULL},
};

PyType_Spec not_freezable_error_spec = {
    .name = "_immutable.NotFreezableError",
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .slots = not_freezable_error_slots,
};

/*
 * MODULE
 */


PyDoc_STRVAR(immutable_module_doc,
"_immutable\n"
"--\n"
"\n"
"Module for immutability support.\n"
"\n"
"This module provides functions to freeze objects and their graphs,\n"
"making them immutable at runtime.");

static struct PyMethodDef immutable_methods[] = {
    IMMUTABLE_REGISTER_FREEZABLE_METHODDEF
    IMMUTABLE_FREEZE_METHODDEF
    IMMUTABLE_ISFROZEN_METHODDEF
    { NULL, NULL }
};


static int
immutable_exec(PyObject *module) {
    immutable_state *module_state = get_immutable_state(module);

    /* Add version to the module. */
    if (PyModule_AddStringConstant(module, "__version__",
                                    MODULE_VERSION) == -1) {
        return -1;
    }

    PyObject *bases = PyTuple_Pack(1, PyExc_TypeError);
    if (bases == NULL) {
        return -1;
    }
    module_state->not_freezable_error_obj = PyType_FromModuleAndSpec(module, &not_freezable_error_spec,
                                                        bases);
    Py_DECREF(bases);
    if (module_state->not_freezable_error_obj == NULL) {
        return -1;
    }

    if (PyModule_AddType(module, (PyTypeObject *)module_state->not_freezable_error_obj) != 0) {
        return -1;
    }

    if (PyModule_AddType(module, &_PyNotFreezable_Type) != 0) {
        return -1;
    }

    return 0;
}

static PyModuleDef_Slot immutable_slots[] = {
    {Py_mod_exec, immutable_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED}, // TODO(Immutable):  This is probably not true, just enabling to see what breaks.
    {0, NULL}
};

static struct PyModuleDef _immutablemodule = {
    PyModuleDef_HEAD_INIT,
    "_immutable",
    immutable_module_doc,
    sizeof(immutable_state),
    immutable_methods,
    immutable_slots,
    immutable_traverse,
    immutable_clear,
    immutable_free
};

PyMODINIT_FUNC
PyInit__immutable(void)
{
    return PyModuleDef_Init(&_immutablemodule);
}
