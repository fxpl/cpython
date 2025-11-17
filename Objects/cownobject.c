#include "Python.h"

#include "pycore_cown.h"

/* Macro that jumps to error, if the expression `x` does not succeed. */
#define SUCCEEDS(x) { do { int r = (x); if (r != 0) goto error; } while (0); }

extern PyTypeObject PyCown_Type;

typedef enum {
    Cown_RELEASED        = 0,
    Cown_ACQUIRED        = 1,
    Cown_PENDING_RELEASE = 2,
    Cown_POISEN          = 3,
} CownState;

typedef struct PyCownObject {
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
    // FIXME(regions): xFrednet: Make sure that an interpreter releases all cowns on destruction.
    uint64_t owning_ip;

    /* The value stored in the cown. This value may be immutable, another cown
     * or a region object.
     */
    PyObject* value;
} PyCownObject;

static int PyCown_init(PyCownObject *self, PyObject *args, PyObject *kwds) {
    // TODO: Pyrona: should not be needed in the future
    SUCCEEDS(_PyImmutability_Freeze(_PyObject_CAST(&PyCown_Type)));

    static char *kwlist[] = {"value", NULL};
    PyObject *value = NULL;

    // See if we got a value as a keyword argument
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &value)) {
        return -1;  // Return -1 on failure
    }

    int state = Cown_PENDING_RELEASE;
    if (value == NULL) {
        SUCCEEDS(_PyImmutability_Freeze(Py_None));
        value = Py_None;
    }

    // TODO(regions): xFrednet: Validate value is immutable, cown or region

    SUCCEEDS(_PyRegion_AddRef(_PyObject_CAST(self), value));
    self->value = _Py_NewRef(value);
    _Py_atomic_store_int(&self->state, state);

    return 0;
error:
    return -1;
}

static int PyCown_traverse(PyCownObject *self, visitproc visit, void *arg) {
    // FIXME(regions): xFrednet: Traverse should not be called on cowns, since
    // they shouldn't be in the GC or region lists. Meaning, it's probably
    // better to error and detect these cases. But this can only be done once
    // cown is actually removed from the GC list
    Py_VISIT(self->value);
    return 0;
}

static int PyCown_clear(PyCownObject *self) {
    Py_CLEAR(self->value);
    return 0;
}

static void PyCown_dealloc(PyCownObject *self) {
    PyTypeObject *tp = Py_TYPE(self);
    PyObject_GC_UnTrack(_PyObject_CAST(self));
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


PyTypeObject PyCown_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "Cown",                                  /* tp_name */
    sizeof(PyCownObject),                    /* tp_basicsize */
    0,                                       /* tp_itemsize */
    (destructor)PyCown_dealloc,              /* tp_dealloc */
    0,                                       /* tp_vectorcall_offset */
    0,                                       /* tp_getattr */
    0,                                       /* tp_setattr */
    0,                                       /* tp_as_async */
    0, // (reprfunc)PyCown_repr,                   /* tp_repr */
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
    0,                                       /* tp_getset */
    0,                                       /* tp_base */
    0,                                       /* tp_dict */
    0,                                       /* tp_descr_get */
    0,                                       /* tp_descr_set */
    0,                                       /* tp_dictoffset */
    (initproc)PyCown_init,                   /* tp_init */
    0,                                       /* tp_alloc */
    PyType_GenericNew,                       /* tp_new */
};
