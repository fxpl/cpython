#include "Python.h"
#include <stdbool.h>
#include "object.h"             // _Py_IsImmutable
#include "pycore_descrobject.h" // _PyMethodWrapper_Type
#include "pycore_gc.h"          // _PyGCHead_NEXT, _PyGCHead_PREV, _Py_FROM_GC
#include "pycore_interp.h"      // PyThreadState_Get
#include "pycore_list.h"
#include "pycore_object.h"
#include "pycore_ownership.h"
#include "pycore_pyerrors.h"
#include "pycore_runtime.h"     // _Py_ID
#include "pycore_region.h"      // _PyRegion_Get(), Py_Region
#include "pycore_unicodeobject.h"
#include "pycore_dict.h" // _PyDict_Reachable
#include "pyerrors.h"
#include "refcount.h"

// Macro that jumps to error, if the expression `x` does not succeed.
#define SUCCEEDS(x) { do { int r = (x); if (r != 0) goto error; } while (0); }

#define _Py_region_data_CAST(region) _Py_CAST(_Py_region_data*, region)

#define REGIO_SENTINEL_VALUE 0x12345678

static int init_state(_Py_ownership_state *state)
{
    state->warned_types = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if(state->warned_types == NULL){
        return -1;
    }
#ifdef Py_OWNERSHIP_INVARIANT
    state->invariant_state = Py_OWNERSHIP_INVARIANT_DISABLED;
#endif

    return 0;
}

static int init_import_state(_Py_ownership_state *state) {

    state->tick = 2;
    // In debug mode, we can store the traceback for debugging purposes.
    // Get a traceback object to use as the ownership location.
#ifdef Py_DEBUG
    PyObject *traceback_module = PyImport_ImportModule("traceback");
    if (traceback_module != NULL) {
        state->traceback_func = PyObject_GetAttrString(traceback_module, "format_stack");
        Py_DECREF(traceback_module);
    }

    state->location_key = PyUnicode_FromString("__ownership_location__");
    if (state->location_key == NULL) {
        return -1;
    }

    PyInterpreterState *interp = PyInterpreterState_Get();
    if (interp == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to get the interpreter state");
        return -1;
    }

    _PyUnicode_InternImmortal(interp, &state->location_key);
#endif

    return 0;
}

static _Py_ownership_state* get_ownership_state(void)
{
    PyInterpreterState *interp = PyInterpreterState_Get();
    if (interp == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to get the interpreter state");
        return NULL;
    }

    _Py_ownership_state *state = &interp->ownership;
    if (state->warned_types == NULL) {
        if (init_state(state) == -1) {
            PyErr_SetString(PyExc_RuntimeError, "Failed to initialize ownership state");
            return NULL;
        }
    }

    return state;
}


// Wrapper around tp_traverse that also visits the type object.
// tp_traverse does not visit the type for non-heap types, but
// tp_reachable should visit all reachable objects including the type.
static int
traverse_via_tp_traverse(PyObject *obj, visitproc visit, void *state)
{
    PyTypeObject *tp = Py_TYPE(obj);

    // `tp_traverse` of heap types *should* include a
    // `Py_VISIT(Py_TYPE(self));` since around Python 2.7 but
    // there are still plenty of types that don't. LLMs currently
    // also don't do this consistently.
    //
    // FIXME(regions): xFrednet: Handle this...

    // Visit the type with traverse
    traverseproc traverse = tp->tp_traverse;
    if (traverse != NULL) {
        int err = traverse(obj, visit, state);
        if (err) {
            return err;
        }
    }

    // Manually visit the type if it's a static type
    if (!(tp->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        return visit((PyObject *)Py_TYPE(obj), state);
    }

    return 0;
}

// Returns the appropriate traversal function for reaching all references
// from an object. Prefers tp_reachable, falls back to tp_traverse wrapped
// to also visit the type. Emits a warning once per type on fallback.
static traverseproc
get_reachable_proc(PyTypeObject *tp)
{
    if (tp->tp_reachable != NULL) {
        return tp->tp_reachable;
    }

    struct _Py_ownership_state *state = get_ownership_state();
    if (state != NULL &&
        _Py_hashtable_get(state->warned_types, (void *)tp) == NULL)
    {
        _Py_hashtable_set(state->warned_types, (void *)tp, (void *)1);
        if (tp->tp_traverse != NULL) {
            PySys_FormatStderr(
                "regions: type '%.100s' has tp_traverse but no tp_reachable\n",
                tp->tp_name);
        } else {
            PySys_FormatStderr(
                "regions: type '%.100s' has no tp_traverse and no tp_reachable\n",
                tp->tp_name);
        }
    }

    // Always return the wrapper; even when tp_traverse is NULL, the wrapper
    // will still visit the type object which tp_reachable is expected to do.
    return traverse_via_tp_traverse;
}

static _Py_ownership_state* get_ownership_state_for_traverse(void)
{
    _Py_ownership_state* state = get_ownership_state();
    if (state == NULL) {
        return NULL;
    }

    if (state->tick == 0) {
        if (init_import_state(state) != 0) {
            PyErr_SetString(PyExc_RuntimeError, "Failed to initialize ownership state for traverse");
            return NULL;
        }
    }

    return state;
}

#define IS_OPEN_REGION_TICK(tick) ((tick) % 2 == 0)

Py_ssize_t _PyOwnership_get_current_tick(void) {
    _Py_ownership_state* state = get_ownership_state();
    if (state == NULL) {
        return 0;
    }

    return state->tick;
}

Py_ssize_t _PyOwnership_get_open_region_tick(void) {
    _Py_ownership_state* state = get_ownership_state();
    if (state == NULL) {
        return 0;
    }

    // Only incremeant the counter, if the state is untrusted
    if (!IS_OPEN_REGION_TICK(state->tick)) {
        state->tick += 1;

        // Prevent overflow, by resetting early
        if (state->tick > (PY_SSIZE_T_MAX - 10)) {
            state->tick = 2;
        }
    }
    assert(IS_OPEN_REGION_TICK(state->tick));

    return state->tick;
}

int _PyOwnership_notify_untrusted_code(const char* reason) {
    // The ownership state is not always available during initialization.
    // Regions are also only created after initialization, so it'll be safe
    // to ignore this here.
    if (Py_IsInitialized() == 0) {
        return 0;
    }

    _Py_ownership_state* state = get_ownership_state();
    if (state == NULL) {
        return 1;
    }

    // Only increment the counter, if the state is trusted
    if (IS_OPEN_REGION_TICK(state->tick)) {
        state->tick += 1;
    }
    assert(!IS_OPEN_REGION_TICK(state->tick));

#ifdef Py_DEBUG
    PyObject* name = PyUnicode_InternFromString(reason);
    if (name != NULL) {
        Py_XSETREF(state->last_dirty_reason, name);
    }
#endif
    // Everything is alright
    return 0;
}

PyObject* _PyOwnership_get_last_dirty_reason(void) {
#ifdef Py_DEBUG
    _Py_ownership_state* state = get_ownership_state();
    if (state == NULL) {
        return NULL;
    }

    PyObject* reason = PyRegion_XNewRef(state->last_dirty_reason);
    if (reason) {
        return reason;
    }
#endif

    Py_RETURN_NONE;
}

/* This function returns true for C wrappers around functions, types and
 * all kinds of wrappers around C with immutable state. For ownership these
 * can be seen as immutable, meaning they can be referenced from immutable
 * objects and from inside regions.
 */
int _PyOwnership_is_c_wrapper(PyObject* obj){
    return PyCFunction_Check(obj) || Py_IS_TYPE(obj, &_PyMethodWrapper_Type) || Py_IS_TYPE(obj, &PyWrapperDescr_Type);
}

static int push(PyObject* s, PyObject* item) {
    if (item == NULL) {
        return 0;
    }

    if (!PyList_Check(s)) {
        PyErr_SetString(PyExc_TypeError, "Expected a list");
        return -1;
    }

    return _PyList_AppendTakeRef(_PyList_CAST(s), PyRegion_NewRef(item));
}

static PyObject* pop(PyObject* s) {
    PyObject* item;
    Py_ssize_t size = PyList_Size(s);
    if (size == 0) {
        return NULL;
    }

    item = PyList_GetItem(s, size - 1);
    if (item == NULL) {
        return NULL;
    }

    if (PyList_SetSlice(s, size - 1, size, NULL)) {
        return NULL;
    }

    return item;
}

typedef struct ownership_traverse_state {
    PyObject *source;
    PyObject *dfs_stack;

    ownershipvisitproc caller_visit;
    void *caller_state;
} ownership_traverse_state;

static int ownership_visit(PyObject* target, void* traverse_state_void)
{
    // References to NULL can be ignored
    if (target == NULL)
        return 0;

    // Cast the state for easier access
    ownership_traverse_state *traverse_state =
        (ownership_traverse_state*)traverse_state_void;

    // Call the visit function
    int result = (traverse_state->caller_visit)(
            traverse_state->source,
            target,
            traverse_state->caller_state
        );

    // Enqueue the target if it should be traversed
    if (result == Py_OWNERSHIP_TRAVERSE_VISIT) {
        result = Py_OWNERSHIP_TRAVERSE_SKIP;

        if (push(traverse_state->dfs_stack, target)) {
            PyErr_NoMemory();
            return -1;
        }
    }

    return result;
}

/* This function calls the `visit` function for the fields of the `obj`
 * which should be effected by ownership. The `data` pointer will be
 * passed along as the second argument to `visit`.
 */
int _PyOwnership_traverse_obj(PyObject *obj, visitproc visit, void *data) {
    traverseproc proc = get_reachable_proc(Py_TYPE(obj));
    SUCCEEDS(proc(obj, visit, data));

    return 0;
error:
    return -1;
}

static int init_traverse_state(
    ownership_traverse_state *state,
    ownershipvisitproc caller_visit,
    void *caller_state
) {
    state->dfs_stack = NULL;
    state->dfs_stack = PyList_New(0);
    if (state->dfs_stack == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to create DFS stack for object graph traversal");
        return -1;
    }

    state->caller_visit = caller_visit;
    state->caller_state = caller_state;

    return 0;
}

/* This function traverses the object graph reachable from the given object.
 *
 * For every object it will call the `caller_check` function to determine if
 * the object should be traversed. For every outgoing reference it will then
 * call `caller_visit` which indicates if the referenced object should be
 * traversed.
 *
 * This function will also store the current stacktrace in debug builds.
 */
int _PyOwnership_traverse_object_graph(
    PyObject *obj,
#ifdef Py_DEBUG
    int is_region_traversal,
#endif
    ownershipcheckproc caller_check,
    ownershipvisitproc caller_visit,
    void *caller_state
) {
    int result = 0;

#ifdef Py_DEBUG
    // This has to be declared early to support the `Py_XDECREF` if any of the
    // `SUCCEEDS` fails
    PyObject* location = NULL;
#endif

    // Enable the invariant. It has to be enabled at the beginning to allow
    // reentry and failure in internal calls.
    SUCCEEDS(_PyOwnership_invariant_enable());
    // This function incrementally marks new objects as frozen. During this
    // process it is possible that frozen objects point to mutable ones. This
    // therefore needs to pause the invariant. Otherwise we might get an
    // exception when freezing calls into Python and triggers the invariant.
    SUCCEEDS(_PyOwnership_invariant_pause());

    // Initialize the traverse state
    ownership_traverse_state traverse_state;
    SUCCEEDS(init_traverse_state(&traverse_state, caller_visit, caller_state));

    // Initialize ownership state
    _Py_ownership_state *ownership_state = get_ownership_state_for_traverse();
    if (ownership_state == NULL) {
        goto error;
    }

#ifdef Py_DEBUG
    // FIXME(regions): xFrednet: Creating new references from objects which are
    //                 currently being traversed is not supported rn. This form
    //                 of reentry is hard to deal with, but it's possible (I believe)
    if (ownership_state->traceback_func != NULL && !is_region_traversal) {
        PyObject *stack = PyObject_CallFunctionObjArgs(ownership_state->traceback_func, NULL);
        if (stack != NULL) {
            // Add the type name to the top of the stack, can be useful.
            PyObject* typename = PyObject_GetAttrString(_PyObject_CAST(Py_TYPE(obj)), "__name__");
            push(stack, typename);
            location = stack;

            // Freezing the location allows all objects to reference it.
            if (is_region_traversal) {
                SUCCEEDS(_PyImmutability_Freeze(location));
                SUCCEEDS(_PyImmutability_Freeze(ownership_state->location_key));
            }
        }
    }
#endif

    // Push the current object to the pending stack
    SUCCEEDS(push(traverse_state.dfs_stack, obj));

    // While there is an object in the pending stack, check it
    while(PyList_Size(traverse_state.dfs_stack) != 0){
        PyObject* item = pop(traverse_state.dfs_stack);

#ifdef Py_DEBUG
        // Set the location early for freezing calls
        if (location != NULL) {
            // Some objects don't have attributes that can be set.
            // As this is a Debug only feature, we could potentially increase the object
            // size to allow this to be stored directly on the object.
            if (PyObject_SetAttr(item, ownership_state->location_key, location) < 0) {
                // Ignore failure to set _freeze_location
                PyErr_Clear();
            }
        }
#endif

        switch (caller_check(item, caller_state)) {
            // The object is fine, but shouldn't be traversed
            case Py_OWNERSHIP_TRAVERSE_SKIP:
                continue;

            // The object is okay and should be traversed
            case Py_OWNERSHIP_TRAVERSE_VISIT:
                traverse_state.source = item;
                SUCCEEDS(_PyOwnership_traverse_obj(item, ownership_visit, (void*)&traverse_state));
                break;

            // An error occured
            default:
                goto error;
        }
    }

    goto finally;

error:
    result = -1;

finally:
#ifdef Py_DEBUG
    Py_XDECREF(location);
#endif
    Py_XDECREF(traverse_state.dfs_stack);
    // Indicate that this funciton no longer requires the invariant to be paused.
    // This can't use the `SUCCEEDS` macro, since that one would jump to the
    // `error` label above.
    if (_PyOwnership_invariant_resume() != 0) {
        result = -1;
    }
    return result;
}

// All code belonging to the invariant
#ifdef Py_OWNERSHIP_INVARIANT

static void throw_invariant_error(
    PyObject* src,
    PyObject* tgt,
    const char *format_str,
    PyObject *format_arg
) {
    // Don't stomp existing exception
    PyThreadState *tstate = PyThreadState_Get();
    if (!tstate || _PyErr_Occurred(tstate)) {
        return;
    }

    // Create the error, this sets the error value in `tstate`
    PyErr_Format(PyExc_RuntimeError, format_str, format_arg);

    // Set source and target fields
    // Get the current exception (should be a RuntimeError)
    PyObject *exc = PyErr_GetRaisedException();
    assert(exc && PyObject_TypeCheck(exc, (PyTypeObject *)PyExc_RuntimeError));

    // Add 'source' and 'target' attributes to the exception
    PyObject_SetAttr(exc, &_Py_ID(source), src ? src : Py_None);
    PyObject_SetAttr(exc, &_Py_ID(target), tgt ? tgt : Py_None);

    PyErr_SetRaisedException((PyObject*)exc);
}

// Lifted from Python/gc.c
//******************************** */
typedef struct _gc_runtime_state GCState;
#define GEN_HEAD(gcstate, n) ((n == 0) ? (&(gcstate)->young.head) : (&(gcstate)->old[n - 1].head))
#define GC_NEXT _PyGCHead_NEXT
#define GC_PREV _PyGCHead_PREV
#define FROM_GC _Py_FROM_GC
//******************************** */

typedef struct _check_invariant_state {
    PyObject *src;
    // A list of regions which have been checked during this pass.
    Py_region_t regions;
} _check_invariant_state;

static void _check_invariant_state_track(_check_invariant_state *state, Py_region_t region) {
    if (region == _Py_LOCAL_REGION
        || region == _Py_IMMUTABLE_REGION
        || region == _Py_COWN_REGION
    ) {
        return;
    }

    _Py_region_data *data = _Py_region_data_CAST(region);

    // Each region should only be added once
    if (data->invariant_data.next != NULL_REGION) {
        return;
    }

    // Add the region to the linked list
    data->invariant_data.next = state->regions;
    state->regions = region;
}

static int validate_check_invariant_state(_check_invariant_state* state) {
    // Validate the visited region
    Py_region_t region = state->regions;
    while (region != REGIO_SENTINEL_VALUE) {
        _Py_region_data *data = _Py_region_data_CAST(region);

        if (_PyRegion_IsDirty(region)) {
            // Dirty regions can be checked, if PY_OWNERSHIP_INVARIANT_CHECK_DIRTY is set
            const char* env = Py_GETENV("PY_OWNERSHIP_INVARIANT_CHECK_DIRTY");
            if (!env) {
                goto next;
            }
        }

        if ((data->invariant_data.lrc != 0 || data->invariant_data.osc != 0)
            && !_PyRegion_IsOpen(region)
        ) {
            throw_invariant_error(
                data->bridge, NULL,
                "Invariant Error: References into `source` were found, but the region is closed",
                Py_None);
            return -1;
        }

        // This value is just an upper bound, since there can be references
        // from non GC objects, for example on the stack
        if (data->lrc < data->invariant_data.lrc) {
            throw_invariant_error(
                data->bridge, NULL,
                "Invariant Error: The LRC of the region in `source` is too high",
                Py_None);
            return -1;
        }

        // This value is just an upper bound, since there can be references
        // from non GC objects, for example on the stack
        if (data->osc < data->invariant_data.osc) {
            throw_invariant_error(
                data->bridge, NULL,
                "Invariant Error: The OSC of the region in `source` is too high",
                Py_None);
            return -1;
        }

    next:
        // Get the next region
        region = data->invariant_data.next;
    }

    return 0;
}

static void clear_check_invariant_state(_check_invariant_state* state) {
    // Clear temporary region data
    while (state->regions != REGIO_SENTINEL_VALUE)
    {
        _Py_region_data *data = _Py_region_data_CAST(state->regions);

        // Get the next region
        state->regions = data->invariant_data.next;

        // Clear data
        data->invariant_data.lrc = 0;
        data->invariant_data.osc = 0;
        data->invariant_data.next = NULL_REGION;
    }
}

static int check_invariant_validate_immutable(PyObject* obj) {
    // Immutable objects should be in the immutable region
    if (_PyRegion_Get(obj) != _Py_IMMUTABLE_REGION) {
        throw_invariant_error(
            obj, NULL,
            "Invariant Error: Immutable objects should be in the immutable region",
            Py_None);
        return -1;
    }

    return 0;
}

static int check_invariant_visit_immutable(PyObject* tgt, _check_invariant_state* state) {
    PyObject* src = state->src;

    // C wrappers are special and allowed
    if (_PyOwnership_is_c_wrapper(tgt)) {
        return 0;
    }

    // Make sure the immutable source only points to immutable objects
    if (!_Py_IsImmutable(tgt)) {
        throw_invariant_error(
            src, tgt,
            "Invariant Error: An immutable objects points to a mutable one",
            Py_None);
        return -1;
    }

    return 0;
}

static int check_invariant_visit_owned(PyObject* tgt, _check_invariant_state* state) {
    PyObject* src = state->src;

    Py_region_t src_region = _PyRegion_Get(src);
    Py_region_t tgt_region = _PyRegion_Get(tgt);

    // This should never happen, since immutable objects have their own visit
    // funciton
    assert(src_region != _Py_IMMUTABLE_REGION);

    // C wrappers are special and allowed
    if (_PyOwnership_is_c_wrapper(tgt)) {
        return 0;
    }

    // References to objects in the cown and immutable regions are allowed
    if (tgt_region == _Py_IMMUTABLE_REGION || tgt_region == _Py_COWN_REGION) {
        return 0;
    }

    // Intra-region references are allowed
    if (src_region == tgt_region) {
        return 0;
    }

    _check_invariant_state_track(state, tgt_region);

    // Dirty regions are basically allowed to do anything
    if (_PyRegion_IsDirty(src_region)) {
        // Dirty regions can be checked, if PY_OWNERSHIP_INVARIANT_CHECK_DIRTY is set
        const char* env = Py_GETENV("PY_OWNERSHIP_INVARIANT_CHECK_DIRTY");
        if (!env) {
            return 0;
        }
    }

    // Objects inside a region are not allowed to reference local objects
    if (tgt_region == _Py_LOCAL_REGION) {
        throw_invariant_error(
            src, tgt,
            "Invariant Error: A owned object is referencing a local object", Py_None);
        return -1;
    }

    // If the object references another region, it has to be the bridge object
    // and this object needs to be the parent.
    if (!_PyRegion_IsBridge(tgt)) {
        throw_invariant_error(
            src, tgt,
            "Invariant Error: A owned object is referencing a foreign contained object",
            Py_None);
        return -1;
    }

    // This is the owning reference to the target region, but target doesn't know about it
    if (_PyRegion_IsBridge(tgt) && !_PyRegion_IsParent(tgt_region, src_region)) {
        throw_invariant_error(
            src, tgt,
            "Invariant Error: A sub region doesn't know about it's parent",
            Py_None);
        return -1;
    }

    // Update the invariant OSC to check the source region data
    if (_PyRegion_IsBridge(tgt) && _PyRegion_IsOpen(tgt_region)) {
        _Py_region_data *src_data = _Py_region_data_CAST(src_region);
        src_data->invariant_data.osc += 1;
    }

    return 0;
}
static int check_invariant_visit_local(PyObject* tgt, _check_invariant_state* state) {
    PyObject* src = state->src;

    Py_region_t src_region = _PyRegion_Get(src);
    Py_region_t tgt_region = _PyRegion_Get(tgt);

    // This should never happen, since immutable objects have their own visit
    // funciton
    assert(src_region == _Py_LOCAL_REGION);

    // References to static regions are trivially fine
    if (tgt_region == _Py_LOCAL_REGION
        || tgt_region == _Py_IMMUTABLE_REGION
        || tgt_region == _Py_COWN_REGION
    ) {
        return 0;
    }

    _check_invariant_state_track(state, tgt_region);

    _Py_region_data *tgt_data = _Py_region_data_CAST(tgt_region);
    tgt_data->invariant_data.lrc += 1;

    return 0;
}

int _PyOwnership_check_invariant(PyThreadState *tstate) {
    _Py_ownership_state *ownership_state = get_ownership_state();
    if (ownership_state == NULL) {
        return -1;
    }

    // Only run the invariant if it's actully enabled and there is no
    // function which paused the invariant
    if (ownership_state->invariant_state != Py_OWNERSHIP_INVARIANT_ENABLED) {
        return 0;
    }

    // Don't run during shutdown. Python needs to mutate data in this state
    // and any breakage will not really matter, since this universe is at
    // its end.
    if (Py_IsFinalizing()) {
        ownership_state->invariant_state = Py_OWNERSHIP_INVARIANT_DISABLED;
        return 0;
    }

    // Don't stomp existing exceptions
    if (_PyErr_Occurred(tstate)) {
        return 0;
    }

    int result = 0;

    // Use the GC data to find all the objects, and traverse them to
    // confirm all their references satisfy the invariant.
    GCState *gcstate = &tstate->interp->gc;

    _check_invariant_state check_state = {
        .src = NULL,
        .regions = REGIO_SENTINEL_VALUE
    };

    // There is an cyclic doubly linked list per generation of all the objects
    // in that generation.
    for (int i = NUM_GENERATIONS-1; i >= 0; i--) {
        PyGC_Head *containers = GEN_HEAD(gcstate, i);
        PyGC_Head *gc = GC_NEXT(containers);
        // Walk doubly linked list of objects.
        for (; gc != containers; gc = GC_NEXT(gc)) {
            PyObject *ob = FROM_GC(gc);

            // C wrappers are complicated see description of the called
            // function. We treat them as immutable objects. But we
            // don't traverse them.
            if (_PyOwnership_is_c_wrapper(ob)) {
                continue;
            }

            // Prepare the check state
            check_state.src = ob;

            // Select which validation function should be used, based on the
            // current object.
            visitproc visit = NULL;
            if (_Py_IsImmutable(ob)) {
                check_invariant_validate_immutable(ob);
                visit = (visitproc)check_invariant_visit_immutable;
            } else if (!PyRegion_IsLocal(ob)) {
                _check_invariant_state_track(&check_state, _PyRegion_Get(ob));
                visit = (visitproc)check_invariant_visit_owned;
            } else if (PyRegion_IsLocal(ob)) {
                visit = (visitproc)check_invariant_visit_local;
            }

            // Use traverse proceduce to visit each field of the object.
            SUCCEEDS(_PyOwnership_traverse_obj(ob, visit, &check_state));
        }
    }

    SUCCEEDS(validate_check_invariant_state(&check_state));

    goto finally;

error:
    // Disable the invariant
    ownership_state->invariant_state = Py_OWNERSHIP_INVARIANT_DISABLED;
    // Return -1 to indicate an error
    result = -1;

finally:
    clear_check_invariant_state(&check_state);
    return result;
}

int _PyOwnership_invariant_enable(void) {
    _Py_ownership_state *state = get_ownership_state();
    if (state == NULL) {
        return -1;
    }

    if (state->invariant_state == Py_OWNERSHIP_INVARIANT_DISABLED) {
        state->invariant_state = Py_OWNERSHIP_INVARIANT_ENABLED;
    }

    return 0;
}

int _PyOwnership_invariant_disable(void) {
    _Py_ownership_state *state = get_ownership_state();
    if (state == NULL) {
        return -1;
    }

    state->invariant_state = Py_OWNERSHIP_INVARIANT_DISABLED;

    return 0;
}

int _PyOwnership_invariant_pause(void) {
    _Py_ownership_state *state = get_ownership_state();
    if (state == NULL) {
        return -1;
    }

    if (state->invariant_state != Py_OWNERSHIP_INVARIANT_DISABLED) {
        state->invariant_state += 1;
    }

    return 0;
}

int _PyOwnership_invariant_resume(void) {
    _Py_ownership_state *state = get_ownership_state();
    if (state == NULL) {
        return -1;
    }

    if (state->invariant_state != Py_OWNERSHIP_INVARIANT_DISABLED) {
        state->invariant_state -= 1;
    }

    return 0;
}
#endif /* Py_OWNERSHIP_INVARIANT */
