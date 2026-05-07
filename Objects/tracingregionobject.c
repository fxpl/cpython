#include "Python.h"
#include "pycore_interp.h"
#include "pycore_gc.h"            // _PyObject_GC_IS_TRACKED()
#include "pycore_object.h"        // _PyObject_GC_TRACK(), _PyDebugAllocatorStats()
#include "pycore_descrobject.h"
#include "pycore_weakref.h"

// #define REGION_TRACING

#ifdef REGION_TRACING
#define if_trace(...) __VA_ARGS__
#define trace_arg(arg) , (Py_uintptr_t)(arg)
#define trace(msg, ...) \
    do { \
        printf(msg "\n" __VA_OPT__(,) __VA_ARGS__); \
    } while(0)
#else
#define if_trace(...)
#define trace_arg(...)
#define trace(...)
#endif

/* Macro that jumps to error, if the expression `x` does not succeed. */
#define SUCCEEDS(x) do { int r = (x); if (r != 0) goto error; } while (0)

// ###################################################################
// Copied from gc.c
// ###################################################################

#ifndef Py_GIL_DISABLED
#define GC_NEXT _PyGCHead_NEXT
#define GC_PREV _PyGCHead_PREV

static inline void
gc_set_old_space(PyGC_Head *g, int space)
{
    assert(space == 0 || space == _PyGC_NEXT_MASK_OLD_SPACE_1);
    g->_gc_next &= ~_PyGC_NEXT_MASK_OLD_SPACE_1;
    g->_gc_next |= space;
}

static inline void
gc_list_init(PyGC_Head *list)
{
    // List header must not have flags.
    // We can assign pointer by simple cast.
    list->_gc_prev = (uintptr_t)list;
    list->_gc_next = (uintptr_t)list;
}

static void
gc_list_move(PyGC_Head *node, PyGC_Head *list)
{
    /* Unlink from current list. */
    PyGC_Head *from_prev = GC_PREV(node);
    PyGC_Head *from_next = GC_NEXT(node);
    _PyGCHead_SET_NEXT(from_prev, from_next);
    _PyGCHead_SET_PREV(from_next, from_prev);

    /* Relink at end of new list. */
    // list must not have flags.  So we can skip macros.
    PyGC_Head *to_prev = (PyGC_Head*)list->_gc_prev;
    _PyGCHead_SET_PREV(node, to_prev);
    _PyGCHead_SET_NEXT(to_prev, node);
    list->_gc_prev = (uintptr_t)node;
    _PyGCHead_SET_NEXT(node, list);
}

static inline int
gc_list_is_empty(PyGC_Head *list)
{
    return (list->_gc_next == (uintptr_t)list);
}

static void
gc_list_merge(PyGC_Head *from, PyGC_Head *to)
{
    assert(from != to);
    if (!gc_list_is_empty(from)) {
        PyGC_Head *to_tail = GC_PREV(to);
        PyGC_Head *from_head = GC_NEXT(from);
        PyGC_Head *from_tail = GC_PREV(from);
        assert(from_head != from);
        assert(from_tail != from);

        _PyGCHead_SET_NEXT(to_tail, from_head);
        _PyGCHead_SET_PREV(from_head, to_tail);

        _PyGCHead_SET_NEXT(from_tail, to);
        _PyGCHead_SET_PREV(to, from_tail);
    }
    gc_list_init(from);
}

static struct _gc_runtime_state*
get_gc_state(void)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    return &interp->gc;
}

static inline void
gc_clear_collecting(PyGC_Head *g)
{
    g->_gc_prev &= ~_PyGC_PREV_MASK_COLLECTING;
}

#elif // Py_GIL_DISABLED
#error "We need GIL"
#endif

// ###################################################################
// Copied from regions-main
// ###################################################################

static PyObject* list_pop(PyObject* s){
    PyObject* item;
    Py_ssize_t size = PyList_Size(s);
    if(size == 0){
        return NULL;
    }
    item = PyList_GetItem(s, size - 1);
    if(item == NULL){
        return NULL;
    }
    // This should never fail, since we shrink the size
    if(PyList_SetSlice(s, size - 1, size, NULL)){
        Py_DECREF(item);
        return NULL;
    }
    return item;
}

typedef enum {
    Py_MOVABLE_YES = 0,
    Py_MOVABLE_NO = 1,
    Py_MOVABLE_FREEZE = 2,
} movable_status;

movable_status get_movable_status(PyObject *obj) {
    // FIXME(regions): xFrednet: Currently it's not possible to set
    // the movability per object. This instead returns the default
    // movability for objects. Note that some shallow immutable objects
    // will not return freeze as their movability.

    // Immortal object have no real RC, this makes it infeasible to have them
    // in a region and dynamically track their ownership. Immortal objects are
    // intended to be immutable in Python, so it should be safe to implicitly
    // freeze them.
    if (_Py_IsImmortal(obj)) {
        return Py_MOVABLE_FREEZE;
    }

    // Immutable objects don't need to be moved
    if (_Py_IsImmutable(obj)) {
        return Py_MOVABLE_FREEZE;
    }

    // Types are a pain for regions since it's likely that objects of one type may
    // end up in multiple regions, requiring the type to be frozen. Types also
    // have a lot of reference pointing to them. Let's hope there is no need to
    // keep them freezable
    if (PyType_Check(obj)) {
        return Py_MOVABLE_FREEZE;
    }

    // Module objects are also complicated. Freezing them should turn most modules
    // into proxys which should make them mostly usable.
    if (PyModule_Check(obj)) {
        return Py_MOVABLE_FREEZE;
    }

    // Functions are a mess as well, making the entire system reachable. Freezing
    // them should again just magically make most things work
    if (PyFunction_Check(obj)) {
        return Py_MOVABLE_FREEZE;
    }

    // CWrappers can't really be owned, but need some special handling since
    // interpreters could still race on their RC. Solution, throw them in the
    // freezer
    if (PyCFunction_Check(obj)
        || Py_IS_TYPE(obj, &_PyMethodWrapper_Type)
        || Py_IS_TYPE(obj, &PyWrapperDescr_Type)
    ) {
        return Py_MOVABLE_FREEZE;
    }

    // Freezing or moving these objects is... complicated. In some cases it is
    // possible but more hassle than it's probably worth. For not we mark them
    // all as unmovable.
    if (PyFrame_Check(obj)
        || PyGen_CheckExact(obj)
        || PyCoro_CheckExact(obj)
        || PyAsyncGen_CheckExact(obj)
        || PyAsyncGenASend_CheckExact(obj)
    ) {
        return Py_MOVABLE_NO;
    }

    // Exceptions don't hold anything obviously problematic preventing them
    // from being moved into a region. The actual problem is that the runtime
    // stores references to them and that these are already emitted on an
    // error path. Moving them into a region could add more problems.
    // We should discuss how to handle these, maybe freezing is the correct
    // approach?
    if (PyExceptionInstance_Check(obj)) {
        return Py_MOVABLE_NO;
    }

    // For now, we define all other objects as movable by default. (Surely
    // this will not backfire)
    return Py_MOVABLE_YES;
}

// This uses the given arguments to create and throw a `RegionError`
static void throw_region_error(
    const char *format_str, const char *tp_name,
    PyObject* src, PyObject* tgt)
{
    // Don't stomp existing exception
    PyThreadState *tstate = PyThreadState_Get();
    if (_PyErr_Occurred(tstate)) {
        return;
    }

    PyErr_Format(PyExc_RuntimeError, format_str, tp_name);

    // Set source and target fields
    // Get the current exception (should be a RuntimeError)
    PyObject *exc = PyErr_GetRaisedException();
    assert(exc && PyObject_TypeCheck(exc, (PyTypeObject *)PyExc_RuntimeError));

    // Add 'source' and 'target' attributes to the exception
    PyObject_SetAttr(exc, &_Py_ID(source), src ? src : Py_None);
    PyObject_SetAttr(exc, &_Py_ID(target), tgt ? tgt : Py_None);

    PyErr_SetRaisedException((PyObject*)exc);
}

// Wrapper around tp_traverse that also visits the type object.
static int
traverse_via_tp_traverse(PyObject *obj, visitproc visit, void *state)
{
    PyTypeObject *tp = Py_TYPE(obj);

    // Visit the type with traverse
    traverseproc traverse = tp->tp_traverse;
    if (traverse != NULL) {
        int err = traverse(obj, visit, state);
        if (err) {
            return err;
        }
    }


    // Most `tp_traverse` don't visit the type even though they should.
    // Here it won't hurt to potentially visit it twice, since types
    // are non-movable but will be frozen.
    return visit((PyObject *)Py_TYPE(obj), state);
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

    if (tp->tp_traverse != NULL) {
        PySys_FormatStderr(
            "regions: type '%.100s' has tp_traverse but no tp_reachable\n",
            tp->tp_name);
    } else {
        PySys_FormatStderr(
            "regions: type '%.100s' has no tp_traverse and no tp_reachable\n",
            tp->tp_name);
    }

    // Always return the wrapper; even when tp_traverse is NULL, the wrapper
    // will still visit the type object which tp_reachable is expected to do.
    return traverse_via_tp_traverse;
}

// ###################################################################
// Tracing Impl
// ###################################################################

static void
gc_list_dissolve(PyGC_Head *list) {
    struct _gc_runtime_state* gc_state = get_gc_state();
    //gc_list_merge(list, &(gc_state->young.head));
    gc_list_merge(list, &(gc_state->old[0].head));
}

typedef struct {
    /// A list of all visited objects
    _Py_hashtable_t *visited;
    /// The number of refs coming into this object graph
    Py_ssize_t external_rc;
    // This is set if an object was frozen and the trace needs to restart to be valid
    bool restart;
    // The GC list used for this trace
    PyGC_Head* gc_list;
    // The source of the reference, this is used for error reporting
    PyObject *src;
    // List of pending objects that are not GC
    PyObject *pending;
} trace_state;

static void trace_state_destroy(trace_state* state) {
    if (state->visited) {
        _Py_hashtable_destroy(state->visited);
        state->visited = NULL;
    }
    if (state->pending) {
        Py_DECREF(state->pending);
        state->pending = NULL;
    }
}
static int trace_state_init(trace_state* state, PyGC_Head *gc_list) {
    assert(gc_list == NULL || gc_list_is_empty(gc_list));

    state->visited = NULL;
    state->pending = NULL;

    state->visited = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if (state->visited == NULL) {
        goto error;
    }

    state->pending = PyList_New(0);
    if (state->pending == NULL) {
        goto error;
    }

    state->external_rc = 0;
    state->restart = false;
    state->gc_list = gc_list;
    state->src = NULL;

    return 0;
error:
    trace_state_destroy(state);
    return -1;
}

typedef struct {
    Py_ssize_t objs;
    Py_ssize_t incoming_refs;
} trace_result;

const int TRACE_RES_ERR = -1;
const int TRACE_RES_DONE = 0;
const int TRACE_RES_RESTART = 1;

static int _move_obj(PyObject* obj, trace_state* state) {
    // Check the movability of the object:
    movable_status status = get_movable_status(obj);
    switch (status) {
    case Py_MOVABLE_YES:
        break;
    case Py_MOVABLE_NO:
        trace("    - %p is not movable", obj);
        throw_region_error(
            "Instances of type '%s' are not movable", Py_TYPE(obj)->tp_name,
            state->src, obj);
        return TRACE_RES_ERR;
    case Py_MOVABLE_FREEZE:
        // Freeze the object, this can invalidate our `external_rc`,
        // we restart after this trace
        trace("    - freezing %p", obj);
        if (_PyImmutability_Freeze(obj)) {
            return TRACE_RES_ERR;
        }

        state->restart = true;
        // Setting the gc_list to NULL will stop objects from being moved
        // between GC lists. Just a small thing we can avoid. The next (full)
        // trace will have this set again.
        state->gc_list = NULL;
        return 0;
    default:
        assert(false);
        break;
    }

    // Move the object
    Py_ssize_t lrc_change = Py_REFCNT(obj);
    if (state->src != NULL) {
        // -1 for the reference we just followed
        lrc_change -= 1;
    }
    trace("    - moving %p; LRC += %zd", obj, lrc_change);
    state->external_rc += lrc_change;

    if (_Py_hashtable_set(state->visited, obj, obj) == -1) {
        return -1;
    }

    // This moves the object into the region list, if provided.
    if (state->gc_list && PyObject_IS_GC(obj) && PyObject_GC_IsTracked(obj)) {
        // This flag may be set if the region is constructed as part of
        // a finalizer. If the flag remains set, for an object removed
        // from its GC list bad things can happen.
        gc_clear_collecting(_Py_AS_GC(obj));
        // Clearing the space flag makes it easy to merge this list back
        // into the local GC lists
        gc_set_old_space(_Py_AS_GC(obj), 0);
        gc_list_move(_Py_AS_GC(obj), state->gc_list);
    }

    if (PyList_Append(state->pending, obj)) {
        return -1;
    }

    return 0;
}

static int _trace_visit(PyObject* obj, trace_state* state) {
    // References to immutable objects are allowed
    if (_PyImmutability_CanViewAsImmutable(obj)) {
        assert(_Py_IsImmutable(obj));
        return 0;
    }

    // Check if the object is already part of the region
    if (_Py_hashtable_get(state->visited, (void*)obj)) {
        trace("    - Internal reference to %p; LRC -= 1", obj);
        state->external_rc -= 1;
        return 0;
    }

    return _move_obj(obj, state);
}

static int _trace_once(PyObject* obj, trace_result* result, PyGC_Head *gc_list) {
    trace("  - starting trace from %p", obj);
    int res = TRACE_RES_DONE;

    // init the trace state
    trace_state state;
    if (trace_state_init(&state, gc_list)) {
        return TRACE_RES_ERR;
    }

    SUCCEEDS(_move_obj(obj, &state));

    while (PyList_GET_SIZE(state.pending) > 0) {
        // Find the next pending item:
        PyObject *item = list_pop(state.pending);

        // Traverse item
        state.src = item;
        trace("  - traversing %p", item);
        traverseproc proc = get_reachable_proc(Py_TYPE(item));
        SUCCEEDS(proc(item, (visitproc)_trace_visit, (void*)&state));

        // Weak refs need special handling
        assert(!PyWeakref_Check(item));
    }

    if (state.restart) {
        res = TRACE_RES_RESTART;
    }

    goto finally;
error:
    res = TRACE_RES_ERR;
finally:
    result->incoming_refs = state.external_rc;
    result->objs = _Py_hashtable_len(state.visited);
    trace_state_destroy(&state);
    return res;
}

static int trace_object(PyObject* obj, trace_result* result, PyGC_Head *gc_list) {
    const int TRIES = 2;
    trace("Starting trace for %p", obj);
    for (int i = 0; i < TRIES; i++) {
        // Reset trace
        result->objs = 0;
        result->incoming_refs = 0;

        // Trace object
        int res = _trace_once(obj, result, gc_list);

        // Restart trace on demand
        if (res == TRACE_RES_RESTART) {
            trace("- restarting trace for %p", obj);
            if (gc_list != NULL) {
                gc_list_dissolve(gc_list);
                assert(gc_list_is_empty(gc_list));
            }
            continue;
        }
        return res;
    }

    return TRACE_RES_DONE;
}

static void detach_weak_refs(PyGC_Head *gc_list) {
    PyGC_Head *current = GC_NEXT(gc_list);
    while (current != gc_list) {
        PyObject *item = _Py_FROM_GC(current);
#ifdef PY_DEBUG
        Py_ssize_t weak_ctn = _PyWeakref_GetWeakrefCount(item);
        if (weak_ctn) {
            trace("- Clearing %zd weak references to %p", weak_ctn, item);
        }
#endif
        if (_PyType_SUPPORTS_WEAKREFS(Py_TYPE(item))) {
            _PyWeakref_ClearWeakRefsNoCallbacks(item);
        }

        current = GC_NEXT(current);
    }
}

// ###################################################################
// Region Object
// ###################################################################

typedef struct {
    PyObject_HEAD
    PyObject *dict;
    // The GC list containing all objects, used during transfer
    PyGC_Head gc_list;
} TracingRegionObject;

static int
TracingRegion_init(TracingRegionObject *self, PyObject *args, PyObject *kwargs) {
    gc_list_init(&self->gc_list);
    return 0;
}

static int
TracingRegion_traverse(TracingRegionObject *self, visitproc visit, void *arg) {
    Py_VISIT(self->dict);
    return 0;
}

static int
TracingRegion_clear(TracingRegionObject *self) {
    // This is deallocating a closed region, we just dissolve it
    if (!gc_list_is_empty(&self->gc_list)) {
        gc_list_dissolve(&self->gc_list);
    }
    Py_CLEAR(self->dict);
    return 0;
}

static void
TracingRegion_dealloc(TracingRegionObject *self) {
    PyObject_GC_UnTrack(self);
    TracingRegion_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject* TracingRegion_trace(PyObject *op) {
    trace_result result;
    if (trace_object(op, &result, NULL)) {
        return NULL;  // propagate Python exception
    }

    PyObject *t = Py_BuildValue("(ii)", result.objs, result.incoming_refs);
    if (t == NULL) {
        return NULL;  // propagate Python exception
    }

    return t;
}

/* This method traces the region and closes it if the caller has the only
 * owning reference into the graph. The reference passed into this function
 * needs to be borrowed.
 *
 * This function requires the GIL to be held.
 *
 * Returns -1 if an exception was raised. 0 if the region couldn't be closed
 * and 1 if the region was closed.
 */
int _PyTracingRegion_Close(PyObject* op) {
    TracingRegionObject *self = (TracingRegionObject*)op;
    assert(gc_list_is_empty(&self->gc_list));

    trace_result result;
    if (trace_object(op, &result, &self->gc_list)) {
        return -1;  // propagate Python exception
    }

    // Keep the region open, if the there are more incoming references
    // besides the expected owning one
    if (result.incoming_refs > 1) {
        trace("- Failed to close region %p, there are %zd incoming references", self, result.incoming_refs);
        gc_list_dissolve(&self->gc_list);
        assert(gc_list_is_empty(&self->gc_list));
        return 0;
    }

    // FIXME: This can be optimized, for example by inserting all objects
    // with weak refs in the beginning.
    detach_weak_refs(&self->gc_list);

    trace("- Closed region %p", self);
    assert(!gc_list_is_empty(&self->gc_list));
    return 1;
}

/* This method opens the region by dissolving it and all objects into the
 * local GC list.
 *
 * This function requires the GIL to be held.
 */
int _PyTracingRegion_Open(PyObject* op) {
    TracingRegionObject *self = (TracingRegionObject*)op;
    assert(!gc_list_is_empty(&self->gc_list));
    gc_list_dissolve(&self->gc_list);
    assert(gc_list_is_empty(&self->gc_list));
    return 0;
}

static PyMethodDef TracingRegion_methods[] = {
    {"trace", _PyCFunction_CAST(TracingRegion_trace), METH_NOARGS,
        "This traces the region and returns the number of incoming references"},
    {NULL,              NULL}           /* sentinel */
};

static PyMemberDef TracingRegion_members[] = {
    {"__dict__", _Py_T_OBJECT, offsetof(TracingRegionObject, dict), Py_READONLY},
    {NULL}
};

PyTypeObject _PyTracingRegion_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "TracingRegion",
    .tp_basicsize = sizeof(TracingRegionObject),
    .tp_dealloc = (destructor)TracingRegion_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .tp_traverse = (traverseproc)TracingRegion_traverse,
    .tp_clear = (inquiry)TracingRegion_clear,
    .tp_members = TracingRegion_members,
    .tp_methods = TracingRegion_methods,
    .tp_dictoffset = offsetof(TracingRegionObject, dict),
    .tp_init = (initproc)TracingRegion_init,
    .tp_new = PyType_GenericNew,
    .tp_reachable = _PyObject_ReachableVisitTypeAndTraverse,
};

// TODO: Weak-references part of the trace are not handled
