#include "Python.h"
#include "pymacro.h"

#include "pycore_cown.h"
#include "pycore_immutability.h"
#include "pycore_lock.h"
#include "pycore_time.h"          // _PyTime_FromSeconds()

/* Macro that jumps to error, if the expression `x` does not succeed. */
#define SUCCEEDS(x) { do { int r = (x); if (r != 0) goto error; } while (0); }

#define Region_Check(x) Py_IS_TYPE((x), &_PyTracingRegion_Type)

#ifdef _Py_PYRONA_INTERPRETER_SHARING
// The interpreter id 0 is used. This value will be used to indicate that
// no interpreter owns the cown.
#define RELEASED_OWNER_ID ((_PyCown_owner_id_t)0xff00ff00ff00ff00LL)
#define NO_BLOCKING_TIMEOUT -1
#else
#define RELEASED_OWNER_ID _Py_UNOWNED_TID
#endif

typedef enum CownLockStatus {
    COWN_ACQUIRE_ERROR = -1,
    COWN_ACQUIRE_FAIL = 0,
    COWN_ACQUIRE_SUCCESS = 1
} CownLockStatus;

struct _PyCownObject {
    PyObject_HEAD

    /* The ID of the owner.
     *
     * On NoGIL this is the thread ID, on GIL-enabled Python this is the
     * sub-interpreter ID.
     */
    _PyCown_owner_id_t owner_id;

    /* The value stored in the cown. This value may be immutable, another cown
     * or a region object.
     */
    PyObject* value;

    /* A lock used, mainly to support timeouts and queueing for locking.
     * All other functions should use `owner_id` to determine if they can
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

static _PyCown_owner_id_t cown_get_owner(_PyCownObject *obj) {
    return _Py_atomic_load_uintptr_relaxed(&obj->owner_id);
}

#define BAIL_UNLESS_OWNED_BY(o, tested_owner, result) \
    do {\
        _PyCown_owner_id_t owning_id = cown_get_owner(_PyCownObject_CAST(o)); \
        if (owning_id != tested_owner) { \
            PyErr_Format( \
                PyExc_RuntimeError, \
                "attempted to access a cown owned by %llu from %llu", \
                owning_id, tested_owner); \
            return result; \
        } \
    } while (0);
#define BAIL_UNLESS_OWNED(o, result) BAIL_UNLESS_OWNED_BY(o, _PyCown_ThisOwnerId(), result)
#define BAIL_UNLESS_OWNED_NULL(o) BAIL_UNLESS_OWNED(o, NULL)

static int cown_set_value_unchecked(_PyCownObject* self, PyObject* value) {
    // This doesn't require a lock since only the owning thread can read and
    // write to self->value
    Py_XSETREF(self->value, Py_NewRef(value));

    return 0;
}

static int cown_set_value(_PyCownObject* self, PyObject* value) {
    BAIL_UNLESS_OWNED(self, -1);

    // Bridge objects are allowed
    if (Region_Check(value)) {
        return cown_set_value_unchecked(self, value);
    }

    // Immutable objects are allowed
    if (_PyImmutability_CanViewAsDeepImmutable(value)) {
        return cown_set_value_unchecked(self, value);
    }

    // Local objects are forbidden
    PyErr_Format(
        PyExc_RuntimeError,
        "attempted to store a local mutable object in a cown.\n"
        "Only regions, cown, and immutable objects are allowed");

    return -1;
}

/* Attempt to lock the cown.
 *
 * Timeout values:
 * (-1) => Non-blocking locking
 *  (0) => Block with no timeout
 *  (n) => Blocking with timeout
 */
static int cown_lock(_PyCownObject* self, PyTime_t timeout, _PyCown_owner_id_t owner_id, bool has_gil) {
    // A blocking time should only be set, if this call holds the GIL
    assert(has_gil || timeout == NO_BLOCKING_TIMEOUT);

    // Try to lock the mutex directly, without releasing the GIL first
    PyLockStatus r = _PyMutex_LockTimed(&self->lock, 0, _Py_LOCK_DONT_DETACH);

    // The cown is currently owned by something else. Release the GIL and
    // wait for the timeout.
    if (r != PY_LOCK_ACQUIRED && timeout != NO_BLOCKING_TIMEOUT) {
        // Release the GIL
        Py_BEGIN_ALLOW_THREADS;

        // Attempt to lock the mutex. This uses a PyMutex for the locking,
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

    // Set the owner_ip to this thread/interpreter, thereby taking ownership
    _PyCown_owner_id_t released_value = RELEASED_OWNER_ID;
    if (!_Py_atomic_compare_exchange_uintptr(
        &self->owner_id,
        &released_value,
        owner_id)
    ) {
        // Failed to set owning_ip, this should never happen and points
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

    // Only untrack objects if we shared them across sub-interpreters
#ifdef _Py_PYRONA_INTERPRETER_SHARING
    if (self->value && Region_Check(self->value)) {
        assert(!PyObject_GC_IsTracked(self->value));
        PyObject_GC_Track(self->value);
    }
#endif

    return COWN_ACQUIRE_SUCCESS;
}

_PyCown_owner_id_t _PyCown_ThisOwnerId(void) {
#ifdef _Py_PYRONA_INTERPRETER_SHARING
    _PyCown_owner_id_t ip = PyInterpreterState_GetID(PyInterpreterState_Get());
    return ip;
#else
    return _Py_ThreadId();
#endif
}

static int PyCown_init(_PyCownObject *self, PyObject *args, PyObject *kwds) {
    // See if we got a value as a keyword argument
    static char *kwlist[] = {"value", NULL};
    PyObject *value = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &value)) {
        return -1;
    }

    // Init the cown as being acquired by this owner
    _PyCown_owner_id_t this_owner = _PyCown_ThisOwnerId();
    _Py_atomic_store_uintptr_relaxed(&self->owner_id, RELEASED_OWNER_ID);
    if (cown_lock(self, NO_BLOCKING_TIMEOUT, this_owner, true) != COWN_ACQUIRE_SUCCESS) {
        PyErr_Format(
            PyExc_RuntimeError,
            "Newly created cown couldn't be acquired by this owner %lld",
            this_owner);
        return -1;
    }

    // Set the cown value using the internal function for full validation
    SUCCEEDS(cown_set_value(self, value));

    // For sub-interpreters, we need to enable atomic RC and untrack the object
#ifdef _Py_PYRONA_INTERPRETER_SHARING
    _Py_EnableAtomicRC(self);
    PyObject_GC_UnTrack(self);
#endif

    return 0;
error:
    return -1;
}

static int PyCown_traverse(_PyCownObject *self, visitproc _ignore1, void* _ignore2) {
#ifdef _Py_PYRONA_INTERPRETER_SHARING
    // tp_traverse should never be called on cowns since they're not
    // tracked by the GC or in any other GC list. The cown type
    // still defines `tp_traverse` to ensure that this is never
    // accidentally called. Later we may want to simple remove it
    // from the type.
    assert(false);
    return -1;
#else
    Py_VISIT(self->value);
    return 0;
#endif
}

static int PyCown_reachable(_PyCownObject *self, visitproc visit, void *arg) {
    Py_VISIT(Py_TYPE(self));

    // The value is explicitly not visited. Freezing or moving cowns should
    // not propagate to the value.
    // Py_VISIT(self->value);

    return 0;
}

static int PyCown_clear(_PyCownObject *self) {
    cown_set_value_unchecked(self, Py_None);
    Py_CLEAR(self->value);
    return 0;
}

static void PyCown_dealloc(_PyCownObject *self) {
    PyObject_GC_UnTrack(self);
    PyCown_clear(self);
    PyObject_GC_Del(self);
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

    const PyTime_t unset_timeout = _PyTime_FromSeconds(NO_BLOCKING_TIMEOUT);
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

static PyObject *
CownObject_acquire(_PyCownObject *self, PyObject *args, PyObject *kwds)
{
    // Parse the arguments
    PyTime_t timeout;
    if (lock_acquire_parse_args(args, kwds, &timeout) < 0) {
        return NULL;
    }

    // Attempt to lock the cown
    _PyCown_owner_id_t this_owner = _PyCown_ThisOwnerId();
    int res = cown_lock(self, timeout, this_owner, true);
    if (res == COWN_ACQUIRE_ERROR) {
        return NULL;
    }

    // Return the result
    return PyBool_FromLong(res == COWN_ACQUIRE_SUCCESS);
}

PyDoc_STRVAR(CownObject_acquire_doc,
"acquire($self, /, blocking=True, timeout=-1)\n\
Attempts to acquires the cown.\n\
\n\
With default arguments this will block until the cown can be acquired, \n\
even when acquire is called from the same owner. The return indicates \n\
if the cown was was acquired. The blocking operation is interruptable.");

static int cown_release_unchecked(_PyCownObject* self, _PyCown_owner_id_t unlocking_id) {
    // Set owner_id to indicate the released state
    if (!_Py_atomic_compare_exchange_uintptr(&self->owner_id, &unlocking_id, RELEASED_OWNER_ID)) {
        PyErr_Format(
            PyExc_RuntimeError,
            "owner %lld (this) attempted to release a cown owned by someone else\n"
            "Cown: %U",
            unlocking_id, self);
        return -1;
    }

    // Unlocking should always succeed
    int res = _PyMutex_TryUnlock(&self->lock);
    assert(res == 0);
    (void)res;

    return 0;
}

/* Checks that the cown is not released, and that it's owned by the caller. */
static int cown_check_owner_before_release(_PyCownObject *self, _PyCown_owner_id_t unlocking_owner) {
    _PyCown_owner_id_t actual_owner = cown_get_owner(self);
    if (actual_owner == RELEASED_OWNER_ID) {
        PyErr_Format(
            PyExc_RuntimeError,
            "owner %lld attempted to release a released cown",
            unlocking_owner
        );
        return -1;
    }
    if (actual_owner != unlocking_owner) {
        PyErr_Format(
            PyExc_RuntimeError,
            "owner %lld attempted to release a cown owned by %lld",
            unlocking_owner, actual_owner
        );
        return -1;
    }
    return 0;
}

/* This attempts to close the region
 *
 * It returns non-zero if the closing failed
 */
static int cown_close_region(_PyCownObject *self) {
    assert(Region_Check(self->value));

    // Close the region
    int closing_res = _PyTracingRegion_Close(self->value);
    if (closing_res < 0) {
        return -1;
    }

    // Make sure that the cown owns the only external reference to the bridge object.
    if (Py_REFCNT(self->value) > 1) {
        PyErr_Format(
            PyExc_RuntimeError,
            "the cown couldn't be released, due to the bridge having incoming references");
        return -1;
    }

    // FIXME(regions): Test that we can't create weak refs to the bridge object. Otherwise, we also need to clear them.

#ifdef _Py_PYRONA_INTERPRETER_SHARING
    // We need to untrack the region to share it across sub-interpreters
    PyObject_GC_UnTrack(self->value);
#endif

    return 0;
}

static int cown_release(_PyCownObject *self, _PyCown_owner_id_t unlocking_owner) {
    if (cown_check_owner_before_release(self, unlocking_owner) < 0) {
        return -1;
    }

    // Immutable objects are safe to share, the cown can be release directly
    if (_PyImmutability_CanViewAsDeepImmutable(self->value)) {
        return cown_release_unchecked(self, unlocking_owner);
    }
    assert(Region_Check(self->value));

    // The contained region needs to be closed, to allow the cown to release
    if (cown_close_region(self)) {
        return -1;
    }

    // Region is closed, safe to release
    return cown_release_unchecked(self, unlocking_owner);
}

static PyObject* CownObject_release(_PyCownObject *self, PyObject *ignored) {
    _PyCown_owner_id_t owner = _PyCown_ThisOwnerId();
    if (cown_release(self, owner) < 0) {
        return NULL;
    }

    Py_RETURN_NONE;
}

PyDoc_STRVAR(CownObject_release_doc,
"release($self, /)\n\
Release the cown, allowing another owner to acquire the cown\n\
\n\
The cown must be in the locked state and must be unlocked from owner.");

static PyObject *
CownObject_locked(_PyCownObject *op, PyObject *Py_UNUSED(dummy))
{
    return PyBool_FromLong(cown_get_owner(op) != RELEASED_OWNER_ID);
}

PyDoc_STRVAR(CownObject_locked_doc,
"locked($self, /)\n\
--\n\
\n\
Return whether the cown currently released or acquired.  \n\
Use `owned()` to check if the cown is acquired by the current thread.");

static PyObject *
CownObject_owned(_PyCownObject *op, PyObject *Py_UNUSED(dummy))
{
    return PyBool_FromLong(cown_get_owner(op) == _PyCown_ThisOwnerId());
}

PyDoc_STRVAR(CownObject_owned_doc,
"owned($self, /)\n\
--\n\
\n\
Return true if the cown is currently acquired by this thread, false otherwise.");

// FIXME(regions): This should be a function on the Region type.
static PyObject *
CownObject_is_closed(_PyCownObject *self, PyObject *Py_UNUSED(dummy))
{
    if (!Region_Check(self->value)) {
        PyErr_SetString(PyExc_TypeError, "cown value is not a tracing region");
        return NULL;
    }

    return PyBool_FromLong(_PyTracingRegion_IsClosed(self->value));
}

PyDoc_STRVAR(CownObject_is_closed_doc,
"_is_closed($self, /)\n\
--\n\
\n\
Return true if the cown's tracing region value is closed.");


// Define the CownType with methods
static PyMethodDef PyCown_methods[] = {
    {"acquire", _PyCFunction_CAST(CownObject_acquire), METH_VARARGS | METH_KEYWORDS, CownObject_acquire_doc},
    {"release", _PyCFunction_CAST(CownObject_release), METH_NOARGS, CownObject_release_doc},
    {"locked", _PyCFunction_CAST(CownObject_locked), METH_NOARGS, CownObject_locked_doc},
    {"owned", _PyCFunction_CAST(CownObject_owned), METH_NOARGS, CownObject_owned_doc},
    {"_is_closed", _PyCFunction_CAST(CownObject_is_closed), METH_NOARGS, CownObject_is_closed_doc},
    {NULL}  // Sentinel
};

static PyObject *CownObject_get_value(_PyCownObject *self, void *closure) {
    BAIL_UNLESS_OWNED_NULL(self);

    return Py_NewRef(self->value);
}

static int CownObject_set_value(_PyCownObject *self, PyObject *value, void *closure) {
    BAIL_UNLESS_OWNED(self, -1);

    return cown_set_value(self, value);
}

static PyGetSetDef PyCownObject_getset[] = {
    {"value", (getter)CownObject_get_value, (setter)CownObject_set_value,
        "", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyObject *PyCown_repr(_PyCownObject *self) {
    _PyCown_owner_id_t owner = cown_get_owner(self);
    // This thread/interpreter owns the cown
    if (owner == _PyCown_ThisOwnerId()) {
        return PyUnicode_FromFormat(
            "Cown(owner=%llu (this), value=%S)",
            owner,
            PyObject_Repr(self->value)
        );
    }

    // The cown is released and can be acquired
    if (owner == RELEASED_OWNER_ID) {
        return PyUnicode_FromFormat(
            "Cown(owner=None, status=Released)"
        );
    }

    // The cown is owned by a different thread
    return PyUnicode_FromFormat(
        "Cown(owner=%llu (other))",
        owner
    );
}

PyTypeObject _PyCown_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "Cown",
    .tp_basicsize = sizeof(_PyCownObject),
    .tp_dealloc = (destructor)PyCown_dealloc,
    .tp_repr = (reprfunc)PyCown_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .tp_traverse = (traverseproc)PyCown_traverse,
    .tp_reachable = (traverseproc)PyCown_reachable,
    .tp_clear = (inquiry)PyCown_clear,
    .tp_methods = PyCown_methods,
    .tp_getset = PyCownObject_getset,
    .tp_init = (initproc)PyCown_init,
    .tp_new = PyType_GenericNew,
};

