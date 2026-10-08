#include "Python.h"
#include "pycore_interp.h"
#include "pycore_gc.h"            // _PyObject_GC_IS_TRACKED()
#include "pycore_dict.h"          // _PyObject_MaterializeManagedDict()
#include "pycore_object.h"        // _PyObject_GC_TRACK(), _PyDebugAllocatorStats()
#include "pycore_descrobject.h"
#include "pycore_modsupport.h"    // _PyArg_NoPositional()
#include "pycore_weakref.h"
#include "pycore_cown.h"
#include "pycore_regionref.h"
#ifdef Py_GIL_DISABLED
#include "pycore_parking_lot.h"   // _PyParkingLot_Park()
#include "pycore_time.h"          // _PyDeadline_Init()
#endif

#define ERROR_OBJECT_REPORT_COUNT 5
#define ERROR_MERMAID_REPORT_LIMIT 50
#define ERROR_MERMAID_HIDE_IMMUTABLE true

/* Set this to the path of the file that a failed close should write its mermaid
 * graph to. The graph is not written when the variable is unset or empty. */
#define REGION_GRAPH_ENV_VAR "PYTHON_REGION_GRAPH"

// #define REGION_TRACING

#ifdef REGION_TRACING
#define dbg(msg, ...) \
    do { \
        printf(msg "\n" __VA_OPT__(,) __VA_ARGS__); \
    } while(0)
#else
#define dbg(...)
#endif

/* Macro that jumps to error, if the expression `x` does not succeed. */
#define SUCCEEDS(x) do { int r = (x); if (r != 0) goto error; } while (0)

#define Region_Check(x) Py_IS_TYPE((x), &_PyTracingRegion_Type)
#define Cown_Check(x) Py_IS_TYPE((x), &_PyCown_Type)

// ###################################################################
// Copied from gc.c
// ###################################################################

#pragma region Copies from gc.c
#ifndef Py_GIL_DISABLED
#define GC_NEXT _PyGCHead_NEXT
#define GC_PREV _PyGCHead_PREV

static inline int
gc_old_space(PyGC_Head *g)
{
    return g->_gc_next & _PyGC_NEXT_MASK_OLD_SPACE_1;
}

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
        assert(gc_list_is_empty(to) ||
            gc_old_space(to_tail) == gc_old_space(from_tail));

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

#endif
#pragma endregion // Copies from gc.c

// ###################################################################
// Copied from regions-main
// ###################################################################

/* Removes the last item of the list and returns it as a new reference.
 *
 * The caller needs a reference of its own, since the list was the only thing
 * keeping the item alive. Traversing the item can run arbitrary code, for
 * example through `_PyImmutability_Freeze()`, which could otherwise deallocate
 * it while it is being traversed.
 *
 * Returns NULL with an exception set on failure. The list must not be empty.
 */
static PyObject* list_pop(PyObject* s){
    Py_ssize_t size = PyList_GET_SIZE(s);
    assert(size > 0);

    PyObject *item = Py_NewRef(PyList_GET_ITEM(s, size - 1));
    // This should never fail, since we shrink the size
    if (PyList_SetSlice(s, size - 1, size, NULL)) {
        Py_DECREF(item);
        return NULL;
    }
    return item;
}

// Removes slot i, discarding its item; swaps in the last element, so order is
// not preserved. Caller must own the only reference to the list.
static void list_remove(PyObject *list, Py_ssize_t i)
{
    Py_ssize_t last = PyList_GET_SIZE(list) - 1;
    PyObject *removed = PyList_GET_ITEM(list, i);
    PyList_SET_ITEM(list, i, PyList_GET_ITEM(list, last));
    PyList_SET_ITEM(list, last, NULL);
    Py_SET_SIZE(list, last);
    Py_DECREF(removed);
}

typedef enum {
    Py_MOVABLE_YES = 0,
    Py_MOVABLE_NO = 1,
    // The object should be frozen
    Py_MOVABLE_FREEZE = 2,
    // The object is not movable, but the reference is allowed. The object
    // should be skipped
    Py_MOVABLE_COWN = 3,
} movable_status_t;

static movable_status_t get_movable_status(PyObject *obj) {
    // FIXME(regions): xFrednet: Currently it's not possible to set
    // the movability per object. This instead returns the default
    // movability for objects. Note that some shallow immutable objects
    // will not return freeze as their movability.

    // References to immutable objects are allowed
    if (_Py_IsDeepImmutable(obj)) {
        return Py_MOVABLE_COWN;
    }

    // Immortal object have no real RC, this makes it infeasible to have them
    // in a region and dynamically track their ownership. Immortal objects are
    // intended to be immutable in Python, so it should be safe to implicitly
    // freeze them.
    if (_Py_IsImmortal(obj)) {
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
    // into proxies which should make them mostly usable.
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

    // Cowns are not movable, but the reference is explicitly allowed.
    if (Cown_Check(obj)) {
#ifdef _Py_PYRONA_INTERPRETER_SHARING
        assert(_Py_NeedsAtomicRC(obj));
#endif
        return Py_MOVABLE_COWN;
    }

    // Freezing or moving these objects is... complicated. In some cases it is
    // possible but more hassle than it's probably worth. For now we mark them
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

    // Regions are theoretically only movable, if they're closed. The traversal
    // checks this manually.

    // For now, we define all other objects as movable by default. (Surely
    // this will not backfire)
    return Py_MOVABLE_YES;
}

// This uses the given arguments to create and throw a `RuntimeError`
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

    PyObject *exc = PyErr_GetRaisedException();
    assert(exc != NULL);

    // Failing to attach it must not replace the error raised above.
    if (PyObject_SetAttr(exc, &_Py_ID(source), src ? src : Py_None) < 0
        || PyObject_SetAttr(exc, &_Py_ID(target), tgt ? tgt : Py_None) < 0)
    {
        PyErr_Clear();
    }

    PyErr_SetRaisedException(exc);
}

// ###################################################################
// Tracing Impl
// ###################################################################

#ifdef _Py_PYRONA_INTERPRETER_SHARING
static void
gc_list_dissolve(PyGC_Head *list) {
    struct _gc_runtime_state* gc_state = get_gc_state();
    gc_list_merge(list, &(gc_state->old[0].head));
}
#endif

typedef struct {
    // The weak references that live inside the region and therefore survive the
    // close. NULL when the trace did not find any.
    _Py_hashtable_t *keep;
    PyObject *region;
} detach_weak_refs_state_t;

static int
detach_weak_refs_visit(_Py_hashtable_t *ht, const void *key, const void *value, void *user_data)
{
    PyObject *item = (PyObject *)key;
    if (!_PyType_SUPPORTS_WEAKREFS(Py_TYPE(item))) {
        return 0;
    }
    detach_weak_refs_state_t *state = (detach_weak_refs_state_t *)user_data;

#ifdef Py_DEBUG
    Py_ssize_t weak_ctn = _PyWeakref_GetWeakrefCount(item);
    if (weak_ctn) {
        dbg("- Clearing %zd weak references to %p", weak_ctn, item);
    }
#endif
    _PyRegionRef_CloseWeakRefs(item, state->keep, state->region);
    return 0;
}

/* Detaches all weak references pointing to objects inside the region, and
 * re-homes the region references, which are meant to survive the close.
 *
 * This walks the set of traced objects instead of the region's GC list, since
 * objects that are not tracked by the GC never enter that list. Missing one
 * would leave a live weak reference pointing into the closed region, which is
 * enough for external code to read and mutate its contents.
 *
 * Re-homing rides along on this walk on purpose. Finding the references into a
 * region means looking at every member's weakref list, which is exactly what
 * this already does.
 */
static void detach_weak_refs(
    PyObject *region, _Py_hashtable_t *visited, bool has_weak_refs)
{
    detach_weak_refs_state_t state = {
        .keep = has_weak_refs ? visited : NULL,
        .region = region,
    };
    // `detach_weak_refs_visit()` never fails, so the result can be ignored.
    (void)_Py_hashtable_foreach(visited, detach_weak_refs_visit, &state);
}

typedef enum {
    // The region is closed
    Py_REGION_STATUS_CLOSED = 0,
    // The region is open
    Py_REGION_STATUS_OPEN = 1,
    // A trace is in progress. Any access attempts should mark the trace as failed.
    Py_REGION_STATUS_TRACING = 2,
    // A trace was invalidated by a concurrent thread. The failing trace hasn't concluded yet.
    Py_REGION_STATUS_TRACING_FAILED = 3,
} region_state_t;

typedef struct {
    PyObject_HEAD
    PyObject *dict;
#ifdef _Py_PYRONA_INTERPRETER_SHARING
    // The GC list containing all objects while the region is closed. The bridge
    // object is not in this GC list but in the list of the owning region or in no
    // list if it's owned by a released cown.
    PyGC_Head gc_list;
#endif
    // All objects that belong to a closed region are in the `gc_list` above. This
    // removes them from the local GC and allows this region to be moved between
    // sub-interpreters, but it would prevent the collection of closed regions with
    // internal references to the bridge. On closed regions, we therefore manually
    // subtract internal references from the RC. We basically hide the cycles, until
    // the region is open. This is the number of references subtracted from the rc.
    // These are readded in the constructor or when opening the region.
    Py_ssize_t internal_bridge_refs;
    // Indicates if the region is open or closed. Additional values are used
    // to track active traces and detect concurrent accesses.
    int32_t state;
    // The node every region reference into this region resolves through, or
    // NULL when nothing points into it. Only closed regions can have a meta,
    // opening restamps it
    _PyRegionRefMetadata *meta;
} TracingRegionObject;

#define _PyTRegion_CAST(x) _Py_CAST(TracingRegionObject*, x)

static bool region_is_open_lock_held(TracingRegionObject *self) {
    return self->state >= Py_REGION_STATUS_OPEN;
}

static bool region_is_open(TracingRegionObject *self) {
    bool res;
    Py_BEGIN_CRITICAL_SECTION(self);
    res = region_is_open_lock_held(self);
    Py_END_CRITICAL_SECTION();
    return res;
}

static int region_set_tracing_mark(TracingRegionObject *self) {
    int res = 0;
    Py_BEGIN_CRITICAL_SECTION(self);
    if (self->state == Py_REGION_STATUS_CLOSED) {
        res = 1;
    } else if (self->state == Py_REGION_STATUS_OPEN) {
        self->state = Py_REGION_STATUS_TRACING;
    } else if (self->state == Py_REGION_STATUS_TRACING) {
        // Someone else is tracing the region, mark it as failed.
        self->state = Py_REGION_STATUS_TRACING_FAILED;
        res = -1;
    }
    Py_END_CRITICAL_SECTION();
    return res;
}

static int region_clear_tracing_mark(TracingRegionObject *self) {
    int res = 0;
    Py_BEGIN_CRITICAL_SECTION(self);
    assert(self->state != Py_REGION_STATUS_CLOSED && self->state != Py_REGION_STATUS_OPEN);
    self->state = Py_REGION_STATUS_OPEN;
    Py_END_CRITICAL_SECTION();
    return res;
}

/* Returns this region's node, allocating it if this is the first reference the
 * current close has found. Borrowed. The caller must hold `_PyWeakref_Lock`. */
static _PyRegionRefMetadata *
region_meta_lock_held(TracingRegionObject *self)
{
    if (self->meta == NULL) {
        self->meta = _PyRegionRef_NewRegionMetaLockHeld(_PyObject_CAST(self));
    }
    return self->meta;
}

static void
region_meta_release_lock_held(TracingRegionObject *self)
{
    if (self->meta != NULL) {
        _PyRegionRef_MetaRegionOpened(self->meta);
        _PyRegionRef_MetaDecref(self->meta);
        self->meta = NULL;
    }
}

/* Hands the node over to the references still holding it: it becomes local to
 * this interpreter with nothing left to open. Used whenever a region stops
 * being closed, including when its contents are being deleted. */
static void
region_meta_release(TracingRegionObject *self)
{
    Py_BEGIN_CRITICAL_SECTION(self);
    region_meta_release_lock_held(self);
    Py_END_CRITICAL_SECTION();
}

static int _region_close_lock_held(
    TracingRegionObject *self,
    Py_ssize_t bridge_rc,
    _Py_hashtable_t *visited,
    bool has_weak_refs
) {
    if (self->state == Py_REGION_STATUS_TRACING_FAILED) {
        PyErr_Format(
            PyExc_RuntimeError,
            "the region cannot be closed since a concurrent thread accessed during tracing");
        return -1;
    }

    dbg("Closing region %p", self);

    detach_weak_refs(_PyObject_CAST(self), visited, has_weak_refs);

    // See comment on `self->internal_bridge_refs`
    if (bridge_rc != 0) {
        assert(bridge_rc >= 0);
#ifdef _Py_PYRONA_INTERPRETER_SHARING
        dbg("- subtracting %zd internal references from the bridge object %p", bridge_rc, self);
        _Py_RefcntAdd(self, -bridge_rc);
#endif
        self->internal_bridge_refs = bridge_rc;
    } else {
        assert(self->internal_bridge_refs == 0);
    }

    assert(self->state == Py_REGION_STATUS_TRACING);
    self->state = Py_REGION_STATUS_CLOSED;
    return 0;
}

static int _region_close(
    TracingRegionObject *self,
    Py_ssize_t bridge_rc,
    _Py_hashtable_t *visited,
    bool has_weak_refs
) {
    int result;
    Py_BEGIN_CRITICAL_SECTION(self);
    result = _region_close_lock_held(self, bridge_rc, visited, has_weak_refs);
    Py_END_CRITICAL_SECTION();
    return result;
}

#ifdef _Py_PYRONA_INTERPRETER_SHARING
/* Re-adds the references to the bridge object that `_region_close()` subtracted.
 *
 * Note that this may resurrect the bridge object. Callers may need to handle this case.
 */
static void _restore_internal_bridge_refs(TracingRegionObject *self) {
    if (self->internal_bridge_refs != 0) {
        assert(self->internal_bridge_refs >= 0);
        dbg("- adding %zd internal references from the bridge object %p", self->internal_bridge_refs, self);
        _Py_RefcntAdd(self, self->internal_bridge_refs);
        self->internal_bridge_refs = 0;
    }
}
#endif

static void _open_region_lock_held(TracingRegionObject *self) {
    if (self->state >= Py_REGION_STATUS_OPEN) {
        if (self->state == Py_REGION_STATUS_TRACING) {
            self->state = Py_REGION_STATUS_TRACING_FAILED;
        }
        return;
    }

    dbg("Opening region %p", self);

    region_meta_release_lock_held(self);

#ifdef _Py_PYRONA_INTERPRETER_SHARING
    _restore_internal_bridge_refs(self);

    // This only dissolves this region, all sub-regions remain closed.
    gc_list_dissolve(&self->gc_list);
    assert(gc_list_is_empty(&self->gc_list));
#endif

    self->state = Py_REGION_STATUS_OPEN;
}

static void _open_region(TracingRegionObject *self) {
    Py_BEGIN_CRITICAL_SECTION(self);
    _open_region_lock_held(self);
    Py_END_CRITICAL_SECTION();
}

#define PER_REGION_TRACE_LIMIT 2

#ifdef Py_GIL_DISABLED
// How long a tree close waits for another thread to merge the refcounts of a
// non-local object before giving up.
#define NON_LOCAL_MERGE_TIMEOUT_MS 1000
#endif

// ###################################################################
// Tree Trace State
// ###################################################################
#pragma region Tree Trace State

typedef struct {
    // This is the stack of regions that still need to be closed to close this
    // region tree. A region stays on the stack until it is closed, so anything
    // its trace discovers is pushed on top of it and handled first. The loop can
    // therefore only drain once every region in the tree is closed.
    //
    // How many attempts a region gets is tracked by `tracing_counts`.
    PyObject *pending;
    // This tracks per region in the tree how often it has been traversed.
    // Some things require the trace to be redone, namely freezing an object
    // as that may create references and finding an open sub-region, as that
    // one needs to be traced and closed first.
    //
    // We limit the number of times we restart the trace per region.
    // Theoretically, this may reject some programs that would eventually
    // reach a fixed point, but if somebody wants to do dark magic, that's
    // really not our problem.
    _Py_hashtable_t *tracing_counts;
    // The region hierarchy of this trace, child nodes map to their parents.
    _Py_hashtable_t *hierarchy;

#ifdef Py_GIL_DISABLED
    // Objects which were owned by another thread, these need to be marked as shared
    // before tracing can succeed.
    PyObject *non_local_objs;
#endif
} tree_trace_state_t;

static void tree_trace_state_destroy(tree_trace_state_t* state) {
    if (state->tracing_counts) {
        _Py_hashtable_destroy(state->tracing_counts);
        state->tracing_counts = NULL;
    }
    if (state->hierarchy) {
        _Py_hashtable_destroy(state->hierarchy);
        state->hierarchy = NULL;
    }
    if (state->pending) {
        Py_CLEAR(state->pending);
    }
#ifdef Py_GIL_DISABLED
    if (state->non_local_objs) {
        Py_CLEAR(state->non_local_objs);
    }
#endif
}

static int tree_trace_state_init(tree_trace_state_t* state) {
    // Both fields have to be cleared up front, so that the error path below can
    // call `tree_trace_state_destroy()` before they have all been assigned.
    state->tracing_counts = NULL;
    state->pending = NULL;
    state->hierarchy = NULL;

    state->tracing_counts = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if (state->tracing_counts == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    state->pending = PyList_New(0);
    if (state->pending == NULL) {
        goto error;
    }

#ifdef Py_GIL_DISABLED
    state->non_local_objs = PyList_New(0);
    if (state->non_local_objs == NULL) {
        goto error;
    }
#endif

    state->hierarchy = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if (state->hierarchy == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    return 0;
error:
    tree_trace_state_destroy(state);
    return -1;
}

#ifdef Py_GIL_DISABLED
// Back-off between merge-flag polls. Owners merge at their eval breaker, so the
// first polls are cheap and quick; a blocked owner only merges during GC, so we
// grow the interval up to a cap to avoid spinning while the timeout runs down.
#define NON_LOCAL_POLL_MIN_NS (10 * 1000)
#define NON_LOCAL_POLL_MAX_NS (1000 * 1000)

// Wait until every object in `non_local_objs` has been merged into shared form
// by its owning thread . An object is done once `_Py_REF_IS_MERGED` holds.
// A negative value waits forever. Returns 0 once the list is empty, or -1 with
// TimeoutError set if the deadline passes first (stragglers stay in the list).
static int tree_trace_state_wait_non_local_objects(tree_trace_state_t* state, Py_ssize_t timeout) {
    PyObject *objs = state->non_local_objs;

    bool has_deadline = timeout >= 0;
    PyTime_t deadline = has_deadline ? _PyDeadline_Init(timeout * 1000 * 1000) : 0;

    // Stack address to park on; nobody unparks it, so each park is just a
    // detached safe-point sleep.
    uint8_t sleeper = 0;
    uint8_t expected = 0;

    PyTime_t backoff = NON_LOCAL_POLL_MIN_NS;

    for (;;) {
        // Swap-remove moves the last element into slot `i`, so scan back to
        // front to leave the not-yet-scanned prefix untouched.
        for (Py_ssize_t i = PyList_GET_SIZE(objs) - 1; i >= 0; i--) {
            PyObject *obj = PyList_GET_ITEM(objs, i);
            if (_Py_REF_IS_MERGED(_Py_atomic_load_ssize_acquire(&obj->ob_ref_shared))) {
                list_remove(objs, i);
            }
        }

        if (PyList_GET_SIZE(objs) == 0) {
            return 0;
        }

        PyTime_t sleep_ns = backoff;
        if (has_deadline) {
            PyTime_t remaining = _PyDeadline_Get(deadline);
            if (remaining <= 0) {
                PyErr_Format(PyExc_TimeoutError,
                    "timed out waiting for %zd non-local object(s) to become shared",
                    PyList_GET_SIZE(objs));
                return -1;
            }
            if (remaining < sleep_ns) {
                sleep_ns = remaining;
            }
        }

        // Park with detach=1 so a concurrent stop-the-world can proceed while
        // we back off -- in particular the GC, which is what merges the queues
        // of owners that are blocked or not running bytecode.
        (void)_PyParkingLot_Park(&sleeper, &expected, sizeof(sleeper),
                                 sleep_ns, NULL, /*detach=*/1);

        if (backoff < NON_LOCAL_POLL_MAX_NS) {
            backoff *= 2;
            if (backoff > NON_LOCAL_POLL_MAX_NS) {
                backoff = NON_LOCAL_POLL_MAX_NS;
            }
        }
    }
}
#endif
#pragma endregion // Tree Trace State

// ###################################################################
// Region Trace State
// ###################################################################
#pragma region Region Trace State

typedef struct {
    Py_ssize_t unaccounted_rc;
    Py_ssize_t inital_rc;
} region_visited_info_t;

static region_visited_info_t* new_visit_info(Py_ssize_t unaccounted_rc, Py_ssize_t inital_rc) {
    region_visited_info_t *info = PyMem_Calloc(1, sizeof(region_visited_info_t));
    if (info == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    info->unaccounted_rc = unaccounted_rc;
    info->inital_rc = inital_rc;

    return info;
}

#define _VisitInfo_CAST(x) _Py_CAST(region_visited_info_t*, x)

typedef struct {
    // List of pending objects that are not GC
    PyObject *pending;
    // A list of all visited objects, mapping from the object to a
    // `region_visited_info_t`
    _Py_hashtable_t *visited;

    // The trace state belonging to the region tree that this region
    // is a part of.
    tree_trace_state_t *tree_trace_state;
    // The bridge object of the region that is currently being traced.
    PyObject* bridge;
    // The source of the reference, this is used for error reporting
    PyObject *src;

    // The number of refs coming into this object graph
    Py_ssize_t external_rc;
    // The number of refs coming from inside the region to the bridge object
    Py_ssize_t bridge_rc;

    // The GC list used for this trace, it may be null if the trace
    // should not move the objects from their current list.
    PyGC_Head* gc_list;


    // This is set if an object was frozen and the trace needs
    // to restart to be valid
    bool restart;

    // Indicates if the given reference is a strong reference or a weak one.
    bool strong_ref;

    bool has_weak_refs;
} region_trace_state_t;

static int _region_trace_state_free_visited(
    _Py_hashtable_t *ht, const void *key, const void *value, void *user_data)
{
#ifdef Py_GIL_DISABLED
    PyObject* obj = _PyObject_CAST(key);

    // Clear the flag we set during traversal, this is needed for the error
    // case if the validation code didn't clear the flag.
    _Py_OB_FLAG_REMOVE(obj, _Py_REGION_TRACE_FLAG);

    // Balances the `Py_INCREF(obj)` taken when the object was marked visited.
    Py_DECREF(obj);
#endif

    PyMem_Free((void*)value);

    return 0;
}

static void region_trace_state_destroy(region_trace_state_t* state) {
    if (state->pending) {
        Py_CLEAR(state->pending);
    }
    if (state->visited) {
        (void)_Py_hashtable_foreach(
            state->visited, _region_trace_state_free_visited, NULL);
        _Py_hashtable_destroy(state->visited);
        state->visited = NULL;
    }
}

static int region_trace_state_init(
    region_trace_state_t* state,
    PyObject* bridge,
#ifdef _Py_PYRONA_INTERPRETER_SHARING
    PyGC_Head* gc_list,
#endif
    tree_trace_state_t *tree_trace_state
) {
    state->pending = NULL;
    state->visited = NULL;

    state->pending = PyList_New(0);
    if (state->pending == NULL) {
        goto error;
    }

    state->visited = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if (state->visited == NULL) {
        goto error;
    }

    state->tree_trace_state = tree_trace_state;
    state->bridge = bridge;
    state->src = NULL;

    state->external_rc = 0;
    state->bridge_rc = 0;
    state->restart = false;
    // References are strong unless the trace explicitly follows a weak one.
    state->strong_ref = true;
    state->has_weak_refs = false;

#ifdef _Py_PYRONA_INTERPRETER_SHARING
    assert(gc_list == NULL || gc_list_is_empty(gc_list));
    state->gc_list = gc_list;
#endif

    return 0;
error:
    region_trace_state_destroy(state);
    return -1;
}

static void region_trace_state_set_restart(region_trace_state_t* state) {
    state->restart = true;
    // Setting the gc_list to NULL will stop objects from being moved
    // between GC lists. Just a small thing we can avoid. The next (full)
    // trace will have this set again.
    state->gc_list = NULL;
}

enum {
    TRACE_RES_ERR = -1,
    TRACE_RES_DONE = 0,
    // The trace itself succeeded, but it was based on information that changed
    // while it ran, so the region is still open and needs another attempt.
    TRACE_RES_RESTART = 1,
};

#pragma endregion // Region Trace State

// ###################################################################
// Error Reporting
// ###################################################################
#pragma region Error Reporting

typedef struct {
    // Every object with incoming references, used to mark up the mermaid graph.
    _Py_hashtable_t *problem_objs;
    // The subset of `problem_objs` that the error message lists, capped at
    // `ERROR_OBJECT_REPORT_COUNT` entries.
    _Py_hashtable_t *reported_objs;
    Py_ssize_t incoming_refs;
} close_error_info_t;

typedef struct {
    _Py_hashtable_t *problem_objs;
    _Py_hashtable_t *reported_objs;
} close_error_filter_t;

typedef struct {
    // A strong reference, see `collect_incoming_ref()`.
    PyObject *obj;
    Py_ssize_t refs;
} incoming_ref_entry_t;

typedef struct {
    // `collect_close_error_obj()` caps the reported set at this size.
    incoming_ref_entry_t entries[ERROR_OBJECT_REPORT_COUNT];
    Py_ssize_t count;
} incoming_ref_report_t;

typedef struct {
    PyUnicodeWriter *writer;
    _Py_hashtable_t *visited;
    _Py_hashtable_t *problem_objs;
    _Py_hashtable_t *reported_objs;
    PyObject *pending;
    PyObject *src;
} mermaid_dump_state_t;

static int
collect_close_error_obj(_Py_hashtable_t *ht, const void *key, const void *value, void *user_data)
{
    close_error_filter_t *filter = (close_error_filter_t *)user_data;
    Py_ssize_t refs = _VisitInfo_CAST(value)->unaccounted_rc;

    // Objects whose every reference came from inside the region are not part of
    // the problem.
    if (refs <= 0) {
        return 0;
    }
    if (_Py_hashtable_set(filter->problem_objs, key, (void *)refs) < 0) {
        PyErr_NoMemory();
        return -1;
    }
    if (_Py_hashtable_len(filter->reported_objs) < ERROR_OBJECT_REPORT_COUNT) {
        if (_Py_hashtable_set(filter->reported_objs, key, (void *)refs) < 0) {
            PyErr_NoMemory();
            return -1;
        }
    }
    return 0;
}

static void
close_error_info_destroy(close_error_info_t *info)
{
    if (info->problem_objs != NULL) {
        _Py_hashtable_destroy(info->problem_objs);
        info->problem_objs = NULL;
    }
    if (info->reported_objs != NULL) {
        _Py_hashtable_destroy(info->reported_objs);
        info->reported_objs = NULL;
    }
}

static int
close_error_info_init(close_error_info_t *info, region_trace_state_t *state)
{
    info->incoming_refs = state->external_rc;
    info->problem_objs = NULL;
    info->reported_objs = NULL;
    info->problem_objs = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if (info->problem_objs == NULL) {
        return -1;
    }
    info->reported_objs = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if (info->reported_objs == NULL) {
        close_error_info_destroy(info);
        return -1;
    }

    close_error_filter_t filter = {info->problem_objs, info->reported_objs};
    int res = _Py_hashtable_foreach(state->visited, collect_close_error_obj, &filter);
    if (res < 0) {
        close_error_info_destroy(info);
        return -1;
    }
    return 0;
}

static int
collect_incoming_ref(_Py_hashtable_t *ht, const void *key, const void *value, void *user_data)
{
    incoming_ref_report_t *report = (incoming_ref_report_t *)user_data;

    assert(report->count < ERROR_OBJECT_REPORT_COUNT);
    if (report->count >= ERROR_OBJECT_REPORT_COUNT) {
        return 0;
    }

    incoming_ref_entry_t *entry = &report->entries[report->count];
    // The hashtable stores raw pointers without owning a reference. Taking one
    // here keeps every reported object alive while `__str__` runs on the others,
    // since that can execute arbitrary code and drop the last reference to any
    // of them.
    entry->obj = Py_NewRef((PyObject *)key);
    entry->refs = (Py_ssize_t)value;
    report->count += 1;
    return 0;
}

static void
incoming_ref_report_clear(incoming_ref_report_t *report)
{
    for (Py_ssize_t i = 0; i < report->count; i++) {
        Py_CLEAR(report->entries[i].obj);
    }
    report->count = 0;
}

static PyObject *
build_close_error_message(close_error_info_t *info)
{
    incoming_ref_report_t report = {{{NULL, 0}}, 0};
    PyUnicodeWriter *writer = NULL;

    // Collect the reported objects, and with them their references, before any
    // of them is formatted below.
    if (_Py_hashtable_foreach(info->reported_objs, collect_incoming_ref, &report) < 0) {
        goto error;
    }

    writer = PyUnicodeWriter_Create(0);
    if (writer == NULL) {
        goto error;
    }

    if (PyUnicodeWriter_WriteUTF8(writer,
            "The region could not be closed due to:\n", -1) < 0) {
        goto error;
    }

    Py_ssize_t accounted = 0;
    for (Py_ssize_t i = 0; i < report.count; i++) {
        PyObject *obj = report.entries[i].obj;
        Py_ssize_t refs = report.entries[i].refs;
        accounted += refs;

        if (PyUnicodeWriter_Format(writer,
                "- %zd incoming reference%s to %s '%S'\n",
                refs, (refs == 1) ? "" : "s", Py_TYPE(obj)->tp_name, obj) < 0) {
            goto error;
        }
    }

    if (accounted < info->incoming_refs) {
        Py_ssize_t others = info->incoming_refs - accounted;
        if (PyUnicodeWriter_Format(writer,
                "- %zd reference%s to other objects\n",
                others, (others == 1) ? "" : "s") < 0) {
            goto error;
        }
    }

    incoming_ref_report_clear(&report);
    return PyUnicodeWriter_Finish(writer);

error:
    if (!PyErr_Occurred()) {
        PyErr_SetString(PyExc_RuntimeError, "failed to build region close error message");
    }
    incoming_ref_report_clear(&report);
    PyUnicodeWriter_Discard(writer);
    return NULL;
}

static int
mermaid_write_node(PyUnicodeWriter *writer, PyObject *obj)
{
    if (Region_Check(obj)) {
        bool open = region_is_open(_PyTRegion_CAST(obj));
        const char *status = open ? "open" : "closed";
        return PyUnicodeWriter_Format(writer,
            "n%p[\\Region<br>%s<br>rc=%zd<br><sub><sup>%p</sup></sub>/]",
            obj, status, Py_REFCNT(obj), obj);
    }
    if (Cown_Check(obj)) {
        return PyUnicodeWriter_Format(writer,
            "n%p([\"Cown<br>rc=%zd<br><sub><sup>%p</sup></sub>\"])",
            obj, Py_REFCNT(obj), obj);
    }
    return PyUnicodeWriter_Format(writer,
        "n%p[\"[%s]<br>rc=%zd<br><sub><sup>%p</sup></sub>\"]",
        obj, Py_TYPE(obj)->tp_name, Py_REFCNT(obj), obj);
}

static int
mermaid_write_class(
    PyUnicodeWriter *writer,
    PyObject *obj,
    _Py_hashtable_t *problem_objs,
    _Py_hashtable_t *reported_objs)
{
    if (_Py_IsShallowImmutable(obj)) {
        return PyUnicodeWriter_Format(writer, "    class n%p immutable\n", obj);
    }
    if (_Py_hashtable_get_entry(reported_objs, obj) != NULL) {
        return PyUnicodeWriter_Format(writer, "    class n%p error\n", obj);
    }
    if (_Py_hashtable_get_entry(problem_objs, obj) != NULL) {
        return PyUnicodeWriter_Format(writer, "    class n%p problem\n", obj);
    }
    return 0;
}

static int
mermaid_write_escaped_label(PyUnicodeWriter *writer, const char *label)
{
    for (const char *p = label; *p != '\0'; p++) {
        switch (*p) {
        case '|':
            if (PyUnicodeWriter_WriteChar(writer, '/') < 0) {
                return -1;
            }
            break;
        case '\n':
        case '\r':
            if (PyUnicodeWriter_WriteChar(writer, ' ') < 0) {
                return -1;
            }
            break;
        default:
            if (PyUnicodeWriter_WriteChar(writer, (Py_UCS4)(unsigned char)*p) < 0) {
                return -1;
            }
            break;
        }
    }
    return 0;
}

static int
mermaid_write_escaped_unicode_label(PyUnicodeWriter *writer, PyObject *label)
{
    Py_ssize_t size;
    const char *utf8 = PyUnicode_AsUTF8AndSize(label, &size);
    if (utf8 == NULL) {
        return -1;
    }

    Py_ssize_t start = 0;
    for (Py_ssize_t i = 0; i < size; i++) {
        switch (utf8[i]) {
        case '|':
            if (i > start && PyUnicodeWriter_WriteUTF8(writer, utf8 + start, i - start) < 0) {
                return -1;
            }
            if (PyUnicodeWriter_WriteChar(writer, '/') < 0) {
                return -1;
            }
            start = i + 1;
            break;
        case '\n':
        case '\r':
            if (i > start && PyUnicodeWriter_WriteUTF8(writer, utf8 + start, i - start) < 0) {
                return -1;
            }
            if (PyUnicodeWriter_WriteChar(writer, ' ') < 0) {
                return -1;
            }
            start = i + 1;
            break;
        default:
            break;
        }
    }
    if (size > start && PyUnicodeWriter_WriteUTF8(writer, utf8 + start, size - start) < 0) {
        return -1;
    }
    return 0;
}

static int
mermaid_enqueue_if_needed(mermaid_dump_state_t *state, PyObject *obj)
{
    if (_Py_IsShallowImmutable(obj) || Cown_Check(obj)) {
        return 0;
    }
    if (Region_Check(obj) && state->src != NULL) {
        return 0;
    }
    if (_Py_hashtable_get_entry(state->visited, obj) != NULL) {
        return 0;
    }
    if (_Py_hashtable_set(state->visited, obj, obj) < 0) {
        PyErr_NoMemory();
        return -1;
    }
    return PyList_Append(state->pending, obj);
}

static int
mermaid_visit_labeled(
    PyObject *obj,
    mermaid_dump_state_t *state,
    const char *ascii_label,
    PyObject *unicode_label)
{
    if (_Py_IsDeepImmutable(obj) && ERROR_MERMAID_HIDE_IMMUTABLE && !Cown_Check(obj)) {
        return 0;
    }

    if (state->src != NULL) {
        if (PyUnicodeWriter_WriteUTF8(state->writer, "    ", -1) < 0) {
            return -1;
        }
        if (mermaid_write_node(state->writer, state->src) < 0) {
            return -1;
        }
        if (ascii_label != NULL || unicode_label != NULL) {
            if (PyUnicodeWriter_WriteUTF8(state->writer, " -->|", -1) < 0) {
                return -1;
            }
            if (ascii_label != NULL) {
                if (mermaid_write_escaped_label(state->writer, ascii_label) < 0) {
                    return -1;
                }
            }
            if (unicode_label != NULL && mermaid_write_escaped_unicode_label(state->writer, unicode_label) < 0) {
                return -1;
            }
            if (PyUnicodeWriter_WriteUTF8(state->writer, "| ", -1) < 0) {
                return -1;
            }
        }
        else if (PyUnicodeWriter_WriteUTF8(state->writer, " --> ", -1) < 0) {
            return -1;
        }
    } else if (PyUnicodeWriter_WriteUTF8(state->writer, "    ", -1) < 0) {
        return -1;
    }

    if (mermaid_write_node(state->writer, obj) < 0) {
        return -1;
    }
    if (PyUnicodeWriter_WriteUTF8(state->writer, "\n", -1) < 0) {
        return -1;
    }
    if (mermaid_write_class(state->writer, obj, state->problem_objs, state->reported_objs) < 0) {
        return -1;
    }

    return mermaid_enqueue_if_needed(state, obj);
}

static int
mermaid_visit(PyObject *obj, mermaid_dump_state_t *state)
{
    return mermaid_visit_labeled(obj, state, NULL, NULL);
}

static int
mermaid_visit_dict(PyObject *obj, mermaid_dump_state_t *state)
{
    Py_ssize_t pos = 0;
    PyObject *key;
    PyObject *value;

    while (PyDict_Next(obj, &pos, &key, &value)) {
        if (!_PyImmutability_CanViewAsDeepImmutable(key)
            && !Cown_Check(key)
            && !Region_Check(key)
        ) {
            if (mermaid_visit_labeled(key, state, "<key>", NULL) < 0) {
                return -1;
            }
        }

        PyObject *label = PyUnicode_Check(key) ? key : NULL;
        if (mermaid_visit_labeled(value, state, NULL, label) < 0) {
            return -1;
        }
    }
    return 0;
}

static int
mermaid_visit_sequence(PyObject *obj, mermaid_dump_state_t *state)
{
    Py_ssize_t size = PyList_CheckExact(obj) ? PyList_GET_SIZE(obj) : PyTuple_GET_SIZE(obj);
    for (Py_ssize_t i = 0; i < size; i++) {
        char label[32];
        PyOS_snprintf(label, sizeof(label), "#91;%zd#93;", i);
        PyObject *item = PyList_CheckExact(obj) ? PyList_GET_ITEM(obj, i) : PyTuple_GET_ITEM(obj, i);
        if (mermaid_visit_labeled(item, state, label, NULL) < 0) {
            return -1;
        }
    }
    return 0;
}

static int
mermaid_traverse(PyObject *obj, mermaid_dump_state_t *state)
{
    if (PyDict_CheckExact(obj)) {
        return mermaid_visit_dict(obj, state);
    }
    if (PyList_CheckExact(obj) || PyTuple_CheckExact(obj)) {
        return mermaid_visit_sequence(obj, state);
    }

    return _PyObject_VisitReachable(obj, (visitproc)mermaid_visit, (void *)state);
}

static void
mermaid_dump_state_destroy(mermaid_dump_state_t *state)
{
    if (state->writer != NULL) {
        PyUnicodeWriter_Discard(state->writer);
        state->writer = NULL;
    }
    Py_CLEAR(state->pending);
    if (state->visited != NULL) {
        _Py_hashtable_destroy(state->visited);
        state->visited = NULL;
    }
}

static int
mermaid_dump_state_init(
    mermaid_dump_state_t *state,
    _Py_hashtable_t *problem_objs,
    _Py_hashtable_t *reported_objs)
{
    state->writer = NULL;
    state->visited = NULL;
    state->pending = NULL;
    state->src = NULL;
    state->problem_objs = problem_objs;
    state->reported_objs = reported_objs;

    state->writer = PyUnicodeWriter_Create(0);
    if (state->writer == NULL) {
        goto error;
    }
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
    return 0;

error:
    mermaid_dump_state_destroy(state);
    return -1;
}

static int
dump_mermaid_diagram(
    PyObject *root,
    _Py_hashtable_t *problem_objs,
    _Py_hashtable_t *reported_objs)
{
    int res = -1;
    mermaid_dump_state_t state;
    PyObject *diagram = NULL;
    // Owns the item currently being traversed, released at `finally`.
    PyObject *item = NULL;

    // Writing a file into the working directory is too surprising to do by
    // default, so the graph is only dumped when it has been asked for. The
    // value of the variable is the path to write to.
    const char *path = Py_GETENV(REGION_GRAPH_ENV_VAR);
    if (path == NULL || *path == '\0') {
        return 0;
    }

    if (mermaid_dump_state_init(&state, problem_objs, reported_objs) < 0) {
        return -1;
    }

    if (PyUnicodeWriter_WriteUTF8(state.writer, "flowchart TD\n", -1) < 0) {
        goto finally;
    }
    if (mermaid_visit(root, &state) < 0) {
        goto finally;
    }

    while (PyList_GET_SIZE(state.pending) > 0) {
        Py_XSETREF(item, list_pop(state.pending));
        if (item == NULL) {
            goto finally;
        }
        state.src = item;
        SUCCEEDS(mermaid_traverse(item, &state));
    }
    Py_CLEAR(item);

    diagram = PyUnicodeWriter_Finish(state.writer);
    state.writer = NULL;
    if (diagram == NULL) {
        goto finally;
    }

    const char *body = PyUnicode_AsUTF8(diagram);
    if (body == NULL) {
        goto finally;
    }

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
        goto finally;
    }
    if (fputs(
            "<div style='background: #fff'>\n"
            "\n"
            "```mermaid\n"
            "%%{init: {'theme': 'neutral', 'themeVariables': { 'fontSize': '16px' }}}%%\n"
            "\n",
            f) < 0
        || fputs(body, f) < 0
        || fputs(
            "\n"
            "classDef immutable fill:#94f7ff\n"
            "classDef problem fill:#ffe8d6,stroke:#f08c00,stroke-width:2px\n"
            "classDef error fill:#ffe8d6,stroke:red,stroke-width:4px\n"
            "```\n"
            "</div>\n",
            f) < 0)
    {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
        fclose(f);
        goto finally;
    }
    // Buffered writes can still fail here, so this result matters too.
    if (fclose(f) != 0) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
        goto finally;
    }

    res = 0;

finally:
    Py_XDECREF(item);
    mermaid_dump_state_destroy(&state);
    Py_XDECREF(diagram);
    return res;
error:
    goto finally;
}
#pragma endregion // Error Reporting

// ###################################################################
// Tracing Region Impl
// ###################################################################
#pragma region Region Tracing Impl
static int _move_obj(PyObject* obj, region_trace_state_t* state) {
    // Check the movability of the object:
    movable_status_t status = get_movable_status(obj);
    switch (status) {
    case Py_MOVABLE_YES:
        break;
    case Py_MOVABLE_NO:
        dbg("    - %p is not movable", obj);
        throw_region_error(
            "Instances of type '%s' are not movable", Py_TYPE(obj)->tp_name,
            state->src, obj);
        return TRACE_RES_ERR;
    case Py_MOVABLE_FREEZE:
        // Freeze the object, this can invalidate our `external_rc`,
        // we restart after this trace
        dbg("    - freezing %p", obj);
        if (_PyImmutability_DeepFreeze(obj, false)) {
            return TRACE_RES_ERR;
        }

        region_trace_state_set_restart(state);
        return 0;
    case Py_MOVABLE_COWN:
        return 0;
    default:
        Py_UNREACHABLE();
    }

    // References to the bridge object are allowed and counted by
    // `state->bridge_rc` instead. `_trace_visit()` intercepts them, so the
    // bridge must never end up in `visited` or in the LRC below.
    assert(obj != state->bridge);

#ifndef _Py_PYRONA_INTERPRETER_SHARING
    // We get the owner first, just in case the actual owner is concurrently
    // decref'ing and then setting the owner to shared.
    uintptr_t owner = _Py_atomic_load_uintptr_relaxed(&obj->ob_tid);
#endif

    // Update the LRC
    Py_ssize_t object_rc = Py_REFCNT(obj);
    Py_ssize_t lrc_change = object_rc;
    if (state->strong_ref) {
        // -1 for the reference we just followed
        lrc_change -= 1;
    }
#ifndef _Py_PYRONA_INTERPRETER_SHARING
    // On Sub-Interpreters we subtract the internal references from closed
    // bridge. However, on free-threading we keep the reference count intact.
    // We manually have to subtract it here.
    if (Region_Check(obj)) {
        lrc_change -= _PyTRegion_CAST(obj)->internal_bridge_refs;
    }
#endif
    dbg("    - moving %p; LRC += %zd", obj, lrc_change);
    state->external_rc += lrc_change;

    region_visited_info_t *visit_info = new_visit_info(lrc_change, object_rc);
    if (visit_info == NULL) {
        return -1;
    }

#ifdef Py_GIL_DISABLED
    // We have to set the flag after we read the RC. Otherwise, a concurrent
    // thread may modify the RC without us observing it in either the flag
    // or RC value.
    _Py_OB_FLAG_ADD(obj, _Py_REGION_TRACE_FLAG);

    // On Free-Threading we need to incref the object in case another thread kills
    // all references. This ensures that obj is still allocated on cleanup
    Py_INCREF(obj);
#endif
    // Mark the object as visited, this stores the lrc_change for better error reporting
    if (_Py_hashtable_set(state->visited, obj, (void*)visit_info) == -1) {
        PyErr_NoMemory();
        return -1;
    }

#ifdef Py_GIL_DISABLED
    if (owner != _Py_ThreadId() && owner != _Py_UNOWNED_TID) {
        Py_INCREF(obj);
        _Py_brc_queue_object(obj);
        if (PyList_Append(state->tree_trace_state->non_local_objs, obj)) {
            return -1;
        }
        region_trace_state_set_restart(state);
    }
#endif

#ifdef _Py_PYRONA_INTERPRETER_SHARING
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
#endif

    // Bridge objects of sub-regions are moved, but shouldn't be traversed.
    if (!Region_Check(obj)) {
        if (PyList_Append(state->pending, obj)) {
            return -1;
        }
    }

    return 0;
}

static int _trace_visit_bridge_ref(PyObject* obj, region_trace_state_t* state) {
    assert(Region_Check(obj));
    tree_trace_state_t *tree_state = state->tree_trace_state;

    // If the child region is closed we can move it directly
    if (_PyTracingRegion_IsClosed(obj)) {
        int res = _move_obj(obj, state);
        // Update the region reference meta of the child, if it has one
        if (res == 0) {
            TracingRegionObject *child = (TracingRegionObject *)obj;
            bool no_memory = false;
            LOCK_REGION_REF_META();
            if (child->meta != NULL) {
                _PyRegionRefMetadata *parent =
                    region_meta_lock_held((TracingRegionObject *)state->bridge);
                if (parent == NULL) {
                    no_memory = true;
                }
                else {
                    _PyRegionRef_MetaSetParentLockHeld(child->meta, parent);
                }
            }
            UNLOCK_REGION_REF_META();
            if (no_memory) {
                PyErr_NoMemory();
                return TRACE_RES_ERR;
            }
        }
        return res;
    }

    _Py_hashtable_entry_t *entry = _Py_hashtable_get_entry(tree_state->hierarchy, (void*)obj);
    if (entry == NULL) {
        if (_Py_hashtable_set(tree_state->hierarchy, (void*)obj, state->bridge)) {
            PyErr_NoMemory();
            return -1;
        }

        void *child = (void*)state->bridge;
        entry = _Py_hashtable_get_entry(tree_state->hierarchy, child);
        while (entry != NULL) {
            if (entry->value == obj) {
                PyErr_Format(
                    PyExc_RuntimeError,
                    "the region %p can not be closed as it attempts to reference one of its parent regions %p",
                    (void *)obj,
                    entry->value);
                return -1;
            }

            child = entry->value;
            entry = _Py_hashtable_get_entry(tree_state->hierarchy, child);
        }
    } else {
        // We could use this branch to enforce that only a single owning
        // exists for each bride. For now we allow these as long as they
        // come from the same region
    }

    // The child region is open, we need to traverse it first and then
    // retry closing this.
    if (PyList_Append(tree_state->pending, obj) < 0) {
        return -1;
    }
    region_trace_state_set_restart(state);

    return 0;
}

static int _trace_visit(PyObject* obj, region_trace_state_t* state) {
    // References to immutable objects are allowed
    if (_PyImmutability_CanViewAsDeepImmutable(obj)) {
        return 0;
    }

    // References to the bridge are tracked separately
    if (obj == state->bridge) {
        // Region objects can't have weak references
        assert(state->strong_ref);
        assert(get_movable_status(obj) == Py_MOVABLE_YES);
        // This branch also accounts for references from the bridge object to itself.
        dbg("    - Internal reference to bridge from %p; bridge_rc += 1", state->src);
        state->bridge_rc += 1;
        return 0;
    }

    // Check if the object is already part of the region
    _Py_hashtable_entry_t *entry = _Py_hashtable_get_entry(state->visited, (void*)obj);
    if (entry != NULL) {
        assert(get_movable_status(obj) == Py_MOVABLE_YES);
        // state->external_rc only counts strong references
        if (state->strong_ref) {
            region_visited_info_t *visit_info = _VisitInfo_CAST(entry->value);
            visit_info->unaccounted_rc -= 1;
            dbg("    - Internal reference to %p; LRC -= 1", obj);
            state->external_rc -= 1;
        }
        return 0;
    }

    // References external regions turns them into sub-regions. These
    // need to be traversed and closed separately
    if (Region_Check(obj)) {
        return _trace_visit_bridge_ref(obj, state);
    }

    return _move_obj(obj, state);
}

#ifndef _Py_PYRONA_INTERPRETER_SHARING
static int
_validate_region_closed_visit(_Py_hashtable_t *ht, const void *key, const void *value,
                    void *user_data)
{
    PyObject *obj = _PyObject_CAST(key);
    region_visited_info_t *info = _VisitInfo_CAST(value);

    // TODO(regions): Pause weaks until we can invalidate it

    // ### Soundness:
    // During tracing we set a flag on each object. Every RC operation
    // first checks this flag and then clears it. If the flag has been
    // cleared we abort since a concurrent thread has/had access to
    // the traced objects.
    if ((_Py_OB_FLAGS_LOAD(obj) & _Py_REGION_TRACE_FLAG) == 0) {
        goto error;
    }
    _Py_OB_FLAG_REMOVE(obj, _Py_REGION_TRACE_FLAG);

    // ### Soundness:
    // This catches RC updates that passed the flag but got paused before
    // the RC update.
    //
    // The thread could remain stalled there, but then we would either observe
    // the incoming reference and not make it this far, or if it's a lock-free
    // read on a now killed reference the try-inc-ref will fail.
    Py_ssize_t expected_rc = info->inital_rc + 1;
    if (Py_REFCNT(obj) != expected_rc) {
        goto error;
    }

    // ### Soundness:
    // Closed Sub-regions are not traversed, which means we wouldn't detect the
    // graph to not be isolated. During this validation check we just ensure that
    // any sub-region remained closed until now.
    if (Region_Check(obj)) {
        if (region_is_open(_PyTRegion_CAST(obj))) {
                PyErr_Format(
                PyExc_RuntimeError,
                "the sub-region %p was opend during tracing by a concurrent thread",
                (void*)obj);
            return -1;
        }
    }

    return 0;
error:
    PyErr_Format(
        PyExc_RuntimeError,
        "the region cannot be closed since a concurrent thread accessed during tracing");
    return -1;
}
#endif

static int _validate_region_closed(TracingRegionObject *region_obj, region_trace_state_t *state) {
    // With the GIL we know that an isolated trace is valid, however on
    // free-threaded Python we need to validate that no references were
    // manipulated under foot.
#ifndef _Py_PYRONA_INTERPRETER_SHARING
    if (_Py_hashtable_foreach(
        state->visited, _validate_region_closed_visit, NULL)
    ) {
        return -1;
    }
#endif
    return 0;
}

static int _try_close_region(PyObject *region_obj, tree_trace_state_t *tree_trace_state) {
    assert(Region_Check(region_obj));
    TracingRegionObject* region = _PyTRegion_CAST(region_obj);

    // Finalized regions can't be closed since they're deletion would not call the
    // finalizer and therefore leak the owned nodes.
    if (_PyGC_FINALIZED(region_obj)) {
        PyErr_Format(
            PyExc_RuntimeError,
            "the region %p has been finalized and cannot be closed again",
            (void *)region_obj);
        return TRACE_RES_ERR;
    }

    int mark_res = region_set_tracing_mark(region);
    if (mark_res < 0) {
        return TRACE_RES_ERR;
    } else if (mark_res > 0) {
        assert(region->state == Py_REGION_STATUS_CLOSED);
        return TRACE_RES_DONE;
    }

    // Init trace state.
    region_trace_state_t state;
    if (region_trace_state_init(
        &state,
        _PyObject_CAST(region),
#ifdef _Py_PYRONA_INTERPRETER_SHARING
        &region->gc_list,
#endif
        tree_trace_state)
    ) {
        region_clear_tracing_mark(region);
        return TRACE_RES_ERR;
    }
    int region_trace_res = TRACE_RES_DONE;
    // Owns the item currently being traversed, released at `finally`.
    PyObject *item = NULL;

    SUCCEEDS(PyList_Append(state.pending, _PyObject_CAST(region)));

    while (PyList_GET_SIZE(state.pending) > 0) {
        // Find the next pending item:
        Py_XSETREF(item, list_pop(state.pending));
        if (item == NULL) {
            goto error;
        }

        // Traverse item
        state.src = item;
        dbg("  - traversing %p", item);
        SUCCEEDS(_PyObject_VisitReachable(item, (visitproc)_trace_visit, (void*)&state));

        if (PyWeakref_Check(item)) {
            PyWeakReference *wref = (PyWeakReference*)item;
            state.strong_ref = false;
            // TODO(regions): This races with concurrent deallocation of the
            // referent. `Py_BEGIN_CRITICAL_SECTION(wref)` locks the weakref's
            // own `ob_mutex`, but `wr_object` is cleared under the referent's
            // list lock (`WEAKREF_LIST_LOCK(wr_object)`), so this critical
            // section does not exclude the clearing path. Another thread can
            // free the referent between the load and the `Py_INCREF` in
            // `_move_obj()`. The sound read is `get_ref_lock_held()`'s
            // try-incref under `LOCK_WEAKREFS(referent)`, but pinning the
            // referent inflates its `Py_REFCNT` and thus the weak-ref LRC,
            // which still needs to be compensated for.
            Py_BEGIN_CRITICAL_SECTION(wref);
            SUCCEEDS(_trace_visit(wref->wr_object, &state));
            Py_END_CRITICAL_SECTION();
            state.strong_ref = true;
            state.has_weak_refs = true;
        }
    }
    Py_CLEAR(item);

    if (state.restart) {
#ifdef _Py_PYRONA_INTERPRETER_SHARING
        gc_list_dissolve(&region->gc_list);
#endif
        region_trace_res = TRACE_RES_RESTART;
        region_clear_tracing_mark(region);
        goto finally;
    }

    // Report an error, if the region couldn't be closed.
    if (state.external_rc > 0) {
#ifdef _Py_PYRONA_INTERPRETER_SHARING
        gc_list_dissolve(&region->gc_list);
#endif

        dbg("- Failed to close region %p, there are %zd incoming references", region, state.external_rc);
        close_error_info_t error_info = {0};
        if (close_error_info_init(&error_info, &state) < 0) {
            goto error;
        }
        if (_Py_hashtable_len(state.visited) < ERROR_MERMAID_REPORT_LIMIT) {
            // Borrowed error tables; dump_mermaid_diagram() does not take ownership.
            if (dump_mermaid_diagram(
                    region_obj,
                    error_info.problem_objs,
                    error_info.reported_objs) < 0) {
                // The graph is a diagnostic aid. Report why it is missing, but
                // don't let that replace the region error being built here.
                PyErr_FormatUnraisable(
                    "Exception ignored while writing the region graph");
            }
        }

        PyObject *msg = build_close_error_message(&error_info);
        close_error_info_destroy(&error_info);
        if (msg == NULL) {
            goto error;
        }
        PyErr_SetObject(PyExc_RuntimeError, msg);
        Py_DECREF(msg);
        goto error;
    }

    SUCCEEDS(_validate_region_closed(region, &state));

    SUCCEEDS(_region_close(region, state.bridge_rc, state.visited, state.has_weak_refs));

    goto finally;
error:
    region_trace_res = TRACE_RES_ERR;
    region_clear_tracing_mark(region);
finally:
    Py_CLEAR(item);
    region_trace_state_destroy(&state);

    return region_trace_res;
}
#pragma endregion

/* Resolves the region reference meta of every region this trace touched.
 */
static int
resolve_region_meta(_Py_hashtable_t *ht, const void *key, const void *value,
                    void *user_data)
{
    TracingRegionObject *region = (TracingRegionObject *)key;
    if (region->meta == NULL) {
        return 0;
    }
    // The region remains open, therefore we mark it as being local to the IP
    if (region_is_open(region)) {
        region_meta_release(region);
        return 0;
    }

    // The region was closed, we resolve the WIP state
    _PyRegionRef_MetaResolveWip(region->meta);
    return 0;
}

static int try_close_region_tree(PyObject *root) {
    dbg("Starting region tree trace from %p", root);

    tree_trace_state_t state;
    if (tree_trace_state_init(&state)) {
        return -1;
    }

    int tree_trace_res = TRACE_RES_DONE;

#ifdef Py_GIL_DISABLED
    // Pending-stack index of the region whose last attempt queued non-local
    // objects, or -1 if none.
    Py_ssize_t non_local_wait_idx = -1;
#endif

    SUCCEEDS(PyList_Append(state.pending, root));

    while (PyList_GET_SIZE(state.pending) > 0) {
        // Look at the region on top of the stack without removing it. A region
        // stays queued until it is closed, so the sub-regions that its trace
        // discovers end up above it and are closed first. Draining the stack
        // therefore means every region in the tree is closed, which is what lets
        // this function report success.
        Py_ssize_t top = PyList_GET_SIZE(state.pending) - 1;
        PyObject *region = PyList_GET_ITEM(state.pending, top);
        assert(Region_Check(region));

        // A closed region has nothing left to do. Regions can be queued more
        // than once, this handles all safe cases.
        if (!region_is_open(_PyTRegion_CAST(region))) {
            SUCCEEDS(PyList_SetSlice(state.pending, top, top + 1, NULL));
            continue;
        }

        // Account for this attempt before running it. Counting afterwards would
        // report a region that was closed by its last attempt as a failure, and
        // would grant `PER_REGION_TRACE_LIMIT + 1` attempts.
        _Py_hashtable_entry_t *entry = _Py_hashtable_get_entry(state.tracing_counts, (void*)region);
        if (entry == NULL) {
            if (_Py_hashtable_set(state.tracing_counts, (void*)region, (void*)1) < 0) {
                PyErr_NoMemory();
                goto error;
            }
        } else if ((Py_uintptr_t)entry->value < PER_REGION_TRACE_LIMIT) {
            entry->value = (void*)(((Py_uintptr_t)entry->value) + 1);
        } else {
            // FIXME(regions): It would be nicer to spend the last attempt on a
            // trace that reports the objects keeping the region open, like the
            // `external_rc != 0` path in `_try_close_region()` does, instead of
            // this bare message. The catch is that such a trace may close the
            // region after all, which is why it can't simply be run here.
            PyErr_Format(
                PyExc_RuntimeError,
                "the region %p could not be closed after %d tracing attempts",
                (void *)region,
                PER_REGION_TRACE_LIMIT);
            goto error;
        }

#ifdef Py_GIL_DISABLED
        if (top == non_local_wait_idx) {
            // This region's previous attempt queued non-local objects, wait until they're
            // shared before attempting again
            non_local_wait_idx = -1;
            SUCCEEDS(tree_trace_state_wait_non_local_objects(
                &state, NON_LOCAL_MERGE_TIMEOUT_MS));
        }
        Py_ssize_t non_local_before = PyList_GET_SIZE(state.non_local_objs);
#endif

        dbg("- tracing region %p", region);
        int res = _try_close_region(region, &state);
        if (res == TRACE_RES_ERR) {
            goto error;
        }
        // A restarted trace leaves the region open on purpose. It keeps its slot
        // on the stack and is retried once the sub-regions that its trace pushed
        // on top of it have been closed.
        assert(res == TRACE_RES_RESTART || _PyTracingRegion_IsClosed(region));

#ifdef Py_GIL_DISABLED
        // A grown list means this attempt queued non-local objects.
        if (PyList_GET_SIZE(state.non_local_objs) > non_local_before) {
            assert(res == TRACE_RES_RESTART);
            non_local_wait_idx = top;
        }
#endif
    }

    goto finally;
error:
    tree_trace_res = TRACE_RES_ERR;
finally:
    // `resolve_region_meta()` never fails, so the result can be ignored.
    (void)_Py_hashtable_foreach(state.tracing_counts, resolve_region_meta, NULL);
    tree_trace_state_destroy(&state);

    return tree_trace_res;
}

// ###################################################################
// Region Object
// ###################################################################
#pragma region Region Object

static PyObject *
TracingRegion_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    TracingRegionObject *self = _PyTRegion_CAST(type->tp_alloc(type, 0));
    if (self == NULL) {
        return NULL;
    }

#ifdef _Py_PYRONA_INTERPRETER_SHARING
    // The region is set up here rather than in `tp_init()`, so that a region
    // can never be observed in an uninitialized state.
    gc_list_init(&self->gc_list);
#endif

    self->meta = NULL;
    // We make the region open by default, this ensures that the first close
    // will handle the region type correctly. Alternatively, we could make them
    // closed in the beginning, but then handle the cases specifically.
    self->state = Py_REGION_STATUS_OPEN;

    return _PyObject_CAST(self);
}

static int
TracingRegion_init(TracingRegionObject *self, PyObject *args, PyObject *kwargs) {
    // `tp_new()` already set the region up. Re-running the initialization here
    // would reset the GC list holding the contents of a closed region and drop
    // the reference count that `_region_close()` subtracted from the bridge
    // object, so this only validates the arguments.
    if (!_PyArg_NoPositional("TracingRegion", args)
        || !_PyArg_NoKeywords("TracingRegion", kwargs))
    {
        return -1;
    }
    return 0;
}

/* Disposes of everything a closed region owns.
 *
 * Closing a region establishes that no object inside it has incoming references
 * from the outside; only the bridge object may have those. So once the bridge
 * object dies, every member of the region is garbage too, however the references
 * between them happen to be arranged.
 *
 * That lets the region clean up after itself instead of handing the objects back
 * to the GC.
 *
 * This can resurrect the bridge object, so it has to run as a finalizer.
 */
static void _region_delete_contents(TracingRegionObject *self) {
    assert(!region_is_open(self));

    dbg("Deleting the contents of region %p", self);

#ifdef _Py_PYRONA_INTERPRETER_SHARING
    PyGC_Head members;
    PyGC_Head survivors;
    gc_list_init(&members);
    gc_list_init(&survivors);

    // Steal the members first. `_open_region()` will then set internal values
    // but keep not invalidate `members`.
    gc_list_merge(&self->gc_list, &members);
    assert(gc_list_is_empty(&self->gc_list));
    _open_region(self);

    // The disposal needs a clean error state; a dealloc can happen mid-raise.
    PyObject *exc = PyErr_GetRaisedException();

    // Finalize everything before anything is released, so that no `__del__`
    // observes a member that is already gone.
    _PyGC_FinalizeGarbage(&members);

    // Cleaning the dict should deallocate most things.
    Py_CLEAR(self->dict);

    // Deallocate remaining cyclic garbage
    _PyGC_DeleteGarbage(&members, &survivors);
    PyErr_SetRaisedException(exc);

    // Anything a finalizer kept alive is not owned by the region any more.
    if (!gc_list_is_empty(&survivors)) {
        gc_list_dissolve(&survivors);
    }
    // Nothing may still point at these stack allocated list heads.
    assert(gc_list_is_empty(&members));
    assert(gc_list_is_empty(&survivors));
#else
    // FIXME: Here we can do more than just opening the region, we could delete all objects
    // and thereby delete cycles without the need of the GC.
    // This requires us to keep a list of contained objects in some other way.
    _open_region(self);

    Py_BEGIN_CRITICAL_SECTION(self);
    Py_CLEAR(self->dict);
    Py_END_CRITICAL_SECTION();
#endif
}

static int
TracingRegion_traverse(TracingRegionObject *self, visitproc visit, void *arg) {
#ifdef _Py_PYRONA_INTERPRETER_SHARING
    // If the region is closed, we know that everything inside the region is reachable.
    // There is no advantage of opening the region to double check. This would also
    // mess with the GC list of this region.
    if (region_is_open(self)) {
        Py_VISIT(self->dict);
    }
#else
    Py_VISIT(self->dict);
#endif
    return 0;
}

static int
TracingRegion_clear(TracingRegionObject *self) {
    // tp_clear is usually called from the GC during a STW so we don't
    // need to lock self
    _open_region_lock_held(self);
    Py_CLEAR(self->dict);
    return 0;
}

static void
TracingRegion_finalize(PyObject *op) {
    TracingRegionObject *self = _PyTRegion_CAST(op);

    if (region_is_open(self)) {
#ifdef _Py_PYRONA_INTERPRETER_SHARING
        assert(gc_list_is_empty(&self->gc_list));
#endif
        PyObject *dict = NULL;
        Py_BEGIN_CRITICAL_SECTION(self);
        dict = self->dict;
        self->dict = NULL;
        Py_END_CRITICAL_SECTION();
        // An open region does not own its members. They live in the GC
        // generations and the usual reference counting disposes of them.
        Py_XDECREF(dict);
    } else {
        // Objects in a closed region have no incoming references besides the
        // one from the bridge. We can therefore delete all objects directly
        // instead of returning them to the GC.
        _region_delete_contents(self);
    }
}

static void
TracingRegion_dealloc(TracingRegionObject *self) {
    PyObject *op = (PyObject *)self;

    // `PyObject_CallFinalizerFromDealloc()` requires a GC type to be tracked
    // while the finalizer runs, but the bridge object of a closed region may
    // get untracked by an owning cown.
    if (!_PyObject_GC_IS_TRACKED(op)) {
        _PyObject_GC_TRACK(op);
    }
    if (PyObject_CallFinalizerFromDealloc(op) < 0) {
        // The bridge object was resurrected by the references from inside the
        // region. It is deallocated again once those are gone.
        return;
    }

    Py_BEGIN_CRITICAL_SECTION(self);
    // Make sure any objects added after/during finalization are freed
    Py_CLEAR(self->dict);
    Py_END_CRITICAL_SECTION();

    PyObject_GC_UnTrack(self);
    Py_TYPE(self)->tp_free(op);
}

static PyObject *
TracingRegion_repr(PyObject *op) {
    TracingRegionObject *self = _PyTRegion_CAST(op);

    bool is_open = false;
    Py_BEGIN_CRITICAL_SECTION(self);
    is_open = region_is_open(self);
    Py_END_CRITICAL_SECTION();

    // Deliberately reads `open` instead of going through the attribute access
    // below, so that reporting on a region does not open it. Deliberately
    // address free as well, so that error messages are reproducible.
    return PyUnicode_FromFormat(
        "<TracingRegion %s>", is_open ? "open" : "closed");
}

static PyObject *
TracingRegion_getattro(PyObject *op, PyObject *name) {
    TracingRegionObject *self = (TracingRegionObject*)op;
    PyObject *result = NULL;

    Py_BEGIN_CRITICAL_SECTION(self);
    _open_region_lock_held(self);

    result = _PyObject_GenericGetAttrWithDict(op, name, self->dict, 0);
    Py_END_CRITICAL_SECTION();

    return result;
}

static int
TracingRegion_setattro(PyObject *op, PyObject *name, PyObject *value) {
    TracingRegionObject *self = (TracingRegionObject*)op;
    int result = 0;

    Py_BEGIN_CRITICAL_SECTION(self);
    _open_region_lock_held(self);

    // Allocate lazily because the generic helper only stores into a provided dict.
    if (self->dict == NULL) {
        self->dict = PyDict_New();
    }

    if (self->dict == NULL) {
        result = -1;
    } else {
        result = _PyObject_GenericSetAttrWithDict(op, name, value, self->dict);
    }
    Py_END_CRITICAL_SECTION();

    return result;
}

static PyObject *
TracingRegion_get_dict(PyObject *op, void *Py_UNUSED(context)) {
    TracingRegionObject *self = (TracingRegionObject*)op;
    PyObject *dict = NULL;

    Py_BEGIN_CRITICAL_SECTION(self);
    _open_region_lock_held(self);

    if (self->dict == NULL) {
        self->dict = PyDict_New();
    }
    dict = Py_XNewRef(self->dict);
    Py_END_CRITICAL_SECTION();

    return dict;
}

static int
TracingRegion_set_dict(PyObject *op, PyObject *value, void *Py_UNUSED(context)) {
    TracingRegionObject *self = (TracingRegionObject*)op;
    int result = 0;

    Py_BEGIN_CRITICAL_SECTION(self);
    _open_region_lock_held(self);

    if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "cannot delete __dict__");
        result = -1;
    } else if (!PyDict_Check(value)) {
        PyErr_Format(PyExc_TypeError,
                     "__dict__ must be set to a dictionary, not a '%.200s'",
                     Py_TYPE(value)->tp_name);
        result = -1;
    } else {
        Py_XSETREF(self->dict, Py_NewRef(value));
    }
    Py_END_CRITICAL_SECTION();

    return result;
}


/* This method traces the region and closes it, if there are no references
 * pointing into the region. References to the bridge are allowed.
 *
 * This function requires the GIL to be held if _Py_PYRONA_INTERPRETER_SHARING
 * is defined.
 *
 * Returns -1 if an exception was raised. 0 if the region could be closed.
 */
int _PyTracingRegion_Close(PyObject* op) {
#ifdef _Py_PYRONA_INTERPRETER_SHARING
    TracingRegionObject *self = _PyTRegion_CAST(op);

    assert(gc_list_is_empty(&self->gc_list));
#endif

    return try_close_region_tree(op);
}

int _PyTracingRegion_IsClosed(PyObject* region) {
    TracingRegionObject *self = _PyTRegion_CAST(region);
    return !region_is_open(self);
}

/* Opens the region, so that a region reference can hand out a strong reference
 * into it. Only the sub-regions of this region stay closed.
 *
 * This function requires the GIL to be held, and the caller to have established
 * that this interpreter owns the region.
 */
void _PyTracingRegion_Open(PyObject* region) {
    _open_region(_PyTRegion_CAST(region));
}

_PyRegionRefMetadata *_PyTracingRegion_MetaLockHeld(PyObject* region) {
    return region_meta_lock_held(_PyTRegion_CAST(region));
}

void _PyTracingRegion_SetMetaCown(PyObject* region, PyObject* cown) {
    TracingRegionObject *self = _PyTRegion_CAST(region);
    // Meta is only set if the region is closed and has region references
    if (self->meta != NULL) {
        _PyRegionRef_MetaSetCown(self->meta, cown);
    }
}

void _PyTracingRegion_SetMetaOwner(PyObject* region, _PyCown_owner_id_t owner) {
    TracingRegionObject *self = _PyTRegion_CAST(region);
    if (self->meta != NULL) {
        _PyRegionRef_MetaSetIpid(self->meta, owner);
    }
}

static PyMethodDef TracingRegion_methods[] = {
    {NULL,              NULL}           /* sentinel */
};

static PyGetSetDef TracingRegion_getset[] = {
    {"__dict__", TracingRegion_get_dict, TracingRegion_set_dict},
    {NULL}
};

PyTypeObject _PyTracingRegion_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "TracingRegion",
    .tp_basicsize = sizeof(TracingRegionObject),
    .tp_dealloc = (destructor)TracingRegion_dealloc,
    .tp_repr = TracingRegion_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .tp_traverse = (traverseproc)TracingRegion_traverse,
    .tp_clear = (inquiry)TracingRegion_clear,
    .tp_getset = TracingRegion_getset,
    .tp_methods = TracingRegion_methods,
    .tp_getattro = TracingRegion_getattro,
    .tp_setattro = TracingRegion_setattro,
    .tp_init = (initproc)TracingRegion_init,
    .tp_new = TracingRegion_new,
    .tp_finalize = TracingRegion_finalize,
    .tp_reachable = _PyObject_ReachableVisitTypeAndTraverse,
};
#pragma endregion

/// This attempts to detach the region from the current interpreter and thread.
///
/// Raises an exception and returns -1 if it couldn't be detached.
int _PyTracingRegion_DetachIgnoreRegionRefs(PyObject* region) {
    assert(Region_Check(region));

    // Close the region
    int closing_res = _PyTracingRegion_Close(region);
    if (closing_res < 0) {
        return -1;
    }

    // # Soundness
    // Make sure that the cown owns the only external reference to the bridge object.
    //
    // This check is safe on NoGIL Python, since Cowns only permit the owning
    // thread from accessing the contained value. An RC of 1 indicates that the
    // cown holds the only reference and this thread is the only one that can
    // access that reference.
    TracingRegionObject *self = _PyTRegion_CAST(region);
#ifdef _Py_PYRONA_INTERPRETER_SHARING
    // We subtract the internal RCs for sub-interpreters
    Py_ssize_t external_rc = Py_REFCNT(region);
#else
    Py_ssize_t external_rc = Py_REFCNT(region) - self->internal_bridge_refs;
#endif
    if (external_rc > 1) {
        PyErr_Format(
            PyExc_RuntimeError,
            "the region couldn't be detached, due to incoming references to the bridge");
        return -1;
    }

    // Confirm that no other thread has opened the region again before the
    // singe external reference check above.
    if (region_is_open(self)) {
        PyErr_Format(
            PyExc_RuntimeError,
            "the region couldn't be detached due to a concurrent access");
        return -1;
    }

#ifdef _Py_PYRONA_INTERPRETER_SHARING
    // The region is closed and this is the only owner of the bridge. We untrack
    // from the current GC list.
    PyObject_GC_UnTrack(region);
#endif

    return 0;
}

/// This attempts to detach the region from the current interpreter and thread.
///
/// Raises an exception and returns -1 if it couldn't be detached.
int _PyTracingRegion_Detach(PyObject* region) {
    TracingRegionObject *self = _PyTRegion_CAST(region);

    if (_PyTracingRegion_DetachIgnoreRegionRefs(region)) {
        return -1;
    }

    // This is safe, assuming the region references respect the thread ID,
    // as that one prevents other threads and IPs from opening the chain under foot.
    if (self->meta != NULL) {
        _PyRegionRef_MetaSetReleased(self->meta);
    }

    return 0;
}

int _PyTracingRegion_AttachIgnoreRegionRefs(PyObject* region) {
    assert(Region_Check(region));
#ifdef _Py_PYRONA_INTERPRETER_SHARING
    assert(!PyObject_GC_IsTracked(region));
    PyObject_GC_Track(region);
#endif
    return 0;
}

int _PyTracingRegion_Attach(PyObject* region, _PyCown_owner_id_t owner) {
    TracingRegionObject *self = _PyTRegion_CAST(region);

    if (_PyTracingRegion_AttachIgnoreRegionRefs(region)) {
        return -1;
    }

    Py_BEGIN_CRITICAL_SECTION(self);
    if (self->meta != NULL) {
        _PyRegionRef_MetaSetIpid(self->meta, owner);
    }
    Py_END_CRITICAL_SECTION();

    return 0;
}
