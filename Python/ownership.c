#include "Python.h"
#include <stdbool.h>
#include "object.h"             // _Py_IsImmutable
#include "pycore_descrobject.h" // _PyMethodWrapper_Type
#include "pycore_gc.h"          // _PyGCHead_NEXT, _PyGCHead_PREV, _Py_FROM_GC
#include "pycore_interp.h"      // PyThreadState_Get
#include "pycore_list.h"
#include "pycore_ownership.h"
#include "pycore_pyerrors.h"
#include "pycore_runtime.h"     // _Py_ID
#include "pycore_region.h"      // _PyRegion_Get(), Py_Region
#include "pyerrors.h"
#include "refcount.h"

// Macro that jumps to error, if the expression `x` does not succeed.
#define SUCCEEDS(x) { do { int r = (x); if (r != 0) goto error; } while (0); }

static int init_state(_Py_ownership_state *state)
{
    state->module_locks = NULL;
    state->blocking_on = NULL;

    state->tick = 2;
#ifdef Py_OWNERSHIP_INVARIANT
    state->invariant_state = Py_OWNERSHIP_INVARIANT_DISABLED;
#endif

    return 0;
}

static int init_import_state(_Py_ownership_state *state) {
    PyObject* frozen_importlib = PyImport_ImportModule("_frozen_importlib");
    if (frozen_importlib == NULL) {
        return -1;
    }

    state->module_locks = PyObject_GetAttrString(frozen_importlib, "_module_locks");
    if (state->module_locks == NULL) {
        Py_DECREF(frozen_importlib);
        return -1;
    }

    state->blocking_on = PyObject_GetAttrString(frozen_importlib, "_blocking_on");
    if (state->blocking_on == NULL) {
        Py_DECREF(frozen_importlib);
        return -1;
    }

    Py_DECREF(frozen_importlib);

    // In debug mode, we can store the traceback for debugging purposes.
    // Get a traceback object to use as the ownership location.
#ifdef Py_DEBUG
    PyObject *traceback_module = PyImport_ImportModule("traceback");
    if (traceback_module != NULL) {
        state->traceback_func = PyObject_GetAttrString(traceback_module, "format_stack");
        Py_DECREF(traceback_module);
    }
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
    if (state->tick == 0) {
        if (init_state(state) == -1) {
            PyErr_SetString(PyExc_RuntimeError, "Failed to initialize ownership state");
            return NULL;
        }
    }

    return state;
}

static _Py_ownership_state* get_ownership_state_for_traverse(void)
{
    _Py_ownership_state* state = get_ownership_state();
    if (state == NULL) {
        return NULL;
    }

    if (state->blocking_on == NULL) {
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

int _PyOwnership_notify_untrusted_code(void) {
    _Py_ownership_state* state = get_ownership_state();
    if (state == NULL) {
        return 1;
    }

    // Only incremeant the counter, if the state is trusted
    if (IS_OPEN_REGION_TICK(state->tick)) {
        state->tick += 1;
    }
    assert(!IS_OPEN_REGION_TICK(state->tick));

    // Everything is alright
    return 0;
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

    return _PyList_AppendTakeRef(_PyList_CAST(s), Py_NewRef(item));
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

/**
 * Special function for replacing globals and builtins with a copy of just what they use.
 *
 * This is necessary because the function object has a pointer to the global
 * dictionary, and this is problematic because freezing any function directly
 * (as we do with other objects) would make all globals immutable.
 *
 * Instead, we walk the function and find any places where it references
 * global variables or builtins, and then freeze just those objects. The globals
 * and builtins dictionaries for the function are then replaced with
 * copies containing just those globals and builtins we were able to determine
 * the function uses.
 */
static int shadow_function_globals(PyObject* op)
{
    PyObject* builtins = NULL;
    PyObject* shadow_builtins = NULL;
    PyObject* globals = NULL;
    PyObject* shadow_globals = NULL;
    PyFunctionObject* f = NULL;
    PyObject* f_ptr = NULL;
    PyCodeObject* f_code = NULL;
    Py_ssize_t size;
    bool check_globals = false;

    _PyObject_ASSERT(op, PyFunction_Check(op));

    f = (PyFunctionObject*)op;

    globals = f->func_globals;
    builtins = f->func_builtins;

    f_ptr = f->func_code;

    shadow_builtins = PyDict_New();
    if(shadow_builtins == NULL){
        goto nomemory;
    }

    shadow_globals = PyDict_New();
    if(shadow_globals == NULL){
        goto nomemory;
    }

    if(PyDict_SetItemString(shadow_globals, "__builtins__", shadow_builtins)){
        Py_DECREF(shadow_builtins);
        Py_DECREF(shadow_globals);
        return 0;
    }

    _PyObject_ASSERT(f_ptr, PyCode_Check(f_ptr));
    f_code = (PyCodeObject*)f_ptr;

    size = 0;
    if (f_code->co_names != NULL)
        size = PySequence_Fast_GET_SIZE(f_code->co_names);
    for(Py_ssize_t i = 0; i < size; i++){
        PyObject* name = PySequence_Fast_GET_ITEM(f_code->co_names, i);

        if(PyUnicode_CompareWithASCIIString(name, "globals") == 0){
            // if the code calls the globals() builtin, then any
            // cellvar or const in the function could, potentially, refer to
            // a global variable. As such, we need to check if the globals
            // dictionary contains that key and then make it immutable
            // from this point forwards.
            check_globals = true;
        }

        if(PyDict_Contains(globals, name)){
            PyObject* value = PyDict_GetItem(globals, name);
            if(PyDict_SetItem(shadow_globals, name, value)){
                Py_DECREF(shadow_builtins);
                Py_DECREF(shadow_globals);
                return 0;
            }
        }else if(PyDict_Contains(builtins, name)){
            PyObject* value = PyDict_GetItem(builtins, name);
            if(PyDict_SetItem(shadow_builtins, name, value)){
                Py_DECREF(shadow_builtins);
                Py_DECREF(shadow_globals);
                return 0;
            }
        }
    }

    size = PySequence_Fast_GET_SIZE(f_code->co_consts);
    for(Py_ssize_t i = 0; i < size; i++){
        PyObject* value = PySequence_Fast_GET_ITEM(f_code->co_consts, i);
        if(check_globals && PyUnicode_Check(value)){
            // if the code calls the globals() builtin, then any
            // cellvar or const in the function could, potentially, refer to
            // a global variable. As such, we need to check if the globals
            // dictionary contains that key and then make it immutable
            // from this point forwards.
            PyObject* name = value;
            if(PyDict_Contains(globals, name)){
                value = PyDict_GetItem(globals, name);
                if(PyDict_SetItem(shadow_globals, name, value)){
                    Py_DECREF(shadow_builtins);
                    Py_DECREF(shadow_globals);
                    return 0;
                }
            }
        }
    }

    size = 0;
    if(f->func_closure != NULL){
        size = PyTuple_Size(f->func_closure);
        if(size == -1){
            Py_DECREF(shadow_builtins);
            Py_DECREF(shadow_globals);
            return 0;
        }
    }

    for(Py_ssize_t i=0; i < size; ++i){
        PyObject* cellvar = PyTuple_GET_ITEM(f->func_closure, i);
        PyObject* value = PyCell_GET(cellvar);

        PyObject* shadow_cellvar = PyCell_New(value);
        if(PyTuple_SetItem(f->func_closure, i, shadow_cellvar) == -1){
            Py_DECREF(shadow_cellvar);
            Py_DECREF(shadow_builtins);
            Py_DECREF(shadow_globals);
            return 0;
        }

        if(PyUnicode_Check(value) && check_globals){
            // if the code calls the globals() builtin, then any
            // cellvar or const in the function could, potentially, refer to
            // a global variable. As such, we need to check if the globals
            // dictionary contains that key and then make it immutable
            // from this point forwards.
            PyObject* name = value;
            if(PyDict_Contains(globals, name)){
                value = PyDict_GetItem(globals, name);
                if(PyDict_SetItem(shadow_globals, name, value)){
                    Py_DECREF(shadow_builtins);
                    Py_DECREF(shadow_globals);
                    return 0;
                }
            }
        }
    }

    f->func_globals = shadow_globals;
    Py_DECREF(globals);

    f->func_builtins = shadow_builtins;
    Py_DECREF(builtins);

    if(f->func_annotations == NULL){
        f->func_annotations = PyDict_New();
        if(f->func_annotations == NULL){
            goto nomemory;
        }
    }

    return 0;

nomemory:
    Py_XDECREF(shadow_builtins);
    Py_XDECREF(shadow_globals);
    PyErr_NoMemory();
    return -1;
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
    if (PyType_Check(obj)) {
        // TODO(Immutable): mjp: Special case for types not sure if required. We should review.
        // Additional Note: xFrednet: It looks like each type has a handle to
        // its module, so this is probably needed unless we want to freeze or
        // replace the module pointer?
        PyTypeObject* type = (PyTypeObject*)obj;

        SUCCEEDS(visit(type->tp_dict, data));
        SUCCEEDS(visit(type->tp_mro, data));
        // We need to freeze the tuple object, even though the types
        // within will have been frozen already.
        SUCCEEDS(visit(type->tp_bases, data));
    }
    else
    {
        traverseproc traverse = Py_TYPE(obj)->tp_traverse;
        if(traverse != NULL){
            SUCCEEDS(traverse(obj, visit, data));
        }
    }

    // tp_traverse doesn't cover the object type, this therefore needs
    // to explicitly visit the type.
    SUCCEEDS(visit(_PyObject_CAST(Py_TYPE(obj)), data));

    return 0;
error:
    return -1;
}

/* This prepares the given object to be frozen or moved into a region. The
 * object is then traversed using `_PyOwnership_traverse_obj`.
 */
int _PyOwnership_prep_and_traverse_obj(PyObject* obj, void *data)
{
    if (_PyOwnership_is_c_wrapper(obj)) {
        // C functions are not mutable
        // Types are manually traversed
        return 0;
    }

    // Function require some work to freeze, so we do not freeze the
    // world as they mention globals and builtins.  This will shadow what they
    // use, and then we can freeze the those components.
    if (PyFunction_Check(obj)) {
        SUCCEEDS(shadow_function_globals(obj));
    }

    SUCCEEDS(_PyOwnership_traverse_obj(obj, ownership_visit, data));

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
    if (ownership_state->traceback_func != NULL) {
        PyObject *stack = PyObject_CallFunctionObjArgs(ownership_state->traceback_func, NULL);
        if (stack != NULL) {
            // Add the type name to the top of the stack, can be useful.
            PyObject* typename = PyObject_GetAttrString(_PyObject_CAST(Py_TYPE(obj)), "__name__");
            push(stack, typename);
            location = stack;
        }
    }
#endif

    // Push the current object to the pending stack
    SUCCEEDS(push(traverse_state.dfs_stack, obj));

    // While there is an object in the pending stack, check it
    while(PyList_Size(traverse_state.dfs_stack) != 0){
        PyObject* item = pop(traverse_state.dfs_stack);

        // The `blocking_on` and `mutable_locks` should never be visited
        if (item == ownership_state->blocking_on ||
           item == ownership_state->module_locks
        ) {
            continue;
        }

        switch (caller_check(item, caller_state)) {
            // The object is fine, but shouldn't be traversed
            case Py_OWNERSHIP_TRAVERSE_SKIP:
                continue;

            // The object is okat and should be traversed
            case Py_OWNERSHIP_TRAVERSE_VISIT:
                SUCCEEDS(_PyOwnership_prep_and_traverse_obj(
                    item,
                    (void*)&traverse_state));
                break;

            // An error occured
            default:
                goto error;
        }

#ifdef Py_DEBUG
        if (location != NULL) {
            // Some objects don't have attributes that can be set.
            // As this is a Debug only feature, we could potentially increase the object
            // size to allow this to be stored directly on the object.
            if (PyObject_SetAttrString(item, "__ownership_location__", location) < 0) {
                // Ignore failure to set _freeze_location
                PyErr_Clear();
                // We still want to freeze the object, so we continue
            }
        }
#endif
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

static int check_invariant_validate_immutable(PyObject* obj) {
    // Immutable objects should be in the immutable region
    if (_PyRegion_Get(obj) == _Py_IMMUTABLE_REGION) {
        throw_invariant_error(
            obj, NULL,
            "Invariant Error: Immutable objects should be in the immutable region",
            Py_None);
        return -1;
    }

    return 0;
}

static int check_invariant_visit_immutable(PyObject* tgt, void* src_void) {
    PyObject* src = (PyObject*)src_void;

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

static int check_invariant_visit_owned(PyObject* tgt, void* src_void) {
    PyObject* src = (PyObject*)src_void;

    Py_region_t src_region = _PyRegion_Get(src);
    Py_region_t tgt_region = _PyRegion_Get(tgt);

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
    if (_PyRegion_GetBridge(tgt) != tgt || !_PyRegion_IsParent(tgt_region, src_region)) {
        throw_invariant_error(
            src, tgt,
            "Invariant Error: A owned object is referencing a foreign contained object",
            Py_None);
        return -1;
    }

    return 0;
}

int _PyOwnership_check_invariant(PyThreadState *tstate) {
    _Py_ownership_state *state = get_ownership_state();
    if (state == NULL) {
        return -1;
    }

    // Only run the invariant if it's actully enabled and there is no
    // function which paused the invariant
    if (state->invariant_state != Py_OWNERSHIP_INVARIANT_ENABLED) {
        return 0;
    }

    // Don't run during shutdown. Python needs to mutate data in this state
    // and any breakage will not really matter, since this universe is at
    // its end.
    if (Py_IsFinalizing()) {
        state->invariant_state = Py_OWNERSHIP_INVARIANT_DISABLED;
        return 0;
    }

    // Don't stomp existing exceptions
    if (_PyErr_Occurred(tstate)) {
        return 0;
    }

    // Use the GC data to find all the objects, and traverse them to
    // confirm all their references satisfy the invariant.
    GCState *gcstate = &tstate->interp->gc;

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

            // Select which validation function should be used, based on the
            // current object.
            visitproc visit = NULL;
            if (_Py_IsImmutable(ob)) {
                check_invariant_validate_immutable(ob);
                visit = (visitproc)check_invariant_visit_immutable;
            } else if (!_Py_IsLocal(ob)) {
                visit = (visitproc)check_invariant_visit_owned;
            } else if (_Py_IsLocal(ob)) {
                // Mutable objects are allowed to reference all other objects
                // (regardless if mutable or not). These therefore don't need
                // to be traversed.
                continue;
            }

            // Use traverse proceduce to visit each field of the object.
            SUCCEEDS(_PyOwnership_traverse_obj(ob, visit, ob));
        }
    }

    return 0;

error:
    // Disable the invariant
    state->invariant_state = Py_OWNERSHIP_INVARIANT_DISABLED;
    // Return -1 to indicate an error
    return -1;
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
