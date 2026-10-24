#include "Python.h"
#include "pymacro.h"

#include "pycore_ceval.h"         // _PyEval_AddPendingCall()
#include "pycore_mutexobject.h"
#include "pycore_immutability.h"
#include "pycore_interp.h"        // _PyInterpreterState_LookUpID()
#include "pycore_lock.h"
#include "pycore_time.h"          // _PyTime_FromSeconds()

/* Macro that jumps to error, if the expression `x` does not succeed. */
#define SUCCEEDS(x) { do { int r = (x); if (r != 0) goto error; } while (0); }

#define Region_Check(x) Py_IS_TYPE((x), &_PyTracingRegion_Type)

#define RELEASED_OWNER_ID _Py_PYRONA_RELEASED_OWNER_ID
#define NO_BLOCKING_TIMEOUT -1

typedef enum MutexLockStatus {
    MUTEX_ACQUIRE_ERROR = -1,
    MUTEX_ACQUIRE_FAIL = 0,
    MUTEX_ACQUIRE_SUCCESS = 1
} MutexLockStatus;

struct _PyMutexObject {
    PyObject_HEAD

    /* The ID of the owner.
     *
     * On NoGIL this is the thread ID, on GIL-enabled Python this is the
     * sub-interpreter ID.
     */
    _PyPyrona_owner_id_t owner_id;

    /* The value stored in the mutex. This value may be immutable, another mutex
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

static _PyPyrona_owner_id_t mutex_get_owner(_PyMutexObject *obj) {
    return _Py_atomic_load_uintptr_relaxed(&obj->owner_id);
}

#define BAIL_UNLESS_OWNED_BY(o, tested_owner, result) \
    do {\
        _PyPyrona_owner_id_t owning_id = mutex_get_owner(_PyMutexObject_CAST(o)); \
        if (owning_id != tested_owner) { \
            PyErr_Format( \
                PyExc_RuntimeError, \
                "attempted to access a mutex owned by %llu from %llu", \
                owning_id, tested_owner); \
            return result; \
        } \
    } while (0);
#define BAIL_UNLESS_OWNED(o, result) BAIL_UNLESS_OWNED_BY(o, _PyPyrona_ThisOwnerId(), result)
#define BAIL_UNLESS_OWNED_NULL(o) BAIL_UNLESS_OWNED(o, NULL)

static int mutex_set_value_unchecked(_PyMutexObject* self, PyObject* value) {
    // Storing a value requires ownership. The exception is the teardown of a
    // released mutex, which nobody owns and only its last reference can reach.
    assert(mutex_get_owner(self) == RELEASED_OWNER_ID
           || mutex_get_owner(self) == _PyPyrona_ThisOwnerId());

    // This doesn't require a lock since only the owning thread can read and
    // write to self->value
    Py_XSETREF(self->value, Py_NewRef(value));

    return 0;
}

static int mutex_set_value(_PyMutexObject* self, PyObject* value) {
    BAIL_UNLESS_OWNED(self, -1);

    // Bridge objects are allowed
    if (Region_Check(value)) {
        return mutex_set_value_unchecked(self, value);
    }

    // Immutable objects are allowed
    int immutable = _PyImmutability_CanViewAsDeepImmutable(value);
    if (immutable < 0) {
        return -1;
    }
    if (immutable) {
        return mutex_set_value_unchecked(self, value);
    }

    // Local objects are forbidden
    PyErr_Format(
        PyExc_RuntimeError,
        "attempted to store a local mutable object in a mutex.\n"
        "Only regions, mutex, and immutable objects are allowed");

    return -1;
}

/* Attempt to lock the mutex.
 *
 * Timeout values:
 * (-1) => Non-blocking locking
 *  (0) => Block with no timeout
 *  (n) => Blocking with timeout
 */
static int mutex_lock(_PyMutexObject* self, PyTime_t timeout, _PyPyrona_owner_id_t owner_id, bool has_gil) {
    // A blocking time should only be set, if this call holds the GIL
    assert(has_gil || timeout == NO_BLOCKING_TIMEOUT);

    // Try to lock the mutex directly, without releasing the GIL first
    PyLockStatus r = _PyMutex_LockTimed(&self->lock, 0, _Py_LOCK_DONT_DETACH);

    // The mutex is currently owned by something else. Release the GIL and
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
        return MUTEX_ACQUIRE_ERROR;
    }

    // The lock acquisition failed
    if (r == PY_LOCK_FAILURE) {
        return MUTEX_ACQUIRE_FAIL;
    }

    // Set the owner_ip to this thread/interpreter, thereby taking ownership
    _PyPyrona_owner_id_t released_value = RELEASED_OWNER_ID;
    if (!_Py_atomic_compare_exchange_uintptr(
        &self->owner_id,
        &released_value,
        owner_id)
    ) {
        // Failed to set owning_ip, this should never happen and points
        // to a deeper issue.
        PyErr_Format(
            PyExc_RuntimeError,
            "[BUG] failed to set owner on a locked mutex\n"
            "Mutex: %U",
            self
        );

        _PyMutex_Unlock(&self->lock);
        return MUTEX_ACQUIRE_ERROR;
    }

    // Attach the region to the current concurrent unit
    if (self->value && Region_Check(self->value)) {
        if (_PyTracingRegion_Attach(self->value, owner_id)) {
            return MUTEX_ACQUIRE_ERROR;
        }
    }

    return MUTEX_ACQUIRE_SUCCESS;
}

_PyPyrona_owner_id_t _PyPyrona_ThisOwnerId(void) {
#ifdef _Py_PYRONA_INTERPRETER_SHARING
    _PyPyrona_owner_id_t ip = PyInterpreterState_GetID(PyInterpreterState_Get());
    return ip;
#else
    return _Py_ThreadId();
#endif
}

// All setup happens here instead of `tp_init`: a shared mutex must not be
// re-initialized by whoever calls `__init__` on it.
static PyObject *PyMutex_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    // See if we got a value as a keyword argument
    static char *kwlist[] = {"value", NULL};
    PyObject *value = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &value)) {
        return NULL;
    }

    _PyMutexObject *self = _PyMutexObject_CAST(type->tp_alloc(type, 0));
    if (self == NULL) {
        return NULL;
    }
    self->value = Py_NewRef(Py_None);

    // Init the mutex as being acquired by this owner
    _PyPyrona_owner_id_t this_owner = _PyPyrona_ThisOwnerId();
    _Py_atomic_store_uintptr_relaxed(&self->owner_id, RELEASED_OWNER_ID);
    if (mutex_lock(self, NO_BLOCKING_TIMEOUT, this_owner, true) != MUTEX_ACQUIRE_SUCCESS) {
        PyErr_Format(
            PyExc_RuntimeError,
            "Newly created mutex couldn't be acquired by this owner %lld",
            this_owner);
        goto error;
    }

    // Set the mutex value using the internal function for full validation
    SUCCEEDS(mutex_set_value(self, value));

    // For sub-interpreters, we need to enable atomic RC and untrack the object
#ifdef _Py_PYRONA_INTERPRETER_SHARING
    _Py_EnableAtomicRC(self);
    PyObject_GC_UnTrack(self);
#endif

    return _PyObject_CAST(self);
error:
    Py_DECREF(self);
    return NULL;
}

static int PyMutex_traverse(PyObject *op, visitproc visit, void *arg) {
    _PyMutexObject *self = _PyMutexObject_CAST(op);
#ifdef _Py_PYRONA_INTERPRETER_SHARING
    // tp_traverse should never be called on mutexes since they're not
    // tracked by the GC or in any other GC list. The mutex type
    // still defines `tp_traverse` to ensure that this is never
    // accidentally called. Later we may want to simple remove it
    // from the type.
    assert(false);
    (void)visit;
    (void)arg;
    (void)self;
    return -1;
#else
    Py_VISIT(self->value);
    return 0;
#endif
}

static int PyMutex_reachable(PyObject *op, reachablevisitproc visit, void *arg) {
    _PyMutexObject *self = _PyMutexObject_CAST(op);
    Py_VISIT_REACHABLE(Py_TYPE(self), Py_REACHABLE_STRONG);

    // The value is explicitly not visited. Freezing or moving mutexes should
    // not propagate to the value.
    // Py_VISIT_REACHABLE(self->value, Py_REACHABLE_STRONG);

    return 0;
}

static int PyMutex_clear(PyObject *op) {
    _PyMutexObject *self = _PyMutexObject_CAST(op);
    // Regions are the only mutable values, see `mutex_set_value`. Checking that
    // instead of immutability can't fail, which `tp_clear` has no way to report.
    if (self->value != NULL && !Region_Check(self->value)) {
        Py_CLEAR(self->value);
        return 0;
    }

    // A mutable value is a region, which may only be dropped by the interpreter
    // owning this mutex. `PyMutex_dealloc` makes sure that this runs there.
    mutex_set_value_unchecked(self, Py_None);
    return 0;
}

/* Tears the mutex down. Only the thead/interpreter owning the mutex may run this,
 * see `mutex_handoff_dealloc`. */
static void mutex_dealloc_owned(_PyMutexObject *self) {
    if (mutex_get_owner(self) == RELEASED_OWNER_ID) {
        _PyPyrona_owner_id_t this_ip = _PyPyrona_ThisOwnerId();
        // This should never fail, since we have the last remaining instance
        int res = mutex_lock(self, -1, this_ip, true);
        assert(res >= 0);
    }

    // Clearing hands the region off, so no region reference points here any more.
    PyMutex_clear(_PyObject_CAST(self));
    PyObject_GC_Del(self);
}

#ifdef _Py_PYRONA_INTERPRETER_SHARING
static int mutex_pending_dealloc(void *arg) {
    mutex_dealloc_owned((_PyMutexObject *)arg);
    return 0;
}

/* Hands the teardown to the interpreter owning the mutex and returns true, or
 * returns false when the caller should tear the mutex down itself.
 *
 * A mutex is immutable, so the last reference to it can be dropped by an
 * interpreter that never owned it. Its region can not be dropped there: while
 * the mutex is acquired, the region is reference counted non-atomically and
 * tracked in the owner's GC list, so touching it would race with the owner.
 *
 * The mutex itself is handed over as well, instead of only its region, because a
 * region reference resolving through this mutex borrows the pointer. Freeing the
 * mutex here would leave that pointer dangling until the scheduled call runs.
 *
 * A mutex waiting for its owner is unreachable: its reference count is zero, it
 * supports no weak references, and `_Py_TryIncref_Immutable` refuses to
 * resurrect it. It must never be revived either, since a revived mutex could be
 * released a second time, with its region already gone.
 */
static bool mutex_handoff_dealloc(_PyMutexObject *self) {
    _PyPyrona_owner_id_t owner = mutex_get_owner(self);
    // Nobody owns a released mutex, which makes the caller the only one that can
    // reach the region.
    if (owner == RELEASED_OWNER_ID || owner == _PyPyrona_ThisOwnerId()) {
        return false;
    }

    // The lookup raises when the interpreter is gone, and a deallocation can
    // happen mid-raise.
    PyObject *exc = PyErr_GetRaisedException();
    // FIXME(regions): Can the interpreter go away in the middle of scheduling?
    // `weakref_schedule_callbacks` in `Python/immutability.c` asks the same.
    PyInterpreterState *target = _PyInterpreterState_LookUpID((int64_t)owner);
    bool scheduled = target != NULL
        && _PyEval_AddPendingCall(target, mutex_pending_dealloc, self, 0)
               == _Py_ADD_PENDING_SUCCESS;
    PyErr_SetRaisedException(exc);
    if (scheduled) {
        return true;
    }

    // The owner is gone, or its call queue is full. Tearing the mutex down here
    // is all that is left to do, so the region ends up owned by nobody. An
    // interpreter that is already gone can at least not race with us.
    _Py_atomic_store_uintptr_relaxed(&self->owner_id, RELEASED_OWNER_ID);
    return false;
}
#endif

static void PyMutex_dealloc(PyObject *op) {
    _PyMutexObject *self = _PyMutexObject_CAST(op);
    // Reaching zero returned the mutex to this interpreter's GC list. Nothing may
    // traverse it, `PyMutex_traverse` asserts as much.
    PyObject_GC_UnTrack(self);

#ifdef _Py_PYRONA_INTERPRETER_SHARING
    if (mutex_handoff_dealloc(self)) {
        return;
    }
#endif
    mutex_dealloc_owned(self);
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
MutexObject_acquire(PyObject *op, PyObject *args, PyObject *kwds)
{
    _PyMutexObject *self = _PyMutexObject_CAST(op);
    // Parse the arguments
    PyTime_t timeout;
    if (lock_acquire_parse_args(args, kwds, &timeout) < 0) {
        return NULL;
    }

    // Attempt to lock the mutex
    _PyPyrona_owner_id_t this_owner = _PyPyrona_ThisOwnerId();
    int res = mutex_lock(self, timeout, this_owner, true);
    if (res == MUTEX_ACQUIRE_ERROR) {
        return NULL;
    }

    // Return the result
    return PyBool_FromLong(res == MUTEX_ACQUIRE_SUCCESS);
}

PyDoc_STRVAR(MutexObject_acquire_doc,
"acquire($self, /, blocking=True, timeout=-1)\n\
Attempts to acquires the mutex.\n\
\n\
With default arguments this will block until the mutex can be acquired, \n\
even when acquire is called from the same owner. The return indicates \n\
if the mutex was was acquired. The blocking operation is interruptable.");

static int mutex_release_unchecked(_PyMutexObject* self, _PyPyrona_owner_id_t unlocking_id) {
    // Set owner_id to indicate the released state
    if (!_Py_atomic_compare_exchange_uintptr(&self->owner_id, &unlocking_id, RELEASED_OWNER_ID)) {
        PyErr_Format(
            PyExc_RuntimeError,
            "owner %lld (this) attempted to release a mutex owned by someone else\n"
            "Mutex: %U",
            unlocking_id, self);
        return -1;
    }

    // Unlocking should always succeed
    int res = _PyMutex_TryUnlock(&self->lock);
    assert(res == 0);
    (void)res;

    return 0;
}

/* Checks that the mutex is not released, and that it's owned by the caller. */
static int mutex_check_owner_before_release(_PyMutexObject *self, _PyPyrona_owner_id_t unlocking_owner) {
    _PyPyrona_owner_id_t actual_owner = mutex_get_owner(self);
    if (actual_owner == RELEASED_OWNER_ID) {
        PyErr_Format(
            PyExc_RuntimeError,
            "owner %lld attempted to release a released mutex",
            unlocking_owner
        );
        return -1;
    }
    if (actual_owner != unlocking_owner) {
        PyErr_Format(
            PyExc_RuntimeError,
            "owner %lld attempted to release a mutex owned by %lld",
            unlocking_owner, actual_owner
        );
        return -1;
    }
    return 0;
}

static int mutex_release(_PyMutexObject *self, _PyPyrona_owner_id_t unlocking_owner) {
    if (mutex_check_owner_before_release(self, unlocking_owner) < 0) {
        return -1;
    }

    // Immutable objects are safe to share, the mutex can be release directly
    int immutable = _PyImmutability_CanViewAsDeepImmutable(self->value);
    if (immutable < 0) {
        return -1;
    }
    if (immutable) {
        return mutex_release_unchecked(self, unlocking_owner);
    }
    assert(Region_Check(self->value));

    // The contained region needs to be closed, to allow the mutex to release.
    if (_PyTracingRegion_Detach(self->value)) {
        return -1;
    }

    // Region is closed, safe to release
    return mutex_release_unchecked(self, unlocking_owner);
}

static PyObject* MutexObject_release(PyObject *op, PyObject *ignored) {
    _PyMutexObject *self = _PyMutexObject_CAST(op);
    _PyPyrona_owner_id_t owner = _PyPyrona_ThisOwnerId();
    if (mutex_release(self, owner) < 0) {
        return NULL;
    }

    Py_RETURN_NONE;
}

PyDoc_STRVAR(MutexObject_release_doc,
"release($self, /)\n\
Release the mutex, allowing another owner to acquire the mutex\n\
\n\
The mutex must be in the locked state and must be unlocked from owner.");

static PyObject *
MutexObject_locked(PyObject *op, PyObject *Py_UNUSED(dummy))
{
    return PyBool_FromLong(mutex_get_owner(_PyMutexObject_CAST(op)) != RELEASED_OWNER_ID);
}

PyDoc_STRVAR(MutexObject_locked_doc,
"locked($self, /)\n\
--\n\
\n\
Return whether the mutex currently released or acquired.  \n\
Use `owned()` to check if the mutex is acquired by the current thread.");

static PyObject *
MutexObject_owned(PyObject *op, PyObject *Py_UNUSED(dummy))
{
    return PyBool_FromLong(mutex_get_owner(_PyMutexObject_CAST(op)) == _PyPyrona_ThisOwnerId());
}

PyDoc_STRVAR(MutexObject_owned_doc,
"owned($self, /)\n\
--\n\
\n\
Return true if the mutex is currently acquired by this thread, false otherwise.");

// Define the MutexType with methods
static PyMethodDef PyMutex_methods[] = {
    {"acquire", _PyCFunction_CAST(MutexObject_acquire), METH_VARARGS | METH_KEYWORDS, MutexObject_acquire_doc},
    {"release", MutexObject_release, METH_NOARGS, MutexObject_release_doc},
    {"locked", MutexObject_locked, METH_NOARGS, MutexObject_locked_doc},
    {"owned", MutexObject_owned, METH_NOARGS, MutexObject_owned_doc},
    {NULL}  // Sentinel
};

static PyObject *MutexObject_get_value(PyObject *op, void *closure) {
    _PyMutexObject *self = _PyMutexObject_CAST(op);
    BAIL_UNLESS_OWNED_NULL(self);

    return Py_NewRef(self->value);
}

static int MutexObject_set_value(PyObject *op, PyObject *value, void *closure) {
    _PyMutexObject *self = _PyMutexObject_CAST(op);
    BAIL_UNLESS_OWNED(self, -1);

    return mutex_set_value(self, value);
}

static PyGetSetDef PyMutexObject_getset[] = {
    {"value", MutexObject_get_value, MutexObject_set_value,
        "", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyObject *PyMutex_repr(PyObject *op) {
    _PyMutexObject *self = _PyMutexObject_CAST(op);
    _PyPyrona_owner_id_t owner = mutex_get_owner(self);
    // This thread/interpreter owns the mutex
    if (owner == _PyPyrona_ThisOwnerId()) {
        return PyUnicode_FromFormat(
            "Mutex(owner=%llu (this), value=%R)",
            owner,
            self->value
        );
    }

    // The mutex is released and can be acquired
    if (owner == RELEASED_OWNER_ID) {
        return PyUnicode_FromFormat(
            "Mutex(owner=None, status=Released)"
        );
    }

    // The mutex is owned by a different thread
    return PyUnicode_FromFormat(
        "Mutex(owner=%llu (other))",
        owner
    );
}

PyTypeObject _PyMutex_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "Mutex",
    .tp_basicsize = sizeof(_PyMutexObject),
    .tp_dealloc = PyMutex_dealloc,
    .tp_repr = PyMutex_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .tp_traverse = PyMutex_traverse,
    .tp_reachable = PyMutex_reachable,
    .tp_clear = PyMutex_clear,
    .tp_methods = PyMutex_methods,
    .tp_getset = PyMutexObject_getset,
    .tp_new = PyMutex_new,
};

