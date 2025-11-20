#include "Python.h"

#include "pycore_cown.h"
#include "pycore_region.h"
#include "pycore_time.h"          // _PyTime_FromSeconds()
#include "pycore_lock.h"

/* Macro that jumps to error, if the expression `x` does not succeed. */
#define SUCCEEDS(x) { do { int r = (x); if (r != 0) goto error; } while (0); }

// The interpreter id 0 is used. This value will be used to indicate that
// no interpreter owns the cown.
#define CUID_RELEASED ((_PyCown_cuid_t)0xff00ff00ff00ff00LL)

typedef enum CownLockStatus {
    COWN_ACQUIRE_ERROR = -1,
    COWN_ACQUIRE_FAIL = 0,
    COWN_ACQUIRE_SUCEESS = 1
} CownLockStatus;

typedef enum {
    COWN_RELEASED        = 0,
    COWN_ACQUIRED        = 1,
    COWN_PENDING_RELEASE = 2,
    // FIXME(cowns): xFrednet: Do we want/need a poisened state and if
    // so should it be stored here?
    // Cown_POISEN          = 3,
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

    /* A lock used, mainly to support timeouts and queueing for locking.
     * All other functions should use the `owner` to determine if they can
     * access the data or not.
     *
     * Python's mutexes already implement queueing and timeouts in a good way.
     * Later we can role our own, if we need but for not this is better. Note
     * that the optional GIL release from the lock should not be used, as it
     * doesn't seem to account for waiting threads from different interpreters.
     * Therefore, we are responsible for releasing and acquireing the GIL.
     */
    PyMutex lock;
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
    assert(self->value == _PyObject_CAST(region));

    return 0;
}

int _PyCown_RegionClose(_PyCownObject *self, _PyBridgeObject* region, _PyCown_cuid_t cuid) {
    BAIL_UNLESS_OWNED_BY(self, cuid, -1);
    assert(self->value == _PyObject_CAST(region));

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

    // Init the cown as being acquired by the current interpreter
    _Py_atomic_store_uint64(&self->owner, _PyCown_ConcurrentUnitId());
    self->state = COWN_PENDING_RELEASE;
    (void)PyMutex_LockFast(&self->lock);

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

static int
lock_acquire_parse_args(PyObject *args, PyObject *kwds,
                        PyTime_t *timeout)
{
    // Taken from `Modules/_threadmodule.c`

    char *kwlist[] = {"blocking", "timeout", NULL};
    int blocking = 1;
    PyObject *timeout_obj = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|pO:acquire", kwlist,
                                     &blocking, &timeout_obj))
        return -1;

    const PyTime_t unset_timeout = _PyTime_FromSeconds(-1);
    *timeout = unset_timeout;

    if (timeout_obj
        && _PyTime_FromSecondsObject(timeout,
                                     timeout_obj, _PyTime_ROUND_TIMEOUT) < 0)
        return -1;

    if (!blocking && *timeout != unset_timeout ) {
        PyErr_SetString(PyExc_ValueError,
                        "can't specify a timeout for a non-blocking call");
        return -1;
    }
    if (*timeout < 0 && *timeout != unset_timeout) {
        PyErr_SetString(PyExc_ValueError,
                        "timeout value must be a non-negative number");
        return -1;
    }
    if (!blocking)
        *timeout = 0;
    else if (*timeout != unset_timeout) {
        PyTime_t microseconds;

        microseconds = _PyTime_AsMicroseconds(*timeout, _PyTime_ROUND_TIMEOUT);
        if (microseconds > PY_TIMEOUT_MAX) {
            PyErr_SetString(PyExc_OverflowError,
                            "timeout value is too large");
            return -1;
        }
    }
    return 0;
}

/* Attempt to lock the cown.
 *
 * Timeout values:
 * (-1) => Non-blocking locking
 *  (0) => Block with no timeout
 *  (n) => Blocking with timeout
 */
static int cown_lock(_PyCownObject* self, PyTime_t timeout) {
    assert(cown_get_owner(self) != _PyCown_ConcurrentUnitId());

    // Try to lock the mutex directly, without releasing the GIL first
    PyLockStatus r = _PyMutex_LockTimed(&self->lock, 0, _Py_LOCK_DONT_DETACH);

    // The cown is currently owned by something else. Release the GIL and
    // wait for the timeout.
    if (r != PY_LOCK_ACQUIRED && timeout >= 0) {
        // Release the GIL
        Py_BEGIN_ALLOW_THREADS;
    
        // Attempt to lot the mutex. This uses a PyMutex for the locking,
        // timeout and signal handling.
        r = _PyMutex_LockTimed(
            &self->lock,
            timeout,
            _Py_LOCK_DONT_DETACH | _PY_LOCK_HANDLE_SIGNALS
        );
    
        // Acquire the GIL
        Py_END_ALLOW_THREADS;
    }

    // The lock was interrupted
    if (r == PY_LOCK_INTR) {
        return COWN_ACQUIRE_ERROR;
    }

    // The lock acquisition failed
    if (r == PY_LOCK_FAILURE) {
        return COWN_ACQUIRE_FAIL;
    }

    // Set the owner to the current cuid, thereby taking ownership
    _PyCown_cuid_t owner_cuid = CUID_RELEASED;
    _PyCown_cuid_t this_cuid = _PyCown_ConcurrentUnitId();
    if (!_Py_atomic_compare_exchange_uint64(
        &self->owner,
        &owner_cuid,
        this_cuid)
    ) {
        // Failed to set the owner, this should never happen and points
        // to a deeper issue.
        PyErr_Format(
            PyExc_RuntimeError,
            "[BUG] failed to set owner on a locked cown\n"
            "Cown: %U",
            self
        );

        _PyMutex_Unlock(&self->lock);
        return COWN_ACQUIRE_ERROR;
    }

    // Set the state. This doesn't need to be atomic, since this
    // value should only ever be read by the owning interpreter
    self->state = COWN_ACQUIRED;

    return COWN_ACQUIRE_SUCEESS;
}

static PyObject *
CownObject_acquire(_PyCownObject *self, PyObject *args, PyObject *kwds)
{
    // Parse the arguments
    PyTime_t timeout;
    if (lock_acquire_parse_args(args, kwds, &timeout) < 0) {
        return NULL;
    }

    // Attempt to lock the cown
    int res = cown_lock(self, timeout);
    if (res == COWN_ACQUIRE_ERROR) {
        return NULL;
    }

    // Return the result
    return PyBool_FromLong(res == COWN_ACQUIRE_SUCEESS);
}

static PyObject* cown_release_unchecked(_PyCownObject* self) {
    self->state = COWN_RELEASED;

    // Set the owner to indicate the released state
    _PyCown_cuid_t this_cuid = _PyCown_ConcurrentUnitId();
    if (!_Py_atomic_compare_exchange_uint64(&self->owner, &this_cuid, CUID_RELEASED)) {
        PyErr_Format(
            PyExc_RuntimeError,
            "interpreter %lld (this) attempted to release a cown owned by someone else\n"
            "Cown: %U",
            this_cuid, self);
        return NULL;
    }

    // Unlocking should always succeed
    int res = _PyMutex_TryUnlock(&self->lock);
    assert(res == 0);
    (void)res;

    Py_RETURN_NONE;
}

static PyObject* cown_release_region(_PyCownObject* self) {
    assert(_PyRegion_IsBridge(self->value));
    
    // Fetch the region handle to allow 
    Py_region_t region = _PyRegion_Get(self->value);

    // Dirty regions may close after cleaning
    if (_PyRegion_IsDirty(region)) {
        SUCCEEDS(_PyRegion_Clean(region));

        // Update the region handle
        region = _PyRegion_Get(self->value);
    }

    // A closed region is safe to release
    if (_PyRegion_IsOpen(region) == false) {
        return cown_release_unchecked(self);
    }

    PyErr_Format(
        PyExc_RuntimeError,
        "the cown can't be released, since the contained region is still open");
error:
    return NULL;
}

static PyObject* CownObject_release(_PyCownObject *self, PyObject *ignored) {
    _PyCown_cuid_t owner_cuid = cown_get_owner(self);
    _PyCown_cuid_t this_cuid = _PyCown_ConcurrentUnitId();

    // Error if the cown is already released
    if (owner_cuid == CUID_RELEASED) {
        PyErr_Format(
            PyExc_RuntimeError,
            "interpreter %lld attempted to release a released cown",
            this_cuid
        );
        return NULL;
    }

    // Error if the cown is owned by a different interpreter
    if (owner_cuid != this_cuid) {
        PyErr_Format(
            PyExc_RuntimeError,
            "interpreter %lld attempted to release a cown owned by %lld",
            this_cuid, owner_cuid
        );
        return NULL;
    }

    // Validate that the cown is currently not released, otherwise
    // it should have the `CUID_RELEASED` owner.
    assert(self->state != COWN_RELEASED);

    PyObject *value = self->value;

    // Cowns holding cowns or immutable objects can be released without any
    // restrictions
    Py_region_t value_region = _PyRegion_Get(value);
    if (value_region == _Py_COWN_REGION || value_region == _Py_IMMUTABLE_REGION) {
        return cown_release_unchecked(self);
    }

    assert(value_region != _Py_LOCAL_REGION);
    return cown_release_region(self);
}

// Define the CownType with methods
static PyMethodDef PyCown_methods[] = {
    {"acquire", _PyCFunction_CAST(CownObject_acquire), METH_VARARGS | METH_KEYWORDS, "Acquire the cown."},
    {"release", _PyCFunction_CAST(CownObject_release), METH_NOARGS, "Release the cown."},
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

    // The cown is released and can be acquired
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
