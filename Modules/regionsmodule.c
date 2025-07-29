/* regions module */

#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#define MODULE_VERSION "1.0"

#include "Python.h"
#include <stdbool.h>
#include "pycore_object.h"
#include "pycore_region.h"
#include "pycore_ownership.h"

/*[clinic input]
module regions
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=38ff706d605d1871]*/

/*
 * ===================
 * Module State
 * ===================
 */

typedef struct regions_state {
    PyObject *region_error_obj;
    PyObject *region_type;
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
 * Region Object
 * ===================
 */

PyDoc_STRVAR(Region_doc, "TODO =^.^=");

typedef struct RegionObject {
    PyObject_HEAD
    /* A pointer to the region object, this is needed to access the region
     * in the dealloc function when the region field in the object has
     * already been cleared.
     */
    Py_region_t region;
    PyObject *dict;
} RegionObject;

#define RegionObject_CAST(op)  ((RegionObject *)(op))

static PyMemberDef Region_members[] = {
    {"__dict__", _Py_T_OBJECT, offsetof(RegionObject, dict), Py_READONLY},
    {NULL}
};

// static RegionObject* newRegionObject(PyObject *module) {
//     regions_state *state = get_state(module);
//     if (state == NULL) {
//         return NULL;
//     }
    
//     RegionObject *self;
//     self = PyObject_GC_New(RegionObject, (PyTypeObject*)state->region_type);
//     if (self == NULL) {
//         return NULL;
//     }

//     self->region = _PyRegion_New(_PyObject_CAST(self));
//     if (region == NULL_REGION) {
//         PyObject_GC_Del(self);
//         return NULL;
//     }

//     return self;
// }

static int Region_init(RegionObject *self, PyObject *args, PyObject *kwds) {
    // Parse optional parameter
    static char *kwlist[] = {"name", NULL};
    PyObject *name = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|U", kwlist, &name)) {
        return -1;
    }
    assert(name == NULL && "TODO(region): xFrednet Handle Name");

    // Allocate the new region object
    self->region = _PyRegion_New(_PyObject_CAST(self));
    if (self->region == NULL_REGION) {
        return -1;
    }

    // Check the object is alos correctly moved into the region
    assert(_PyRegion_Get(_PyObject_CAST(self)) == self->region);
    assert(_PyRegion_GetBridge(_PyObject_CAST(self)) == _PyObject_CAST(self));

    // Everything is a-okay
    return 0;
}

static PyObject *Region_owns_object(RegionObject *self, PyObject *other) {
    if (_PyRegion_Get(_PyObject_CAST(self)) == _PyRegion_Get(other)) {
        Py_RETURN_TRUE;
    } else {
        Py_RETURN_FALSE;
    }
}

static int
Region_traverse(PyObject *op, visitproc visit, void *arg)
{
    // Visit the type
    Py_VISIT(Py_TYPE(op));

    // Visit the attribute dict
    RegionObject *self = RegionObject_CAST(op);
    Py_VISIT(self->dict);
    return 0;
}

static int
Region_clear(PyObject *op)
{
    RegionObject *self = RegionObject_CAST(op);

    // Clear the region, this uses the internal region pointer
    // since `_PyRegion_Get` might be different or already cleared.
    _PyRegion_DecRc(self->region);
    self->region = NULL_REGION;

    // Clear members
    Py_CLEAR(self->dict);
    return 0;
}

static void
Region_dealloc(PyObject *self)
{
    PyObject_GC_UnTrack(self);
    PyTypeObject *tp = Py_TYPE(self);
    freefunc free = PyType_GetSlot(tp, Py_tp_free);
    free(self);
    Py_DECREF(tp);
}

static PyMethodDef Region_methods[] = {
    {"owns_object", _PyCFunction_CAST(Region_owns_object), METH_O,
        "Check if object is owned by the region."},
    {NULL,              NULL}           /* sentinel */
};

/* The region type is intentionally static and immutable to allow save sharing
 * across subinterpreters. Declaring it as static allows type comparisons to
 * work automatically.
 */
static PyTypeObject Region_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "regions.Region",
    .tp_basicsize = sizeof(RegionObject),
    // .tp_itemsize = 0,
    .tp_dealloc = (destructor)Region_dealloc,
    // .tp_vectorcall_offset = 0,
    // .tp_getattr = 0,
    // .tp_setattr = 0,
    // .tp_as_async = 0,
    // .tp_repr = (reprfunc)PyRegion_repr,
    // .tp_as_number = 0,
    // .tp_as_sequence = 0,
    // .tp_as_mapping = 0,
    // .tp_hash  = 0,
    // .tp_call = 0,
    // .tp_str = 0,
    // .tp_getattro = 0,
    // .tp_setattro = 0,
    // .tp_as_buffer = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .tp_doc = Region_doc,
    .tp_traverse = (traverseproc)Region_traverse,
    .tp_clear = (inquiry)Region_clear,
    // .tp_richcompare = 0,
    // .tp_weaklistoffset = 0,
    // .tp_iter = 0,
    // .tp_iternext = 0,
    .tp_methods = Region_methods,
    .tp_members = Region_members,
    // .tp_getset = 0,
    // .tp_base = 0,
    // .tp_dict = 0,
    // .tp_descr_get = 0,
    // .tp_descr_set = 0,
    .tp_dictoffset = offsetof(RegionObject, dict),
    .tp_init = (initproc)Region_init,
    // .tp_alloc = 0,
    .tp_new = PyType_GenericNew,
};

/*
 * ===================
 * MODULE
 * ===================
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
    if (PyType_Ready(&Region_Type) < 0) {
        return -1;
    }
    if (_PyImmutability_Freeze(_PyObject_CAST(&Region_Type)) != 0) {
        return -1;
    }
    _Py_SetImmortalUntracked(_PyObject_CAST(&Region_Type));
    if (PyModule_AddObject(module, "Region", _PyObject_CAST(&Region_Type)) < 0) {
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
