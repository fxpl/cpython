/* regions module */

#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#define MODULE_VERSION "1.0"

#include "Python.h"
#include <stdbool.h>
#include "pycore_object.h"
#include "pycore_cown.h"
#include "pycore_ownership.h"
#include "pycore_region.h"
#include "pycore_regionobject.h"

/*[clinic input]
module regions
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=38ff706d605d1871]*/

#include "clinic/regionsmodule.c.h"

/*
 * ===================
 * Module State
 * ===================
 */

typedef struct regions_state {
    PyObject *region_error_obj;
} regions_state;

static struct PyModuleDef regionsmodule;

static inline regions_state*
get_state(PyObject *module)
{
    void *state = PyModule_GetState(module);
    assert(state != NULL);
    return (regions_state *)state;
}

static int
regions_clear(PyObject *module)
{
    regions_state *module_state = get_state(module);
    Py_CLEAR(module_state->region_error_obj);
    return 0;
}

static int
regions_traverse(PyObject *module, visitproc visit, void *arg)
{
    regions_state *module_state = get_state(module);
    Py_VISIT(module_state->region_error_obj);
    return 0;
}

static void
regions_free(void *module)
{
   regions_clear((PyObject *)module);
}

/*
 * ===================
 * RegionError
 * ===================
 */

static PyType_Slot region_error_slots[] = {
    {0, NULL},
};

PyType_Spec regions_error_spec = {
    .name = "regions.RegionError",
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .slots = region_error_slots,
};

/*
 * ===================
 * MODULE
 * ===================
 */

PyDoc_STRVAR(regions_module_doc, "");

/*[clinic input]
regions.is_local -> bool
    obj: object
    /

Return True the object is in the local region.
[clinic start generated code]*/

static int
regions_is_local_impl(PyObject *module, PyObject *obj)
/*[clinic end generated code: output=e113b6b045da92b4 input=9e5338e938093877]*/
{
    return PyRegion_IsLocal(obj);
}

/*[clinic input]
regions.is_owned -> bool
    obj: object
    /

Return True the object is owned by a region.
[clinic start generated code]*/

static int
regions_is_owned_impl(PyObject *module, PyObject *obj)
/*[clinic end generated code: output=121858a6b80c07a6 input=bf22ea6a6fc44cae]*/
{
    return _PyRegion_GetBridge(_PyRegion_Get(obj)) != Py_None;
}

/*[clinic input]

regions.get_region -> object
    obj: object
    /

Returns the region object owning this object.

It returns None if the object is local or immutable.
[clinic start generated code]*/

static PyObject *
regions_get_region(PyObject *module, PyObject *obj)
/*[clinic end generated code: output=8c8f6be3371e4e6e input=abd4a6313cd7a2e1]*/
{
    return PyRegion_NewRef(_PyRegion_GetBridge(_PyRegion_Get(obj)));
}

/*[clinic input]
regions.get_last_dirty_reason

Returns the last reason for marking open regions as dirty.

Return value: str
[clinic start generated code]*/

static PyObject *
regions_get_last_dirty_reason_impl(PyObject *module)
/*[clinic end generated code: output=7fa56844889d85b8 input=56996052e520f95d]*/
{
    return _PyOwnership_get_last_dirty_reason();
}

static struct PyMethodDef regions_methods[] = {
    REGIONS_IS_LOCAL_METHODDEF
    REGIONS_IS_OWNED_METHODDEF
    REGIONS_GET_REGION_METHODDEF
    REGIONS_GET_LAST_DIRTY_REASON_METHODDEF
    { NULL, NULL }
};


static int
regions_exec(PyObject *module) {
    regions_state *module_state = get_state(module);

    /* Add version to the module. */
    if (PyModule_AddStringConstant(module, "__version__",
                                    MODULE_VERSION) == -1) {
        return -1;
    }

    // Create the `RegionError` type
    PyObject *bases = PyTuple_Pack(1, PyExc_TypeError);
    if (bases == NULL) {
        return -1;
    }
    module_state->region_error_obj = PyType_FromModuleAndSpec(
        module,
        &regions_error_spec,
        bases);
    Py_DECREF(bases);
    if (module_state->region_error_obj == NULL) {
        return -1;
    }
    if (PyModule_AddType(module, (PyTypeObject *)module_state->region_error_obj) != 0) {
        return -1;
    }

    // Register the `Region` type
    if (PyType_Ready(&_PyRegion_Type) < 0) {
        return -1;
    }
    if (_PyImmutability_Freeze(_PyObject_CAST(&_PyRegion_Type)) != 0) {
        return -1;
    }
    _Py_SetImmortalUntracked(_PyObject_CAST(&_PyRegion_Type));
    if (PyModule_AddObject(module, "Region", _PyObject_CAST(&_PyRegion_Type)) < 0) {
        return -1;
    }

    // Register the `Cown` type
    if (PyType_Ready(&_PyCown_Type) < 0) {
        return -1;
    }
    if (_PyImmutability_Freeze(_PyObject_CAST(&_PyCown_Type)) != 0) {
        return -1;
    }
    _Py_SetImmortalUntracked(_PyObject_CAST(&_PyCown_Type));
    if (PyModule_AddObject(module, "Cown", _PyObject_CAST(&_PyCown_Type)) < 0) {
        return -1;
    }

    // Freeze the dict type, to allow dictionaries to be used across regions.
    if (_PyImmutability_Freeze(_PyObject_CAST(&PyDict_Type)) != 0) {
        return -1;
    }

    // Freeze the `None` struct
    if (_PyImmutability_Freeze(_PyObject_CAST(Py_None)) != 0) {
        return -1;
    }

    // Disable the invariant again, since it slows Python down so much
    if (_PyOwnership_invariant_disable() != 0) {
        return -1;
    }

    return 0;
}

static PyModuleDef_Slot regions_slots[] = {
    {Py_mod_exec, regions_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_USED},
    {0, NULL}
};

static struct PyModuleDef regionsmodule = {
    PyModuleDef_HEAD_INIT,
    "regions",
    regions_module_doc,
    sizeof(regions_state),
    regions_methods,
    regions_slots,
    regions_traverse,
    regions_clear,
    regions_free
};

PyMODINIT_FUNC
PyInit_regions(void)
{
    return PyModuleDef_Init(&regionsmodule);
}
