/* regions module */

#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#define MODULE_VERSION "1.0"

#include "Python.h"
#include <stdbool.h>
#include "pycore_object.h"
#include "pycore_region.h"

/*[clinic input]
module regions
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=38ff706d605d1871]*/

typedef struct {
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

static PyType_Slot region_error_slots[] = {
    {0, NULL},
};

PyType_Spec regions_error_spec = {
    .name = "regions.RegionError",
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .slots = region_error_slots,
};

/*
 * MODULE
 */


PyDoc_STRVAR(regions_module_doc, "");

static struct PyMethodDef regions_methods[] = {
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
