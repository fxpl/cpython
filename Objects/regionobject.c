#include "Python.h"
#include "pymacro.h"
#include "pycore_region.h"
#include "pycore_regionobject.h"

PyDoc_STRVAR(Region_doc, "FIXME(regions): =^.^=");

#define CHECK_BRIDGE(self) \
    if (!_PyRegion_IsBridge(_PyObject_CAST(self))) { \
        RegionErr_NoBridge(); \
        return NULL; \
    }

void RegionErr_NoBridge(void) {
    // FIXME Static RegionError and call
    PyErr_Format(
        PyExc_RuntimeError,
        "a region method was called on a non-bridge object");
}

static int Region_init(_PyRegionObject *self, PyObject *args, PyObject *kwds) {
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

    // Regions should not be tracked in normal GC, those fields will be
    // used to track subregions 
    PyObject_GC_UnTrack(self);

    self->region = NULL_REGION;
    self->name = NULL;

    // Allocate the new region object
    if (_PyRegion_New(_PyRegionObject_CAST(self))) {
        return -1;
    }
    assert(self->region != NULL_REGION);

    // Check the object is also correctly moved into the region
    assert(_PyRegion_Get(self) == self->region);
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

    _PyRegionObject *self = _PyRegionObject_CAST(op);
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

static PyObject* Region_owns(PyObject *self, PyObject *other) {
    CHECK_BRIDGE(self);

    Py_region_t self_region = _PyRegion_Get(self);
    Py_region_t other_region = _PyRegion_Get(other);
    return PyBool_FromLong(self_region == other_region);
}

static PyObject* Region_clean(PyObject *op) {
    CHECK_BRIDGE(op);

    int cleaning_res = _PyRegion_Clean(_PyRegion_Get(op));
    if (cleaning_res < 0) {
        return NULL;
    }

    return PyLong_FromInt32(cleaning_res);
}

static PyObject* Region__make_dirty(PyObject *op) {
    CHECK_BRIDGE(op);

    _PyRegion_MakeDirty(_PyRegion_Get(op));

    Py_RETURN_NONE;
}

static PyMethodDef Region_methods[] = {
    {"owns", _PyCFunction_CAST(Region_owns), METH_O,
        "Check if object is owned by the region."},
    {"clean", _PyCFunction_CAST(Region_clean), METH_NOARGS,
        "Cleans the region and any dirty subregions"},
    {"_make_dirty", _PyCFunction_CAST(Region__make_dirty), METH_NOARGS,
        "Marks the given region as dirty"},
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
    return _PyRegion_NewRef(_PyRegion_GetBridge(parent_region));
}

static PyObject* Region_get_name(PyObject *self, void *closure) {
    CHECK_BRIDGE(self);

    return PyRegion_NewRef(_PyRegionObject_CAST(self)->name);
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

static PyObject* Region_get__subregions(PyObject* self, void* closure) {
    CHECK_BRIDGE(self);

    return _PyRegion_GetSubregions(_PyRegion_Get(self));
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
    {"_subregions", (getter)Region_get__subregions, NULL, 
        "returns a list of all subregions, mainly intended for debugging", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyMemberDef Region_members[] = {
    {"__dict__", _Py_T_OBJECT, offsetof(_PyRegionObject, dict), Py_READONLY},
    {0}
};

static int
Region_traverse(PyObject *op, visitproc visit, void *arg)
{
    _PyRegionObject *self = _PyRegionObject_CAST(op);

    // Visit the type
    Py_VISIT(Py_TYPE(op));

    // Only visit the name from the root bridge object
    Py_VISIT(self->name);

    // Visit the attribute dict
    Py_VISIT(self->dict);
    return 0;
}

static int
Region_clear(PyObject *op)
{
    _PyRegionObject *self = _PyRegionObject_CAST(op);

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

    // Clear members. This doesn't need write barriers since both are owned by
    // this region
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

PyTypeObject _PyRegion_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "regions.Region",                        /* tp_name */
    sizeof(_PyRegionObject),                 /* tp_basicsize */
    0,                                       /* tp_itemsize */
    (destructor)Region_dealloc,              /* tp_dealloc */
    0,                                       /* tp_vectorcall_offset */
    0,                                       /* tp_getattr */
    0,                                       /* tp_setattr */
    0,                                       /* tp_as_async */
    (reprfunc)Region_repr,                   /* tp_repr */
    0,                                       /* tp_as_number */
    0,                                       /* tp_as_sequence */
    0,                                       /* tp_as_mapping */
    0,                                       /* tp_hash  */
    0,                                       /* tp_call */
    0,                                       /* tp_str */
    0,                                       /* tp_getattro */
    0,                                       /* tp_setattro */
    0,                                       /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE, /* tp_flags */
    Region_doc,                              /* tp_doc */
    (traverseproc)Region_traverse,           /* tp_traverse */
    (inquiry)Region_clear,                   /* tp_clear */
    0,                                       /* tp_richcompare */
    0,                                       /* tp_weaklistoffset */
    0,                                       /* tp_iter */
    0,                                       /* tp_iternext */
    Region_methods,                          /* tp_methods */
    Region_members,                          /* tp_members */
    Region_getset,                           /* tp_getset */
    0,                                       /* tp_base */
    0,                                       /* tp_dict */
    0,                                       /* tp_descr_get */
    0,                                       /* tp_descr_set */
    offsetof(_PyRegionObject, dict),         /* tp_dictoffset */
    (initproc)Region_init,                   /* tp_init */
    0,                                       /* tp_alloc */
    PyType_GenericNew,                       /* tp_new */
    .tp_flags2 = Py_TPFLAGS2_REGION_AWARE
};
