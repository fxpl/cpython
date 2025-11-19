#include "Python.h"

#include "pycore_cown.h"
#include "pycore_region.h"

/* Macro that jumps to error, if the expression `x` does not succeed. */
#define SUCCEEDS(x) { do { int r = (x); if (r != 0) goto error; } while (0); }

// The interpreter id 0 is used. This value will be used to indicate that
// no interpreter owns the cown.
#define CUID_RELEASED ((_PyCown_cuid_t)0xff00ff00ff00ff00LL)

typedef enum {
    Cown_RELEASED        = 0,
    Cown_ACQUIRED        = 1,
    Cown_PENDING_RELEASE = 2,
    Cown_POISEN          = 3,
} CownState;

struct _PyCownObject {
    PyObject_HEAD
    /* The current state of this cown object.
     *
     * This value may be read from and written to from different threads.
     * Only use atomic operations to access this field.
     */
    int state;
    /* The id of the interpreter that currently owns this cown.
     *
     * This value may be read from and written to from different threads.
     * Only use atomic operations to access this field.
     */
    // FIXME(cowns): xFrednet: Make sure that an interpreter releases all cowns on destruction.
    _PyCown_cuid_t owner;

    /* The value stored in the cown. This value may be immutable, another cown
     * or a region object.
     */
    PyObject* value;
};

static _PyCown_cuid_t cown_get_owner(_PyCownObject *obj) {
    return _Py_atomic_load_uint64(&obj->owner);
}

#define BAIL_UNLESS_OWNED_BY(o, owned_by, result) \
    do {\
        _PyCown_cuid_t owner = cown_get_owner(_PyCownObject_CAST(o)); \
        if (owner != owned_by) { \
            PyErr_Format( \
                PyExc_RuntimeError, \
                "attempted to access a cown owned by %llu from %llu", \
                owner, owned_by); \
            return result; \
        } \
    } while (0);
#define BAIL_UNLESS_OWNED(o, result) BAIL_UNLESS_OWNED_BY(o, _PyCown_ConcurrentUnitId(), result)
#define BAIL_UNLESS_OWNED_NULL(o) BAIL_UNLESS_OWNED(o, NULL)

static int cown_set_value_unchecked(_PyCownObject* self, PyObject* value) {
    PyObject *old = self->value;

    if (_PyRegion_IsBridge(value)) {
        // Inform owned region about its owner
        if (_PyRegion_SetCown(_PyBridgeObject_CAST(value), self) != 0) {
            return -1;
        }
    }

    // Update the value
    Py_INCREF(value);
    self->value = value;

    if (_PyRegion_IsBridge(old)) {
        // Inform old region about its abondoment
        if (_PyRegion_RemoveCown(_PyBridgeObject_CAST(old), self) != 0) {
            Py_XDECREF(old);
            return -1;
        }
    }

    Py_XDECREF(old);

    return 0;
}

int cown_set_value(_PyCownObject* self, PyObject* value) {
    BAIL_UNLESS_OWNED(self, -1);

    // Bridge objects are allowed
    if (_PyRegion_IsBridge(value)) {
        return cown_set_value_unchecked(self, value);
    }

    // Immutable and cown objects are allowed
    Py_region_t value_region = _PyRegion_Get(value);
    if (value_region == _Py_COWN_REGION || value_region == _Py_IMMUTABLE_REGION) {
        return cown_set_value_unchecked(self, value);
    }

    // Local objects are forbidden
    char const* obj_info = NULL;
    if (value_region == _Py_LOCAL_REGION) {
        obj_info = "local";
    } else {
        obj_info = "owned";
    }

    PyErr_Format(
        PyExc_RuntimeError,
        "attempted to store a %s object in a cown.\n"
        "Only bridges, cown, and immutable objects are allowed",
        obj_info);

    return -1;
}

/* Returns the current concurrent unit used by cowns.
 *
 * The caller must hold the GIL.
 */
_PyCown_cuid_t _PyCown_ConcurrentUnitId(void ) {
    _PyCown_cuid_t cuid = PyInterpreterState_GetID(PyInterpreterState_Get()); 
    assert(cuid != CUID_RELEASED);
    return cuid;
}

int _PyCown_RegionOpen(_PyCownObject *self, _PyBridgeObject* region, _PyCown_cuid_t cuid) {
    BAIL_UNLESS_OWNED_BY(self, cuid, -1);
    return 0;
}

int _PyCown_RegionClose(_PyCownObject *self, _PyBridgeObject* region, _PyCown_cuid_t cuid) {
    BAIL_UNLESS_OWNED_BY(self, cuid, -1);
    return 0;
}

static int PyCown_init(_PyCownObject *self, PyObject *args, PyObject *kwds) {
    // This moves the region into the cown rei
    SUCCEEDS(_PyRegion_SetCownRegion(self));

    // See if we got a value as a keyword argument
    static char *kwlist[] = {"value", NULL};
    PyObject *value = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &value)) {
        return -1;
    }

    // Init the cown as being aquired by the current interpreter
    _Py_atomic_store_uint64(&self->owner, _PyCown_ConcurrentUnitId());
    _Py_atomic_store_int(&self->state, Cown_PENDING_RELEASE);

    // Set the cown value using the internal function for full validation
    SUCCEEDS(cown_set_value(self, value));

    return 0;
error:
    return -1;
}

static int PyCown_traverse(_PyCownObject *self, visitproc visit, void *arg) {
    // FIXME(cowns): xFrednet: Traverse should not be called on cowns, since
    // they shouldn't be in the GC or region lists. Meaning, it's probably
    // better to error and detect these cases. But this can only be done once
    // cown is actually removed from the GC list
    Py_VISIT(self->value);
    return 0;
}

static int PyCown_clear(_PyCownObject *self) {
    cown_set_value_unchecked(self, Py_None);
    Py_CLEAR(self->value);
    return 0;
}

static void PyCown_dealloc(_PyCownObject *self) {
    // Self has already been removed from the GC when it was moved
    // into the cown region.
    Py_TRASHCAN_BEGIN(self, PyCown_dealloc)
    PyCown_clear(self);
    PyObject_GC_Del(self);
    Py_TRASHCAN_END
}

// Define the CownType with methods
static PyMethodDef PyCown_methods[] = {
    // {"acquire", (PyCFunction)PyCown_acquire, METH_NOARGS, "Acquire the cown."},
    // {"release", (PyCFunction)PyCown_release, METH_NOARGS, "Release the cown."},
    // {"get",     (PyCFunction)PyCown_get,     METH_NOARGS, "Get contents of acquired cown."},
    // {"set",     (PyCFunction)PyCown_set,     METH_O, "Set contents of acquired cown."},
    {NULL}  // Sentinel
};

PyObject *CownObject_get_value(_PyCownObject *self, void *closure) {
    BAIL_UNLESS_OWNED_NULL(self);

    return _Py_NewRef(self->value);
}

int CownObject_set_value(_PyCownObject *self, PyObject *value, void *closure) {
    BAIL_UNLESS_OWNED(self, -1);

    return cown_set_value(self, value);
}

static PyGetSetDef PyCownObject_getset[] = {
    {"value", (getter)CownObject_get_value, (setter)CownObject_set_value,
        "", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyObject *PyCown_repr(_PyCownObject *self) {
    _PyCown_cuid_t cuid = cown_get_owner(self);
    // On this interpreter we can access the cown and content
    // safely since we hold the GIL
    if (cuid == _PyCown_ConcurrentUnitId()) {
        return PyUnicode_FromFormat(
            "Cown(interpreter=%llu (this), value=%S)",
            cuid,
            PyObject_Repr(self->value)
        );
    }

    // The cown is released and can be aquired
    if (cuid == CUID_RELEASED) {
        return PyUnicode_FromFormat(
            "Cown(interpreter=None, status=Released)"
        );
    }

    // The cown is owned by a different interpreter
    return PyUnicode_FromFormat(
        "Cown(interpreter=%llu (other))",
        cuid
    );
}

PyTypeObject _PyCown_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "regions.Cown",                          /* tp_name */
    sizeof(_PyCownObject),                   /* tp_basicsize */
    0,                                       /* tp_itemsize */
    (destructor)PyCown_dealloc,              /* tp_dealloc */
    0,                                       /* tp_vectorcall_offset */
    0,                                       /* tp_getattr */
    0,                                       /* tp_setattr */
    0,                                       /* tp_as_async */
    (reprfunc)PyCown_repr,                   /* tp_repr */
    0,                                       /* tp_as_number */
    0,                                       /* tp_as_sequence */
    0,                                       /* tp_as_mapping */
    0,                                       /* tp_hash  */
    0,                                       /* tp_call */
    0,                                       /* tp_str */
    0,                                       /* tp_getattro */
    0,                                       /* tp_setattro */
    0,                                       /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, /* tp_flags */
    0,                                       /* tp_doc */
    (traverseproc)PyCown_traverse,           /* tp_traverse */
    (inquiry)PyCown_clear,                   /* tp_clear */
    0,                                       /* tp_richcompare */
    0,                                       /* tp_weaklistoffset */
    0,                                       /* tp_iter */
    0,                                       /* tp_iternext */
    PyCown_methods,                          /* tp_methods */
    0,                                       /* tp_members */
    PyCownObject_getset,                     /* tp_getset */
    0,                                       /* tp_base */
    0,                                       /* tp_dict */
    0,                                       /* tp_descr_get */
    0,                                       /* tp_descr_set */
    0,                                       /* tp_dictoffset */
    (initproc)PyCown_init,                   /* tp_init */
    0,                                       /* tp_alloc */
    PyType_GenericNew,                       /* tp_new */
};
