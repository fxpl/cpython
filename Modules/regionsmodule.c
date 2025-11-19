/* regions module */

#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#define MODULE_VERSION "1.0"

#include "Python.h"
#include <stdbool.h>
#include "pycore_object.h"
#include "pycore_region.h"
#include "pycore_cown.h"
#include "pycore_ownership.h"

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

void RegionErr_NoBridge(void) {
    // FIXME Static RegionError and call
    PyErr_Format(
        PyExc_RuntimeError,
        "a region method was called on a non-bridge object");
}

/*
 * ===================
 * Region Object
 * ===================
 */

PyDoc_STRVAR(Region_doc, "FIXME =^.^=");

typedef struct RegionObject {
    PyBridgeObject_HEAD
    PyObject *dict;
} RegionObject;

#define RegionObject_CAST(op)  ((RegionObject *)(op))

static PyMemberDef Region_members[] = {
    {"__dict__", _Py_T_OBJECT, offsetof(RegionObject, dict), Py_READONLY},
    {NULL}
};

static int Region_init(RegionObject *self, PyObject *args, PyObject *kwds) {
    // Parse optional parameter
    static char *kwlist[] = {"name", NULL};
    PyObject *name = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|U", kwlist, &name)) {
        return -1;
    }

    // Strings are often interned, which makes sharing complicated. But
    // they are effectivly immutable, which makes freezing a simple and
    // safe fix.
    if (name && _PyImmutability_Freeze(name)) {
        return -1;
    }

    PyBridgeObject_HEAD_INIT(self);

    // Allocate the new region object
    if (_PyRegion_New(_PyBridgeObject_CAST(self))) {
        return -1;
    }
    assert(self->region != NULL_REGION);

    // Check the object is also correctly moved into the region
    assert(_PyRegion_Get(_PyObject_CAST(self)) == self->region);
    assert(_PyRegion_IsBridge(_PyObject_CAST(self)));

    // No write barrier needed, since name is frozen
    self->name = _Py_XNewRef(name);

    // Everything is a-okay
    return 0;
}

static PyObject *
Region_repr(PyObject *op)
{
    if (!_PyRegion_IsBridge(op)) {
        return PyUnicode_FromString("<Region (merged)>");;
    }

    RegionObject *self = RegionObject_CAST(op);
    Py_region_t region = _PyRegion_Get(op);

    PyObject *repr = NULL;
#ifdef Py_DEBUG
    repr = PyUnicode_FromFormat(
        "Region(name=%R _lrc=%zu _osc=%zu is_dirty=%s)",
        self->name ? self->name : Py_None,
        _PyRegion_GetLrc(region),
        _PyRegion_GetOsc(region),
        _PyRegion_IsDirty(region) ? "True" : "False"
    );
#else
    repr = PyUnicode_FromFormat(
        "Region(name=%R)",
        self->name ? self->name : Py_None
    );
#endif

    return repr;
}

#define CHECK_BRIDGE(self) \
    if (!_PyRegion_IsBridge(_PyObject_CAST(self))) { \
        RegionErr_NoBridge(); \
        return NULL; \
    }

static PyObject* Region_owns(PyObject *self, PyObject *other) {
    CHECK_BRIDGE(self);

    Py_region_t self_region = _PyRegion_Get(self);
    Py_region_t other_region = _PyRegion_Get(other);
    return PyBool_FromLong(self_region == other_region);
}

static PyObject* Region_clean(PyObject *op) {
    CHECK_BRIDGE(op);

    if (_PyRegion_Clean(_PyRegion_Get(op))) {
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyMethodDef Region_methods[] = {
    {"owns", _PyCFunction_CAST(Region_owns), METH_O,
        "Check if object is owned by the region."},
    {"clean", _PyCFunction_CAST(Region_clean), METH_NOARGS,
        "Cleans the region and any dirty subregions"},
    {NULL,              NULL}           /* sentinel */
};

static PyObject* Region_is_open(PyObject *self, void *closure) {
    CHECK_BRIDGE(self);

    int is_open = _PyRegion_IsOpen(_PyRegion_Get(self));
    return PyBool_FromLong(is_open);
}

static PyObject* Region_is_dirty(PyObject *self, void *closure) {
    CHECK_BRIDGE(self);

    int is_dirty = _PyRegion_IsDirty(_PyRegion_Get(self));
    return PyBool_FromLong(is_dirty);
}

static PyObject* Region_get_parent(PyObject *self, void *closure) {
    CHECK_BRIDGE(self);

    Py_region_t parent_region = _PyRegion_GetParent(_PyRegion_Get(self));
    return _Py_NewRef(_PyRegion_GetBridge(parent_region));
}

static PyObject* Region_get_name(PyObject *self, void *closure) {
    CHECK_BRIDGE(self);

    return Py_NewRef(RegionObject_CAST(self)->name);
}

static PyObject* Region_get__lrc(PyObject* self, void* closure) {
    CHECK_BRIDGE(self);

    Py_ssize_t lrc = _PyRegion_GetLrc(_PyRegion_Get(self));
    return PyLong_FromSize_t(lrc);
}

static PyObject* Region_get__osc(PyObject* self, void* closure) {
    CHECK_BRIDGE(self);

    Py_ssize_t osc = _PyRegion_GetOsc(_PyRegion_Get(self));
    return PyLong_FromSize_t(osc);
}

static PyGetSetDef Region_getset[] = {
    {"is_open", (getter)Region_is_open, NULL,
        "indicates if the region is currently open or closed", NULL},
    {"is_dirty", (getter)Region_is_dirty, NULL,
        "indicates if the region is currently dirty", NULL},
    {"parent", (getter)Region_get_parent, NULL,
        "the parent of the region", NULL},
    {"name", (getter)Region_get_name, NULL,
        "the name of the region", NULL},
    {"_lrc", (getter)Region_get__lrc, NULL, 
        "the local-reference count, mainly intended for debugging", NULL},
    {"_osc", (getter)Region_get__osc, NULL, 
        "the open-subregion count, mainly intended for debugging", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static int
Region_traverse(PyObject *op, visitproc visit, void *arg)
{
    RegionObject *self = RegionObject_CAST(op);

    // Visit the type
    Py_VISIT(Py_TYPE(op));

    // Only visit the name from the root bridge object
    if (_PyRegion_IsBridge(op)) {
        Py_VISIT(self->name);
    }

    // Visit the attribute dict
    Py_VISIT(self->dict);
    return 0;
}

static int
Region_clear(PyObject *op)
{
    RegionObject *self = RegionObject_CAST(op);

    if (self->region != NULL_REGION) {
        // This merges this region into the local region. This is done because:
        // (1) Once the bridge is gone, there is no way to send the region
        //     anymore therefore there is no advantage of tracking ownership
        //     for these objects
        // (2) Clear might propagate through the object graph. This previously
        //     caused some asserts to fail, which assumed the bridge to always
        //     be there.
        // (3) Only guessing, but merging the region back into the local region
        //     will probably be good for usability, since there is more freedom
        //     to reference previously contained objects.
        _PyRegion_Dissolve(self->region);

        // Clear the region, this uses the internal region pointer
        // since `_PyRegion_Get` might be different or already cleared.
        _PyRegion_RemoveBridge(self->region);
        _PyRegion_DecRc(self->region);
        self->region = NULL_REGION;
    }

    // Clear members
    Py_CLEAR(self->name);
    Py_CLEAR(self->dict);
    return 0;
}

static void
Region_dealloc(PyObject *self)
{
    // The region in the `ob_region` field should be cleared before calling
    // dealloc.
    assert(self->ob_region == NULL_REGION);

    PyObject_GC_UnTrack(self);

    Region_clear(self);

    PyTypeObject *tp = Py_TYPE(self);
    freefunc free = PyType_GetSlot(tp, Py_tp_free);
    free(self);
}

/* The region type is intentionally static and immutable to allow save sharing
 * across subinterpreters. Declaring it as static allows type comparisons to
 * work automatically.
 *
 * One downside is, that the normal `PyType_GetModuleState` function doesn't
 * work for static types. So everthing needs to either use static types or
 * look up the `regions` module dynamically at runtime.
 */
static PyTypeObject Region_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "regions.Region",
    .tp_basicsize = sizeof(RegionObject),
    // .tp_itemsize = 0,
    .tp_dealloc = (destructor)Region_dealloc,
    .tp_getset = Region_getset,
    // .tp_vectorcall_offset = 0,
    // .tp_getattr = 0,
    // .tp_setattr = 0,
    // .tp_as_async = 0,
    .tp_repr = (reprfunc)Region_repr,
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

/*[clinic input]
regions.is_local -> bool
    obj: object
    /

Return True the object is in the local region.
[clinic start generated code]*/

static int
regions_is_local_impl(PyObject *module, PyObject *obj)
/*[clinic end generated code: output=e113b6b045da92b4 input=17b3dedc5693f308]*/
{
    return _Py_IsLocal(obj);
}

static struct PyMethodDef regions_methods[] = {
    REGIONS_IS_LOCAL_METHODDEF
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
