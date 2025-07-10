#ifndef Py_BUILD_CORE_BUILTIN
#define Py_BUILD_CORE_MODULE 1
#endif

#include "Python.h"

#include "pycore_crossinterp.h"
#include "pycore_interp.h"
#include "pycore_llist.h"
#include "pycore_lock.h"
#include "pycore_pystate.h"

#include "_interpreters_common.h"
#include "mimalloc/atomic.h"  // mi_atomic_yield()
#include "posixmodule.h"
#include "pyatomic.h"
// FIXME: Is this the best/correct approach to wait in a spinlock?
#define SPIN_WAIT mi_atomic_yield

#include <stdlib.h>  // qsort

#define MODULE_NAME       _boc
#define MODULE_NAME_STR   Py_STRINGIFY(MODULE_NAME)
#define MODINIT_FUNC_NAME RESOLVE_MODINIT_FUNC_NAME(MODULE_NAME)

typedef struct _shared_cown_data shared_cown_t;
typedef struct _request request_t;
typedef struct _behaviour behaviour_t;

// Flags to be passed to the lock method of the PyMutex.
#define MUTEX_FLAGS _Py_LOCK_DONT_DETACH

static struct PyModuleDef moduledef;

/******************************************************************************/
/* The module state including exception types. */

typedef struct {
    PyTypeObject *Cown_type;
    PyObject *BOCError;  // Boc related exception.
} mod_state;

static inline mod_state *
get_state(PyObject *m)
{
    return (mod_state *)PyModule_GetState(m);
}

/******************************************************************************/
/* The interpreter pool implementation                                        */

// A spawned task on the interpreter pool is a function taking a pointer to
// some arbitrary data.
typedef void (*task_t)(void *);

// Pending tasks are stored in a queue. That queue is made up by a linked
// list defined in "pycore_llist.h". This struct is one node of that queue/list.
typedef struct {
    task_t task;
    void *data;
    struct llist_node node;
} task_queue_item;

// This struct should never be copied or moved because it contains a `PyMutex`
// in one of its fields.
typedef struct {
    // The maximum number of worker threads. At most be 64.
    uint32_t n_max_interps;

    // The number of initialised interpreters. At most `n_max_interps`.
    uint32_t n_initialised_interps;

    // A list of interpreter IDs.
    int64_t *interpreters;

    // A bit set where the ith least significant bit is 1 iff the ith
    // interpreter is idle.
    uint64_t idle_interps;

    // We store a queue of pending tasks if all workers are busy. The queue is
    // implemented by a circular doubly linked list ("pycore_llist.h") with one
    // centinal head-node. This is that head-node. tasks should be enqueued at
    // `head->prev` and dequeued at `head->next`.
    struct llist_node task_queue_head;

    // -----------------------------------------------------
    // These fields are not protected by any mutex lock.

    // It should be possible to shut down the interpreter pool when all tasks
    // are finished. In other words, we need a shut down procedure that gets
    // notified when there are no running tasks and then blocks new tasks from
    // being spawned. We solve this by having a mutex that is locked as long as
    // any task is running. The shut down procedure could wait until this lock
    // is unlocked and then begin the shut down.
    PyMutex shutdown_lock;

    // We keep track of the number of spawned tasks. The `shutdown_lock` should
    // only be released when this number reaches 0. Before the shut down
    // procedure acquires the lock, it should set this field to `INT64_MIN`
    // before starting the shutdown procedure. This signals to the runtime that
    // no new tasks can be spawned. This field must be accessed with atomic
    // operations.
    int64_t n_spawned_tasks;
} interpreter_pool_t;

// A global reference to THE interpreter pool, guarded by a mutex.
static interpreter_pool_t *interpreter_pool = NULL;

// This mutex is locked whenever we make changes to `interpreter_pool`.
static PyMutex interpreter_pool_lock = (PyMutex){ _Py_UNLOCKED };

// Initialise `interpreter_pool` if it is NULL and lock `interpreter_pool_lock`.
//
// If this function returns 0, it guarantees the following:
// - `interpreter_pool_lock` is locked.
// - `interpreter_pool` is initialised
static int
interpreter_pool_init(void)
{
    PyMutex_LockFlags(&interpreter_pool_lock, MUTEX_FLAGS);

    if (interpreter_pool != NULL) {
        return 0;
    }

    // Initialise `interpreter_pool`.

    int n_threads = _cpu_count();
    if (n_threads < 1) {
        n_threads = 1;
    }
    if (n_threads > 64) {
        n_threads = 64;
    }

    int64_t *interpreters =
        (int64_t *)PyMem_RawMalloc(sizeof(int64_t) * n_threads);
    if (interpreters == NULL) {
        PyErr_NoMemory();
        return -1;
    }

    interpreter_pool = PyMem_RawMalloc(sizeof *interpreter_pool);

    if (interpreter_pool == NULL) {
        PyMem_RawFree(interpreters);
        PyErr_NoMemory();
        return -1;
    }

    interpreter_pool->n_max_interps = n_threads;
    interpreter_pool->n_initialised_interps = 0;
    interpreter_pool->interpreters = interpreters;
    interpreter_pool->idle_interps = 0;
    llist_init(&interpreter_pool->task_queue_head);
    interpreter_pool->shutdown_lock = (PyMutex){ _Py_UNLOCKED };
    interpreter_pool->n_spawned_tasks = 0;

    return 0;
}

// Bump the number of spawned tasks. Returns -1 if the interpreter pool is
// shutting down, otherwise returns 0.
static int
inc_spawned_tasks(interpreter_pool_t *interpreter_pool)
{
    int64_t prev_n =
        _Py_atomic_add_int64(&interpreter_pool->n_spawned_tasks, 1);
    if (prev_n == 0) {
        // Lock the shutdown lock. It should not block.
        PyMutex_Lock(&interpreter_pool->shutdown_lock);
    }
    else if (prev_n < 0) {
        // We are currently shutting down.
        return -1;
    }

    return 0;
}

// Decrement the number of spawned tasks. Must be paired with a call to
// `inc_spawned_tasks()`.
static void
dec_spawned_tasks(interpreter_pool_t *interpreter_pool)
{
    int64_t prev_n =
        _Py_atomic_add_int64(&interpreter_pool->n_spawned_tasks, -1);
    assert(prev_n > 0);
    if (prev_n == 1) {
        // There are no spawned tasks anymore.
        PyMutex_Unlock(&interpreter_pool->shutdown_lock);
    }
}

static void
interpreter_pool_shutdown(void)
{
    // Block until all tasks are finished.
    while (true) {
        PyMutex_Lock(&interpreter_pool->shutdown_lock);
        int64_t expected = 0;
        if (_Py_atomic_compare_exchange_int64(
                &interpreter_pool->n_spawned_tasks, &expected, INT64_MIN)) {
            break;
        }
        PyMutex_Unlock(&interpreter_pool->shutdown_lock);
    }
    PyMutex_Unlock(&interpreter_pool->shutdown_lock);

    // Destroy all interpreters.
    for (uint32_t i = 0; i < interpreter_pool->n_initialised_interps; i++) {
        PyInterpreterState *interp =
            _PyInterpreterState_LookUpID(interpreter_pool->interpreters[i]);
        _PyXI_EndInterpreter(interp, NULL, NULL);
    }

    assert(llist_empty(&interpreter_pool->task_queue_head));

    // Free memory.
    PyMem_RawFree(interpreter_pool->interpreters);

    PyMem_RawFree(interpreter_pool);
}

// Clear the least significant set bit and return its index. Returns -1 if
// `mask` is 0.
static int
bit_set_pop(uint64_t *mask)
{
    uint64_t m = *mask;

    if (m == 0) {
        return -1;
    }

    int res = __builtin_ctzll(m);  // Count trailing zeros.

    *mask = m & (m - 1);

    return res;
}

typedef struct {
    task_t task;
    void *data;

    // The interpreter where the task is spawned.
    PyInterpreterState *interp;
    // The index of the interpreter as used in `interpreter_pool_t`. Not the
    // same as a normal interpreter ID.
    int interp_index;
} worker_thread_args;

static void
run_worker_thread(void *args_)
{
    worker_thread_args *args = args_;

    // Create a thread in the subinterpreter.
    PyThreadState *tstate = PyThreadState_New(args->interp);
    if (tstate == NULL) {
        fprintf(stderr,
                "Error: BOC: Creating a thread in spawned subinterpreter "
                "failed");
        PyMem_RawFree(args);
        // TODO: How do we handle this situation: A thread in this
        // sub-interpreter could not be created. This likely means that Python
        // is out of memory. However the main interpreter might blocked on the
        // interpreter pool to shutdown, and it won't shutdown unless all
        // spawned tasks are finished, so it may deadlock.
        return;
    }

    task_t task = args->task;
    void *data = args->data;
    while (true) {
        // Switch to the new thread on the subinterpreter.
        PyEval_RestoreThread(tstate);

        // Run the task.
        task(data);

        dec_spawned_tasks(interpreter_pool);

        // Release the GIL for this interpreter while checking if there are
        // more jobs. This is not strictly necessary but is probably good
        // practice.
        PyEval_SaveThread();

        PyMutex_LockFlags(&interpreter_pool_lock, MUTEX_FLAGS);
        // Check if there are pending tasks.
        if (llist_empty(&interpreter_pool->task_queue_head)) {
            // There are no pending tasks so mark this interpreter as idle.
            interpreter_pool->idle_interps |= (uint64_t)1
                                              << args->interp_index;
            _PyMutex_Unlock(&interpreter_pool_lock);
            break;
        }

        // Pop a pending task of the task queue.
        struct llist_node *node = interpreter_pool->task_queue_head.next;
        task_queue_item *item = llist_data(node, task_queue_item, node);
        llist_remove(node);
        task = item->task;
        data = item->data;
        PyMem_RawFree(item);
        _PyMutex_Unlock(&interpreter_pool_lock);
    }

    PyMem_RawFree(args);

    PyEval_RestoreThread(tstate);
    PyThreadState_Clear(tstate);
    PyThreadState_DeleteCurrent();
}

// Spawn a task on the interpreter pool.
static int
interpreter_pool_spawn(task_t task, void *data)
{
    if (interpreter_pool_init() < 0) {
        return -1;
    }

    if (inc_spawned_tasks(interpreter_pool) < 0) {
        PyErr_SetString(PyExc_RuntimeError,
                        "BOC: Failed to spawn task while interpreter pool is "
                        "shutting down");
        PyMutex_Unlock(&interpreter_pool_lock);
        return -1;
    }

    // Get the index of an idle interpreter.
    int idle_interpreter;

    // Check if there is some idle interpreter.
    idle_interpreter = bit_set_pop(&interpreter_pool->idle_interps);

    if (idle_interpreter < 0) {
        // There were no idle interpreter so let's check if we should spawn a
        // new interpreter.
        if (interpreter_pool->n_initialised_interps
            < interpreter_pool->n_max_interps) {
            // Initialise a new interpreter.
            PyInterpreterConfig config = (PyInterpreterConfig){
                .use_main_obmalloc = 0,
                .allow_fork = 0,
                .allow_exec = 0,
                .allow_threads = 0,
                .allow_daemon_threads = 0,
                .check_multi_interp_extensions = 1,
                .gil = PyInterpreterConfig_OWN_GIL,
            };

            // FIXME: Is this correct?
            long whence = _PyInterpreterState_WHENCE_STDLIB;
            PyInterpreterState *interp =
                _PyXI_NewInterpreter(&config, &whence, NULL, NULL);
            if (interp == NULL) {
                goto error;
            }

            assert(_PyInterpreterState_IsReady(interp));

            idle_interpreter = interpreter_pool->n_initialised_interps;

            int interp_id = PyInterpreterState_GetID(interp);
            if (interp_id < 0) {
                _PyXI_EndInterpreter(interp, NULL, NULL);
                PyErr_SetString(
                    PyExc_InterpreterError,
                    "failed to initialise interpreter: no interpreter ID");
                goto error;
            }

            interpreter_pool->interpreters[idle_interpreter] = interp_id;
            interpreter_pool->n_initialised_interps++;
        }
        else {
            // All interpreters are busy so we add the task to the queue of
            // pending tasks.

            task_queue_item *item = PyMem_RawMalloc(sizeof *item);
            if (item == NULL) {
                PyErr_NoMemory();
                goto error;
            }

            item->task = task;
            item->data = data;
            llist_insert_tail(&interpreter_pool->task_queue_head, &item->node);

            PyMutex_Unlock(&interpreter_pool_lock);
            return 0;
        }
    }

    // Lookup the `PyInterpreterState` of `idle_interpreter`.
    PyInterpreterState *interp = _PyInterpreterState_LookUpID(
        interpreter_pool->interpreters[idle_interpreter]);

    worker_thread_args *args = PyMem_RawMalloc(sizeof *args);
    if (args == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    *args = (worker_thread_args){ .task = task,
                                  .data = data,
                                  .interp = interp,
                                  .interp_index = idle_interpreter };

    if (PyThread_start_new_thread(run_worker_thread, args)
        == PYTHREAD_INVALID_THREAD_ID) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to start a new thread");
        goto error;
    }

    PyMutex_Unlock(&interpreter_pool_lock);
    return 0;

error:
    PyMutex_Unlock(&interpreter_pool_lock);
    dec_spawned_tasks(interpreter_pool);
    return -1;
}

/******************************************************************************/
/* Code to spawn an ordinary python function on the interpreter pool without
 * using a when block.
 * FIXME: Should not be needed in the final version.                          */

struct interp_call {
    _PyXIData_t *func;
    _PyXIData_t *args;
    struct {
        _PyXIData_t func;
        _PyXIData_t args;
    } _preallocated;
};

static void
_interp_call_clear(struct interp_call *call)
{
    if (call->func != NULL) {
        _PyXIData_Release(call->func);
    }
    if (call->args != NULL) {
        _PyXIData_Release(call->args);
    }
    *call = (struct interp_call){ 0 };
}

static int
_interp_call_pack(PyThreadState *tstate,
                  struct interp_call *call,
                  PyObject *func,
                  PyObject *args)
{
    xidata_fallback_t fallback = _PyXIDATA_FULL_FALLBACK;
    assert(call->func == NULL);
    assert(call->args == NULL);
    // Handle the func.
    if (!PyCallable_Check(func)) {
        _PyErr_Format(tstate, PyExc_TypeError, "expected a callable, got %R",
                      func);
        return -1;
    }
    if (_PyFunction_GetXIData(tstate, func, &call->_preallocated.func) < 0) {
        PyObject *exc = _PyErr_GetRaisedException(tstate);
        if (_PyPickle_GetXIData(tstate, func, &call->_preallocated.func) < 0) {
            _PyErr_SetRaisedException(tstate, exc);
            return -1;
        }
        Py_DECREF(exc);
    }
    call->func = &call->_preallocated.func;
    // Handle the args.
    if (args == NULL || args == Py_None) {
        // Leave it empty.
    }
    else {
        assert(PyTuple_Check(args));
        if (PyTuple_GET_SIZE(args) > 0) {
            if (_PyObject_GetXIData(tstate, args, fallback,
                                    &call->_preallocated.args)
                < 0) {
                _interp_call_clear(call);
                return -1;
            }
            call->args = &call->_preallocated.args;
        }
    }
    return 0;
}

static int
_interp_call_unpack(struct interp_call *call,
                    PyObject **p_func,
                    PyObject **p_args)
{
    // Unpack the func.
    PyObject *func = _PyXIData_NewObject(call->func);
    if (func == NULL) {
        return -1;
    }
    // Unpack the args.
    PyObject *args;
    if (call->args == NULL) {
        args = PyTuple_New(0);
        if (args == NULL) {
            Py_DECREF(func);
            return -1;
        }
    }
    else {
        args = _PyXIData_NewObject(call->args);
        if (args == NULL) {
            Py_DECREF(func);
            return -1;
        }
        assert(PyTuple_Check(args));
    }
    *p_func = func;
    *p_args = args;
    return 0;
}

static inline int
is_notshareable_raised(PyThreadState *tstate)
{
    PyObject *exctype = _PyXIData_GetNotShareableErrorType(tstate);
    return _PyErr_ExceptionMatches(tstate, exctype);
}

// Portable function to count the number of ones.
static inline uint32_t
count_ones(uint64_t x)
{
    uint32_t c = 0;
    // Kernighan’s method
    while (x) {
        x &= x - 1;
        c++;
    }
    return c;
}

static PyObject *
interpreter_pool_info(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    // Begin by locking the interpreter pool.
    if (interpreter_pool_init() < 0) {
        return NULL;
    }

    uint32_t n_live = interpreter_pool->n_initialised_interps;
    uint32_t n_idle = count_ones(interpreter_pool->idle_interps);
    uint32_t n_max_interpreters = interpreter_pool->n_max_interps;

    // Unlock the interpreter pool before starting to allocate things.
    PyMutex_Unlock(&interpreter_pool_lock);

    PyObject *info_dict = PyDict_New();
    if (info_dict == NULL) {
        return NULL;
    }

    PyObject *n_live_obj;
    PyObject *n_idle_obj;
    PyObject *n_max_interpreters_obj;
    PyObject *result = NULL;

    if ((n_live_obj = PyLong_FromUInt32(n_live)) == NULL) {
        goto done;
    }
    if ((n_idle_obj = PyLong_FromUInt32(n_idle)) == NULL) {
        goto done;
    }
    if ((n_max_interpreters_obj = PyLong_FromUInt32(n_max_interpreters))
        == NULL) {
        goto done;
    }

    if (PyDict_SetItemString(info_dict, "n_live", n_live_obj) < 0
        || PyDict_SetItemString(info_dict, "n_idle", n_idle_obj) < 0
        || PyDict_SetItemString(info_dict, "n_max_interpreters",
                                n_max_interpreters_obj)
               < 0) {
        goto done;
    }

    result = info_dict;
    Py_INCREF(result);

done:
    Py_DECREF(info_dict);
    Py_XDECREF(n_live_obj);
    Py_XDECREF(n_idle_obj);
    Py_XDECREF(n_max_interpreters_obj);

    return result;
}

/******************************************************************************/
/* The cown type                                                              */

// shared_cown_t
//
// This struct contains the value for a cown, shared among interpreters. When an
// interpreter acquires a cown, a python object (`Cown`) is created refering to
// the shared cown. Since at most one when block may access the cown at the same
// time, it is safe to modify the value of the cown inside a when block.
//
// This type is reference counted.
struct _shared_cown_data {
    int64_t refcount;

    // An unique ID for this cown. Used to order cowns during 2PL.
    //
    // This ID should be retreived from the globally incremented id
    // `NEXT_COWN_ID`.
    uint64_t id;

    // The actual data that is shared across interpreters. This might be
    // changed non atomically as the boc implementation ensures that only one
    // interpreter has access to the data at the same time.
    //
    // This is owned by this struct and has to be freed accordingly.
    _PyXIData_t *val;

    // The last request enqueued onto this cown. The request itself may point to
    // earlier requests that have to be resolved first.
    //
    // This is borrowed by this struct and should not be freed when this struct
    // is freed.
    request_t *last_request;
};

static uint64_t NEXT_COWN_ID = 0;

static shared_cown_t *
create_cown(void)
{
    shared_cown_t *cown = PyMem_RawMalloc(sizeof(shared_cown_t));
    if (cown == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    cown->refcount = 1;
    cown->id = _Py_atomic_add_uint64(&NEXT_COWN_ID, 1);
    cown->val = NULL;
    cown->last_request = NULL;

    return cown;
}

static void
cown_incref(shared_cown_t *cown)
{
    int64_t old_refcount = _Py_atomic_add_int64(&cown->refcount, 1);
    assert(old_refcount > 0);
}

static void
cown_decref(shared_cown_t *cown)
{
    assert(cown != NULL);

    int64_t old_refcount = _Py_atomic_add_int64(&cown->refcount, -1);
    // The atomic add operation returns the old value, so it reaches 0 if the
    // old value is 1.
    assert(old_refcount >= 1);
    if (old_refcount > 1) {
        return;
    }

    assert(cown->last_request == NULL);

    // Deallocate the cown data.
    if (cown->val != NULL && _PyXIData_ReleaseAndRawFree(cown->val) < 0) {
        assert(PyErr_Occurred());
        PyErr_FormatUnraisable("exception ignored when deallocating a cown");
    }

    PyMem_RawFree(cown);
}

static int
cown_set_value(shared_cown_t *cown, PyObject *val)
{
    PyThreadState *tstate = _PyThreadState_GET();

    _PyXIData_t *serialised_val = PyMem_RawCalloc(1, sizeof *serialised_val);
    if (serialised_val == NULL) {
        PyErr_NoMemory();
        return -1;
    }

    if (_PyObject_GetXIData(tstate, val, _PyXIDATA_FULL_FALLBACK,
                            serialised_val)
        < 0) {
        return -1;
    }

    // Delete the old value if it exists.
    if (cown->val != NULL) {
        _PyXIData_ReleaseAndRawFree(cown->val);
    }

    cown->val = serialised_val;

    return 0;
}

// The `Cown` Python type.
//
// A new `Cown` is created for each when block.
typedef struct {
    PyObject_HEAD

    shared_cown_t *cown;

    // This field is only set inside a when block. When a when block is
    // completed it is serialised and put in the global cown.
    PyObject *val;
} Cown;

static Cown *
cown_new(PyTypeObject *type, PyObject *_args, PyObject *_kwds)
{
    Cown *self = (Cown *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }

    self->cown = NULL;
    self->val = NULL;

    return self;
}

static int
cown_init(Cown *self, PyObject *args, PyObject *kwargs)
{
    mod_state *m = get_state(PyType_GetModuleByDef(Py_TYPE(self), &moduledef));

    if (self->cown != NULL) {
        PyErr_SetString(m->BOCError, "cown already initialised");
        return -1;
    }
    assert(self->val == NULL);

    PyObject *value;
    if (!PyArg_ParseTuple(args, "O", &value)) {
        return -1;
    }

    self->cown = create_cown();
    if (self->cown == NULL) {
        return -1;
    }

    if (cown_set_value(self->cown, value) < 0) {
        cown_decref(self->cown);
        self->cown = NULL;
        return -1;
    }

    return 0;
}

// Getter: Cown.val
static PyObject *
cown_get_val(Cown *self, void *_closure)
{
    mod_state *m = get_state(PyType_GetModuleByDef(Py_TYPE(self), &moduledef));

    if (self->val == NULL) {
        PyErr_SetString(m->BOCError,
                        "accessing the value of a cown outside a when block "
                        "is prohibited");
        return NULL;
    }

    return Py_NewRef(self->val);
}

// Setter: Cown.val
static int
cown_set_val(Cown *self, PyObject *value, void *_clusore)
{
    mod_state *m = get_state(PyType_GetModuleByDef(Py_TYPE(self), &moduledef));

    if (self->val == NULL) {
        PyErr_SetString(m->BOCError,
                        "accessing the value of a cown outside a when block "
                        "is prohibited");
        return -1;
    }

    if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "cannot delete the value of a cown");
        return -1;
    }

    Py_SETREF(self->val, Py_NewRef(value));
    return 0;
}

static int
cown_traverse(Cown *self, visitproc visit, void *arg)
{
    if (self->val != NULL) {
        Py_VISIT(self->val);
    }
    Py_VISIT(Py_TYPE(self));
    return 0;
}

static int
cown_clear(Cown *self)
{
    // It should not be possible for a `Cown` to be destructed inside a when
    // block because then there is always a reference to it from the
    // `when_block_run()` function.
    assert(self->val == NULL);

    return 0;
}

static void
cown_dealloc(Cown *self)
{
    if (self->cown != NULL) {
        cown_decref(self->cown);
    }

    PyTypeObject *type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    (void)cown_clear(self);
    type->tp_free(self);
    Py_DECREF(type);
}

static PyGetSetDef cown_getsetters[] = {
    { "val", (getter)cown_get_val, (setter)cown_set_val,
     PyDoc_STR("the value of the cown"), NULL },
    { NULL }  /* Sentinel */
};

static PyType_Slot Cown_type_slots[] = {
    { Py_tp_new,      (newfunc)cown_new        },
    { Py_tp_init,     (initproc)cown_init      },
    { Py_tp_dealloc,  (destructor)cown_dealloc },
    { Py_tp_traverse, cown_traverse            },
    { Py_tp_clear,    cown_clear               },
    { Py_tp_getset,   cown_getsetters          },
    { 0,              NULL                     },
};

static PyType_Spec Cown_type_spec = {
    .name = MODULE_NAME_STR ".Cown",
    .basicsize = sizeof(Cown),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE
             | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC,
    .slots = Cown_type_slots,
};

/******************************************************************************/
/* Boc implementation                                                         */

struct _request {
    // The behaviour enqueued after this request on this cown if any.
    behaviour_t *next_behaviour;

    // Set to true when the request has been scheduled.
    //
    // This is false during PL2 locking to prevent bad things from happening.
    // FIXME: Improve this comment :)
    int scheduled;
};

struct _behaviour {
    // The function that should be run when all cowns are available.
    //
    // It takes the module state, a pointer to some context, an array with all
    // cowns, and an integer indicating the number of cowns.
    void (*func)(mod_state *, void *, shared_cown_t **, size_t);

    // Some context to be passed to `func`.
    void *ctx;

    // Destructor for the xontext.
    void (*ctx_destructor)(void *);

    // The number of cowns/requests.
    size_t n_cowns;

    // A list of all the cowns.
    //
    // This is owned by this struct and must be freed accordingly.
    shared_cown_t **cowns;

    // A list of all the cowns sorted by ID.
    //
    // This is owned by this struct and must be freed accordingly.
    shared_cown_t **sorted_cowns;

    // A list of requests that has to be resolved.
    //
    // This is owned by this struct and must be freed accordingly.
    request_t *requests;

    // A count of all un]-resolved requests. The behaviour is run when this
    // reaches 0.
    ssize_t count;
};

static void schedule_behaviour(behaviour_t *behaviour);

// Compare cowns (`shared_cown_t`) based on IDs.
static inline int
cmp_cowns(const void *lhs_, const void *rhs_)
{
    const shared_cown_t *lhs = *(const shared_cown_t **)lhs_;
    const shared_cown_t *rhs = *(const shared_cown_t **)rhs_;
    return (lhs->id > rhs->id) - (lhs->id < rhs->id);
}

// Spawn a behaviour when all cowns are available.
//
// The `cowns` argument must be a tuple of `Cown`s. The caller is responsible of
// ensuring that it is a tuple with `PyTuple_Check()` but this function makes
// sure that every element is of the `Cown` type, otherwise it raises a
// `TypeError`. The argument is borrowed so it does  not steal a reference to
// `cowns`.
//
// The `additional_cown` argument takes an optional additional cown (or NULL)
// that will be placed at the end on the list of cowns. This is just to make
// this function compatible in as many scenarios as possible and avoid code
// duplication. The argument does not steal a reference to the `shared_cown_t`.
//
// The `ctx` is a context that is passed to `func` and later to
// `ctx_destructor`. There is no guarantee that `func` will be called but
// `ctx_destructor` will be called exactly once.
//
// However if this function fails with a negative return value, neither `cowns`
// will be freed nor `ctx_destructor` will be called.
static int
spawn_behaviour(PyObject *m,
                PyObject *cowns,
                shared_cown_t *additional_cown,
                void *ctx,
                void (*func)(mod_state *, void *, shared_cown_t **, size_t),
                void (*ctx_destructor)(void *))
{
    behaviour_t *behaviour = PyMem_RawCalloc(1, sizeof *behaviour);
    if (behaviour == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    ssize_t n_cowns = PyTuple_Size(cowns);
    if (n_cowns < 0) {
        goto error;
    }
    if (additional_cown != NULL) {
        n_cowns++;
    }
    behaviour->n_cowns = n_cowns;

    behaviour->cowns =
        PyMem_RawMalloc(behaviour->n_cowns * sizeof *behaviour->cowns);
    if (behaviour->cowns == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    for (ssize_t i = 0; i < PyTuple_GET_SIZE(cowns); i++) {
        PyObject *cown_ = PyTuple_GET_ITEM(cowns, i);
        assert(cown_ != NULL);
        if (!PyObject_TypeCheck(cown_, get_state(m)->Cown_type)) {
            PyErr_Format(PyExc_TypeError, "expected a Cown, got %R", cowns);
            goto error;
        }

        Cown *cown = (Cown *)cown_;
        behaviour->cowns[i] = cown->cown;
    }
    if (additional_cown != NULL) {
        behaviour->cowns[behaviour->n_cowns - 1] = additional_cown;
    }

    behaviour->sorted_cowns =
        PyMem_RawMalloc(behaviour->n_cowns * sizeof(shared_cown_t *));
    if (behaviour->sorted_cowns == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    // Sort the cowns.
    behaviour->sorted_cowns =
        memcpy(behaviour->sorted_cowns, behaviour->cowns,
               behaviour->n_cowns * sizeof *behaviour->sorted_cowns);
    qsort(behaviour->sorted_cowns, behaviour->n_cowns,
          sizeof *behaviour->sorted_cowns, cmp_cowns);

    behaviour->requests =
        PyMem_RawMalloc(behaviour->n_cowns * sizeof *behaviour->requests);
    if (behaviour->requests == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    // No fallible operations should be done from this point because the
    // `shared_cown_t`s in `behaviour->cowns` are increfed in this loop and we
    // don't bother undoing that at the `error` label.
    for (size_t i = 0; i < behaviour->n_cowns; i++) {
        behaviour->requests[i] = (request_t){ NULL, false };
        cown_incref(behaviour->cowns[i]);
    }

    // Set various fields.
    behaviour->func = func;
    behaviour->ctx = ctx;
    behaviour->ctx_destructor = ctx_destructor;
    behaviour->count = n_cowns + 1;

    schedule_behaviour(behaviour);

    return 0;
error:
    if (behaviour) {
        PyMem_RawFree(behaviour->requests);
        PyMem_RawFree(behaviour->sorted_cowns);
        PyMem_RawFree(behaviour->cowns);
        PyMem_RawFree(behaviour);
    }
    return -1;
}

static bool resolve_one(behaviour_t *behaviour);
static void run_behaviour(void *behaviour_);

// Schedules the behaviour.
//
// Performs two phase locking (2PL) over the enqueuing of the requests.
// This ensures that the overall effect of the enqueue is atomic.
static void
schedule_behaviour(behaviour_t *behaviour)
{
    // Complete first phase of 2PL enqueuing on all cowns.
    for (size_t i = 0; i < behaviour->n_cowns; i++) {
        shared_cown_t *cown = behaviour->sorted_cowns[i];
        request_t *request = &behaviour->requests[i];
        request_t *prev_req =
            _Py_atomic_exchange_ptr(&cown->last_request, request);
        if (prev_req == NULL) {
            // `request` is the first request in the queue so let's resolve it
            // immediately.
            _Py_atomic_add_ssize(&behaviour->count, -1);
            continue;
        }

        _Py_atomic_store_ptr_release(&prev_req->next_behaviour, behaviour);

        // Wait until `prev_req` is scheduled with a spin lock.
        while (!_Py_atomic_load_int_acquire(&prev_req->scheduled)) {
            SPIN_WAIT();
        }
    }

    // Finish the second phase of the 2PL enqueuing on all cowns.
    for (size_t i = 0; i < behaviour->n_cowns; i++) {
        _Py_atomic_store_int_release(&behaviour->requests[i].scheduled, true);
    }

    // Resolve the additional request.
    // FIXME: Better explonation here.
    if (resolve_one(behaviour)) {
        interpreter_pool_spawn(run_behaviour, behaviour);
    }
}

static void
run_behaviour(void *behaviour_)
{
    behaviour_t *behaviour = behaviour_;
    PyObject *m = PyImport_ImportModuleEx(MODULE_NAME_STR, NULL, NULL, NULL);
    if (m == NULL) {
        PyErr_FormatUnraisable("BOC: Couldn't import the " MODULE_NAME_STR
                               " on a separate sub-interpreter.");
        return;
    }

    behaviour->func(get_state(m), behaviour->ctx, behaviour->cowns,
                    behaviour->n_cowns);
    if (behaviour->ctx_destructor != NULL) {
        behaviour->ctx_destructor(behaviour->ctx);
    }

    Py_DECREF(m);

    // Collect all resolved behaviours.
    // This array is initialised on demand.
    behaviour_t **resolved_behaviours = NULL;
    size_t n_resolved_behaviours = 0;

    // Release the cowns.
    for (size_t i = 0; i < behaviour->n_cowns; i++) {
        shared_cown_t *cown = behaviour->sorted_cowns[i];
        request_t *request = &behaviour->requests[i];
        behaviour_t *next_behaviour;

        if (NULL
            == (next_behaviour =
                    _Py_atomic_load_ptr_acquire(&request->next_behaviour))) {
            // Just make a copy of `request` as it may be modified by the
            // compare-exchange operation.
            request_t *current_request = request;
            if (_Py_atomic_compare_exchange_ptr(&cown->last_request,
                                                &current_request, NULL)) {
                // This was the last request for the cown.
                continue;
            }

            // Some other request is in the process of being enqueued to this
            // cown. It has currently set the `cown->last_request` pointer but
            // not the `request->next_behaviour` pointer. Let's wait until that
            // is done. This shouldn't take long.
            while (NULL
                   == (next_behaviour = _Py_atomic_load_ptr_acquire(
                           &request->next_behaviour))) {
                SPIN_WAIT();
            }
        }
        // We now know that `next_behaviour` is set.

        if (resolve_one(next_behaviour)) {
            if (resolved_behaviours == NULL) {
                // Allocate the `resolved_behaviours` array.

                if (n_resolved_behaviours > 0) {
                    // The allocation of `resolved_behaviours` must have failed
                    // earlier, so don't do anything.
                    continue;
                }

                resolved_behaviours = PyMem_RawCalloc(
                    behaviour->n_cowns, sizeof *resolved_behaviours);
                if (resolved_behaviours == NULL) {
                    PyErr_NoMemory();
                    PyErr_FormatUnraisable(
                        "BOC: ran out of memory when resolving "
                        "behaviours");
                    n_resolved_behaviours++;
                    continue;
                }
            }
            resolved_behaviours[n_resolved_behaviours] = next_behaviour;
            n_resolved_behaviours++;
        }
    }

    // Decref all cowns
    for (size_t i = 0; i < behaviour->n_cowns; i++) {
        cown_decref(behaviour->sorted_cowns[i]);
    }

    PyMem_RawFree(behaviour->cowns);
    PyMem_RawFree(behaviour->sorted_cowns);
    PyMem_RawFree(behaviour->requests);
    PyMem_RawFree(behaviour);

    // Resolve any resolved behaviours.
    if (resolved_behaviours != NULL) {
        assert(n_resolved_behaviours > 0);
        // One behaviour can be run as a tail-call to this function so we don't
        // have to spawn it onto the interpreter pool.
        behaviour_t *first_behaviour = resolved_behaviours[0];

        for (size_t i = 1; i < n_resolved_behaviours; i++) {
            if (interpreter_pool_spawn(run_behaviour, resolved_behaviours[i])
                < 0) {
                PyErr_FormatUnraisable(
                    "BOC: error when trying to spawn resolved behaviour");
            }
        }

        PyMem_RawFree(resolved_behaviours);

        // Run one behaviour as a tail-call.
        run_behaviour(first_behaviour);
    }
}

// This function resolves one request on the behaviour and returns true iff all
// requests have been resolved and the behaviour should be run.
static bool
resolve_one(behaviour_t *behaviour)
{
    ssize_t old_count = _Py_atomic_add_ssize(&behaviour->count, -1);
    // `count` is 0 if `old_count` is 1.
    return old_count == 1;
}

// Destructor for the data/context used by a when block. This is currently a
// Python function stored in a `_PyXIData_t`. In the future, this will hopefully
// be a frozen Python function instead.
static void
when_block_destructor(void *ctx)
{
    _PyXIData_t *func = ctx;
    if (_PyXIData_ReleaseAndRawFree(func) < 0) {
        assert(PyErr_Occurred());
        PyErr_FormatUnraisable(
            "BOC: exception ignored when releasing the function for a when "
            "block");
    }
}

// Run a when block.
static void
when_block_run(mod_state *m,
               void *ctx,
               shared_cown_t **shared_cowns,
               size_t n_cowns)
{
    _PyXIData_t *serialised_func = ctx;

    bool error = false;
    PyObject *func = NULL;
    PyObject *cowns = NULL;
    size_t initialised_cowns = 0;
    PyObject *retval = NULL;

    func = _PyXIData_NewObject(serialised_func);
    if (func == NULL) {
        error = true;
        goto finally;
    }

    cowns = PyTuple_New(n_cowns - 1);  // The last cown is used for the return
                                       // value.
    if (cowns == NULL) {
        error = true;
        goto finally;
    }

    for (size_t i = 0; i < n_cowns - 1; i++) {
        Cown *cown = cown_new(m->Cown_type, NULL, NULL);
        if (cown == NULL) {
            error = true;
            goto finally;
        }

        cown->cown = shared_cowns[i];
        cown_incref(cown->cown);
        cown->val = _PyXIData_NewObject(cown->cown->val);
        if (cown->val == NULL) {
            Py_DECREF(cown);
            error = true;
            goto finally;
        }

        PyTuple_SET_ITEM(cowns, i, cown);
        initialised_cowns++;
    }

    shared_cown_t *retval_cown = shared_cowns[n_cowns - 1];

    retval = PyObject_Call(func, cowns, NULL);
    if (retval == NULL) {
        assert(PyErr_Occurred());
        PyErr_FormatUnraisable("BOC: Uncought exception in when block '%U'",
                               ((PyFunctionObject *)func)->func_name);
    }
    else {
        if (cown_set_value(retval_cown, retval) < 0) {
            error = true;
            goto finally;
        }
    }

finally:
    // If an exception occurred, it is not from the when block (as that was
    // handled immediately) but some internal call.
    if (error) {
        PyObject *name = NULL;
        if (func != NULL) {
            name = ((PyFunctionObject *)func)->func_name;
        }
        PyErr_FormatUnraisable("BOC: error when trying to run when block '%V'",
                               name, "unknown");
    }

    Py_XDECREF(func);
    Py_XDECREF(retval);

    if (cowns != NULL) {
        for (size_t i = 0; i < initialised_cowns; i++) {
            Cown *cown = (Cown *)PyTuple_GET_ITEM(cowns, i);

            if (cown_set_value(cown->cown, cown->val) < 0) {
                PyErr_FormatUnraisable(
                    "BOC: Failed to set value after when block was complete. "
                    "The value is: %R",
                    cown->val);
            }
            Py_DECREF(cown->val);
            cown->val = NULL;
        }
        Py_DECREF(cowns);
    }
}

static PyObject *
when(PyObject *m, PyObject *args)
{
    PyObject *func = NULL;
    PyObject *cowns = NULL;
    _PyXIData_t *serialised_func = NULL;

    if (!PyArg_ParseTuple(args, "O!O!:" MODULE_NAME_STR ".when",
                          &PyFunction_Type, &func, &PyTuple_Type, &cowns)) {
        goto error;
    }

    // Create a cown for the return value of the when block.
    Cown *retval_cown = cown_new(get_state(m)->Cown_type, NULL, NULL);
    if (retval_cown == NULL) {
        goto error;
    }
    retval_cown->cown = create_cown();
    if (retval_cown->cown == NULL) {
        goto error;
    }

    serialised_func = PyMem_RawCalloc(1, sizeof *serialised_func);
    if (serialised_func == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    PyThreadState *tstate = _PyThreadState_GET();
    if (_PyFunction_GetXIData(tstate, func, serialised_func) < 0) {
        PyObject *exc = PyErr_GetRaisedException();
        // Try to pickle instead.
        if (_PyPickle_GetXIData(tstate, func, serialised_func) < 0) {
            PyErr_SetRaisedException(exc);
            goto error;
        }
        Py_DECREF(exc);
    }

    if (spawn_behaviour(m, cowns, retval_cown->cown, serialised_func,
                        when_block_run, when_block_destructor)
        < 0) {
        goto error;
    }

    return (PyObject *)retval_cown;
error:
    Py_XDECREF(retval_cown);

    if (serialised_func != NULL) {
        // Temporarily unset the exception while freeing serialised_func.
        PyObject *exc = PyErr_GetRaisedException();
        when_block_destructor(serialised_func);
        PyErr_SetRaisedException(exc);
    }

    return NULL;
}

/******************************************************************************/
/* An API to wait on the values of cowns from the current thread outside a when
 * block.  It is unclear if this API should exist: We want to encourage users to
 * only use cowns from when blocks to avoid dead locks and adhear to the style
 * of behaviour oriented concurrency.  However, there might be cases when the
 * user actually needs to get the value of a cown from outside a when block, and
 * then this API exists.
 *
 * The main entry point is the `block_on_cowns()` function that spawns a
 * behaviour on a set of cowns.  This behaviour runs the
 * `finished_blocking_on_cowns()` function that stores the value(s) of the
 * cown(s) in a shared context and notifies the calling thread.  This shared
 * context is the `block_on_cowns_t` struct.
 */

// This struct contains the context for a `block_on_cowns()` call. It is used
// both by that function and the `finished_blocking_on_cowns()` function that is
// run by the BOC runtime when all cowns are available.
//
// Because the caller may terminate before the cowns are available. Either due
// to a time out or an interrupt. This means that it is unclear who owns this
// struct and in particular who is responsible to free it. We solve this by
// having a reference count that starts at 2 and is decrefed both by the
// blocking caller and when the behaviour-function managed by the BOC runtime is
// finished.
typedef struct {
    // A reference count that starts at 2 and can only be decremented.
    int refcount;

    // A mutex that will be unlocked when all cowns are available.
    PyMutex lock;

    // A tuple of the cowns values or NULL if an exception has been raised.
    _PyXIData_t *vals;
} block_on_cowns_t;

// Decrement the reference count on `block_on_cowns_t` and possibly destruct it.
static void
block_on_cowns_decref(block_on_cowns_t *ctx)
{
    int old_refcount = _Py_atomic_add_int(&ctx->refcount, -1);
    // The atomic add operation returns the old value, so it reaches 0 if the
    // old value is 1.
    assert(old_refcount >= 1);
    if (old_refcount > 1) {
        return;
    }

    if (ctx->vals != NULL && _PyXIData_ReleaseAndRawFree(ctx->vals) < 0) {
        assert(PyErr_Occurred());
        PyErr_FormatUnraisable(
            "exception ignored when deallocating `block_on_cowns_t->vals");
    }
    PyMem_RawFree(ctx);
}

static void
finished_blocking_on_cowns(mod_state *m,
                           void *ctx_,
                           shared_cown_t **cowns,
                           size_t n_cowns)
{
    block_on_cowns_t *ctx = ctx_;

    PyThreadState *tstate = _PyThreadState_GET();

    PyObject *vals = PyTuple_New(n_cowns);
    if (vals == NULL) {
        goto error;
    }

    // Fetch the values of all the cowns and put them in the tuple.
    for (size_t i = 0; i < n_cowns; i++) {
        PyObject *val = _PyXIData_NewObject(cowns[i]->val);
        if (val == NULL) {
            goto error;
        }

        PyTuple_SET_ITEM(vals, i, val);
    }

    // Serialise the `vals` tuple so it can be sent between interpreters and
    // store it in the `ctx->vals` field.
    ctx->vals = PyMem_RawCalloc(1, sizeof *ctx->vals);
    if (ctx->vals == NULL) {
        PyErr_NoMemory();
        goto error;
    }
    if (_PyObject_GetXIData(tstate, vals, _PyXIDATA_FULL_FALLBACK, ctx->vals)
        < 0) {
        goto error;
    }

    goto finally;
error:
    PyErr_FormatUnraisable("BOC: Exception during a blocking call on cowns");
finally:
    Py_XDECREF(vals);
    PyMutex_Unlock(&ctx->lock);
}

static PyObject *
block_on_cowns(PyObject *m, PyObject *args)
{
    PyObject *result = NULL;
    block_on_cowns_t *ctx = NULL;
    PyObject *cowns = NULL;
    long long timeout_;

    if (!PyArg_ParseTuple(args, "O!L:" MODULE_NAME_STR ".block_on_cowns",
                          &PyTuple_Type, &cowns, &timeout_)) {
        goto finally;
    }

    if (timeout_ > PyTime_MAX) {
        PyErr_Format(PyExc_ValueError, "timeout too large: %lld ns", timeout_);
        goto finally;
    }
    if (timeout_ < -1) {
        timeout_ = -1;
    }
    PyTime_t timeout = timeout_;

    ctx = PyMem_RawMalloc(sizeof *ctx);
    if (ctx == NULL) {
        goto finally;
    }

    *ctx = (block_on_cowns_t){
        .refcount = 2,
        .lock = (PyMutex){ _Py_LOCKED },
        .vals = NULL,
    };

    if (spawn_behaviour(m, cowns, NULL, ctx, finished_blocking_on_cowns,
                        (void (*)(void *))block_on_cowns_decref)
        < 0) {
        goto finally;
    }

    // Wait until `ctx->lock` is unlocked.
    PyLockStatus lock_result = _PyMutex_LockTimed(
        &ctx->lock, timeout, _PY_LOCK_DETACH | _PY_LOCK_HANDLE_SIGNALS);
    if (lock_result == PY_LOCK_FAILURE) {
        PyErr_Format(PyExc_TimeoutError,
                     "blocking on cowns timed out after %lld ns", timeout);
        goto finally;
    }
    else if (lock_result == PY_LOCK_INTR) {
        PyErr_Format(PyExc_KeyboardInterrupt,
                     "blocking on cowns was interrupted");
        goto finally;
    }
    assert(lock_result == PY_LOCK_ACQUIRED);

    if (ctx->vals == NULL) {
        PyErr_Format(PyExc_RuntimeError,
                     "uncought exception in the BOC runtime");
        goto finally;
    }

    PyObject *vals = _PyXIData_NewObject(ctx->vals);
    if (vals == NULL) {
        goto finally;
    }
    result = vals;

finally:
    if (ctx != NULL) {
        block_on_cowns_decref(ctx);
    }

    return result;
}

static PyMethodDef mod_functions[] = {
    { "when",                  when,                  METH_VARARGS, PyDoc_STR("Create a when block.") },
    { "block_on_cowns",        block_on_cowns,        METH_VARARGS,
     PyDoc_STR("Block the current thread to wait on the values of a set of "
                "cowns.")                                                                             },
    { "interpreter_pool_info", interpreter_pool_info, METH_NOARGS,
     PyDoc_STR("Get information about the current state of the interpreter "
                "pool.")                                                                              },
    { NULL,                    NULL,                  0,            NULL                              }  // Sentinel
};

static int
mod_exec(PyObject *m)
{
    mod_state *state = get_state(m);

    state->Cown_type =
        (PyTypeObject *)PyType_FromModuleAndSpec(m, &Cown_type_spec, NULL);
    if (state->Cown_type == NULL) {
        return -1;
    }

    if (PyModule_AddType(m, state->Cown_type) < 0) {
        return -1;
    }

    state->BOCError = PyErr_NewExceptionWithDoc(
        MODULE_NAME_STR ".BOCError",
        "Error raised for misuse or internal failures in the BOC runtime.",
        NULL, NULL);
    if (state->BOCError == NULL) {
        return -1;
    }
    if (PyModule_AddObjectRef(m, "BOCError", state->BOCError) < 0) {
        return -1;
    }

    return 0;
}

static int
mod_traverse(PyObject *m, visitproc visit, void *arg)
{
    mod_state *st = get_state(m);
    Py_VISIT(st->Cown_type);
    Py_VISIT(st->BOCError);
    return 0;
}

static int
mod_clear(PyObject *m)
{
    mod_state *st = get_state(m);
    Py_CLEAR(st->Cown_type);
    Py_CLEAR(st->BOCError);
    return 0;
}

static void
mod_free(void *m)
{
    if (interpreter_pool != NULL) {
        PyThreadState *tstate = PyThreadState_Get();
        if (tstate != NULL && tstate->interp == PyInterpreterState_Main()) {
            interpreter_pool_shutdown();
        }
    }
}

static PyModuleDef_Slot mod_slots[] = {
    { Py_mod_exec,                  mod_exec                             },
    { Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED },
    { Py_mod_gil,                   Py_MOD_GIL_NOT_USED                  },
    { 0,                            NULL                                 }
};

static struct PyModuleDef moduledef = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = MODULE_NAME_STR,
    .m_size = sizeof(mod_state),
    .m_methods = mod_functions,
    .m_slots = mod_slots,
    .m_traverse = mod_traverse,
    .m_clear = mod_clear,
    .m_free = mod_free,
};

PyMODINIT_FUNC
MODINIT_FUNC_NAME(void)
{
    return PyModuleDef_Init(&moduledef);
}
