
#include "Python.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include "pycore_descrobject.h"
#include "pycore_gc.h"
#include "pycore_immutability.h"
#include "pycore_object.h"
#include "pycore_ownership.h"
#include "pycore_list.h"
#include "pycore_region.h"

// #define IMMUTABLE_TRACING

#ifdef IMMUTABLE_TRACING
#define debug(msg, ...) \
   do { \
       printf(msg __VA_OPT__(,) __VA_ARGS__); \
   } while(0)
#define debug_obj(msg, obj, ...) \
   do { \
       PyObject* repr = PyObject_Repr(obj); \
       printf(msg, PyUnicode_AsUTF8(repr), obj __VA_OPT__(,) __VA_ARGS__); \
       Py_DECREF(repr); \
   } while(0)
#else
#define debug(...)
#define debug_obj(...)
#endif

// #define MERMAID_TRACING
#ifdef MERMAID_TRACING
#define TRACE_MERMAID_START() \
    do { \
        FILE* f = fopen("freeze_trace.md", "w"); \
        if (f != NULL) { \
            fprintf(f, "```mermaid\n"); \
            fprintf(f, "graph LR\n"); \
            fclose(f); \
        } \
    } while(0)

#define TRACE_MERMAID_NODE(obj) \
    do { \
        FILE* f = fopen("freeze_trace.md", "a"); \
        if (f != NULL) { \
            fprintf(f, "    %p[\"%s (rc=%zd) - %p\"]\n", \
                (void*)obj, (PyObject*)obj->ob_type->tp_name, \
                Py_REFCNT(obj), (void*)obj); \
            fclose(f); \
        } \
    } while(0)

#define TRACE_MERMAID_EDGE(from, to) \
    do { \
        FILE* f = fopen("freeze_trace.md", "a"); \
        if (f != NULL) { \
            fprintf(f, "    %p --> %p\n", (void*)from, (void*)to); \
            fclose(f); \
        } \
    } while(0)

#define TRACE_MERMAID_END() \
    do { \
        FILE* f = fopen("freeze_trace.md", "a"); \
        if (f != NULL) { \
            fprintf(f, "```\n"); \
            fclose(f); \
        } \
    } while(0)
#else
#define TRACE_MERMAID_START()
#define TRACE_MERMAID_NODE(obj)
#define TRACE_MERMAID_EDGE(from, to)
#define TRACE_MERMAID_END()
#endif

static PyObject *
_destroy(PyObject* set, PyObject *objweakref)
{
    Py_INCREF(set);
    if (PySet_Discard(set, objweakref) < 0) {
        Py_DECREF(set);
        return NULL;
    }
    Py_DECREF(set);

    Py_RETURN_NONE;
}

static PyMethodDef _destroy_def = {
    "_destroy", (PyCFunction) _destroy, METH_O
};

static PyObject *
type_weakref(struct _Py_immutability_state *state, PyObject *obj)
{
    if(state->destroy_cb == NULL){
        state->destroy_cb = PyCFunction_NewEx(&_destroy_def, state->freezable_types, NULL);
        if (state->destroy_cb == NULL) {
            return NULL;
        }
    }

    return PyWeakref_NewRef(obj, state->destroy_cb);
}

static
int init_state(struct _Py_immutability_state *state)
{
    state->freezable_types = PySet_New(NULL);
    if(state->freezable_types == NULL){
        return -1;
    }

    return 0;
}

// This is separate to the previous init as it depends on the traceback
// module being available, and can cause a circular import if it is
// called during register freezable.
#ifdef Py_DEBUG
static
void init_traceback_state(struct _Py_immutability_state *state)
{
    PyObject *traceback_module = PyImport_ImportModule("traceback");
    if (traceback_module != NULL) {
        state->traceback_func = PyObject_GetAttrString(traceback_module, "format_stack");
        Py_DECREF(traceback_module);
    }
}
#endif

static struct _Py_immutability_state* get_immutable_state(void)
{
    PyInterpreterState* interp = PyInterpreterState_Get();
    struct _Py_immutability_state *state = &interp->immutability;
    if(state->freezable_types == NULL){
        if(init_state(state) == -1){
            PyErr_SetString(PyExc_RuntimeError, "Failed to initialize immutability state");
            return NULL;
        }
    }

    return state;
}


PyDoc_STRVAR(notfreezable_doc,
    "NotFreezable()\n\
    \n\
    Indicate that a type cannot be frozen.");


PyTypeObject _PyNotFreezable_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "NotFreezable",
    .tp_doc = notfreezable_doc,
    .tp_basicsize = sizeof(PyObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_BASETYPE,
    .tp_new = PyType_GenericNew
};


static int push(PyObject* s, PyObject* item){
    if(item == NULL){
        return 0;
    }

    if(!PyList_Check(s)){
        PyErr_SetString(PyExc_TypeError, "Expected a list");
        return -1;
    }

    // Don't incref here, so that the algorithm doesn't have to account for the additional counts
    // from the dfs and pending.
    return _PyList_AppendTakeRef(_PyList_CAST(s), item);
}

// Returns a borrowed reference to the last item in the list.
static PyObject* peek(PyObject* s){
    PyObject* item;
    Py_ssize_t size = PyList_Size(s);
    if(size == 0){
        return NULL;
    }

    item = PyList_GetItem(s, size - 1);
    if(item == NULL){
        return NULL;
    }

    return item;
}

// Depend on internal list pop implementation to avoid
// unnecessary refcount operations.
static PyObject* pop(PyObject* s){
    PyObject* item;
    Py_ssize_t size = PyList_Size(s);
    if(size == 0){
        return NULL;
    }

    // The push doesn't incref, so can avoid the extra
    // incref/decref here by using the internal pop.
    item = _Py_ListPop((PyListObject *)s, size - 1);
    if(item == NULL){
        PyErr_SetString(PyExc_RuntimeError, "Internal error: Failed to pop from list");
        return NULL;
    }

    return item;
}

static bool is_c_wrapper(PyObject* obj){
    return PyCFunction_Check(obj) || Py_IS_TYPE(obj, &_PyMethodWrapper_Type) || Py_IS_TYPE(obj, &PyWrapperDescr_Type);
}

// TODO(regions): xFrednet: Make sure that all users of this fuction test for failure
static inline int _Py_SetImmutable(PyObject *op)
{
    if (_PyRegion_SignalImmutable(op)) {
        return -1;
    }

    if(op) {
#if SIZEOF_VOID_P > 4
        op->ob_flags |= _Py_IMMUTABLE_FLAG;
#else
        op->ob_refcnt |= _Py_IMMUTABLE_FLAG;
#endif

    }

    return 0;
}

/**
 * Used to track the state of an in progress freeze operation.
 *
 * TODO(Immutable):  This representation could mostly be done in the
 * GC header for the GIL enabled build.  Doing it externally works for
 * both builds, and we can optimize later.
 **/
struct FreezeState {
    // Used to track traversal order
    PyObject *dfs;
    // Used to track SCC to handle cycles during traversal
    PyObject *pending;

// TODO(Free-Threaded) Needed for Free-threaded build.
//   This is done in the old GC header for no-GIL builds.
#ifdef GIL_DISABLED
    // Used to represent SCCs with union find.
    // The representative reuse this information as follows.
    //  The bottom bit represents if this representative can reach mutable state.
    //  Second bit unset, pointer towards representative
    //  Second bit set, this is a representative
    //     Third bit set, this is a complete representative
    //        The higher bits represent the reference count for this scc
    //        that has not be discovered in the current free graph
    //     Third bit unset, this is a pending representative that is still being explored.
    //        The higher bits represent the reference count from outside this scc
    //        this is the total of rcs for current visited objects,
    //        minus any internal edges we have explored.
    _Py_hashtable_t *rep;

    // Forms cyclic singly linked list of elements of a scc.
    _Py_hashtable_t *next;
#endif
#ifdef Py_DEBUG
    // For debugging, track the stack trace of the freeze operation.
    PyObject* freeze_location;
#endif
#ifdef MERMAID_TRACING
    PyObject* start;
#endif
};

#define REPRESENTATIVE_FLAG 1
#define COMPLETE_FLAG 2
#define REFCOUNT_SHIFT 2

/*
    In GIL builds we use the _gc_prev and _gc_next fields to store SCC information:
    - The _gc_prev field stores either the rank of the SCC (if the SCC is a
      representative), or a pointer to the parent representative (if not).
      The Collecting bit on the prev field is used to distinguish between the two.
      We cannot use the finalizer flag as that needs to be preserved.
      We could have a situation where an object is frozen after having a finalizer
      run on it, and we do not want to run the finalizer again.
    - The _gc_next field stores the next object in the cyclic list of objects
      in the SCC.
*/
#define SCC_RANK_FLAG _PyGC_PREV_MASK_COLLECTING

int init_freeze_state(struct FreezeState *state)
{
    state->dfs = PyList_New(0);
    state->pending = PyList_New(0);
#ifdef GIL_DISABLED
    state->rep = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    state->next = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
#endif
#ifdef Py_DEBUG
    state->freeze_location = NULL;
#endif

    // TODO detect failure?
    return 0;
}

void deallocate_FreezeState(struct FreezeState *state)
{
#ifdef GIL_DISABLED
    _Py_hashtable_destroy(state->rep);
    _Py_hashtable_destroy(state->next);
#endif

    // We can't call the destructor directly as we didn't newref the objects
    // on push.
    //  TODO(Immutable): Implement a proper stack for borrowing RCs so this can
    //   be just a deallocation, and not need to traverse the stacks.
    while(PyList_Size(state->pending) > 0){
        pop(state->pending);
    }
    while(PyList_Size(state->dfs) > 0){
        pop(state->dfs);
    }

    Py_DECREF(state->dfs);
    Py_DECREF(state->pending);
}

void set_direct_rc(PyObject* obj)
{
#ifndef GIL_DISABLED
#if SIZEOF_VOID_P > 4
    obj->ob_flags = (obj->ob_flags & ~_Py_IMMUTABLE_MASK) | _Py_IMMUTABLE_DIRECT;
#else
    obj->ob_refcnt = (obj->ob_refcnt & ~_Py_IMMUTABLE_FLAG) | _Py_IMMUTABLE_DIRECT;
#endif
#else
    (void)obj;
#endif
}

void set_indirect_rc(PyObject* obj)
{
#ifndef GIL_DISABLED
#if SIZEOF_VOID_P > 4
    obj->ob_flags = (obj->ob_flags & ~_Py_IMMUTABLE_MASK) | _Py_IMMUTABLE_INDIRECT;
#else
    obj->ob_refcnt = (obj->ob_refcnt & ~_Py_IMMUTABLE_MASK) | _Py_IMMUTABLE_INDIRECT;
#endif
#else
    (void)obj;
#endif
}

bool has_direct_rc(PyObject* obj)
{
#ifdef GIL_DISABLED
    return false;
#elif SIZEOF_VOID_P > 4
    return (obj->ob_flags & _Py_IMMUTABLE_MASK) == _Py_IMMUTABLE_DIRECT;
#else
    return (obj->ob_refcnt & _Py_IMMUTABLE_FLAG) == _Py_IMMUTABLE_DIRECT;
#endif
}


int is_representative(PyObject* obj, struct FreezeState *state)
{
#ifdef GIL_DISABLED
    void* result = _Py_hashtable_get(state->rep, obj);
    return ((uintptr_t)result & REPRESENTATIVE_FLAG) != 0;
#else
    return (_Py_AS_GC(obj)->_gc_prev & SCC_RANK_FLAG) != 0;
#endif
}

void set_scc_parent(PyObject* obj, PyObject* parent)
{
    PyGC_Head* gc = _Py_AS_GC(obj);
    // Use GC space for the parent pointer.
    assert(((uintptr_t)parent & ~_PyGC_PREV_MASK) == 0);
    uintptr_t finalized_bit = gc->_gc_prev & _PyGC_PREV_MASK_FINALIZED;
    gc->_gc_prev = finalized_bit | _Py_CAST(uintptr_t, parent);
}

PyObject* scc_parent(PyObject* obj)
{
    // Use GC space for the parent pointer.
    assert((_Py_AS_GC(obj)->_gc_prev & SCC_RANK_FLAG) == 0);
    return _Py_CAST(PyObject*, _Py_AS_GC(obj)->_gc_prev & _PyGC_PREV_MASK);
}

void set_scc_rank(PyObject* obj, size_t rank)
{
    // Use GC space for the rank.
    _Py_AS_GC(obj)->_gc_prev = (rank << _PyGC_PREV_SHIFT) | SCC_RANK_FLAG;
}

size_t scc_rank(PyObject* obj)
{
    assert((_Py_AS_GC(obj)->_gc_prev & SCC_RANK_FLAG) == SCC_RANK_FLAG);
    // Use GC space for the rank.
    return _Py_AS_GC(obj)->_gc_prev >> _PyGC_PREV_SHIFT;
}

void set_scc_next(PyObject* obj, PyObject* next)
{
    debug("   set_scc_next %p -> %p\n", obj, next);
    // Use GC space for the next pointer.
    _Py_AS_GC(obj)->_gc_next = (uintptr_t)next;
}

PyObject* scc_next(PyObject* obj)
{
    // Use GC space for the next pointer.
    return _Py_CAST(PyObject*, _Py_AS_GC(obj)->_gc_next);
}

void scc_init_non_trivial(PyObject* obj)
{
    // Check if this not been part of an SCC yet.
    if (scc_next(obj) == NULL) {
        // Set up a new SCC with a single element.
        set_scc_rank(obj, 0);
        set_scc_next(obj, obj);
    }
}

void return_to_gc(PyObject* op)
{
    set_scc_next(op, NULL);
    set_scc_parent(op, NULL);
    // Use internal version as we don't satisfy all the invariants,
    // as we call this on state we are tearing down in SCC reclaiming.
    //    PyObject_GC_Track(op);
    _PyObject_GC_TRACK(op);
}

void scc_init(PyObject* obj)
{
    assert(_PyObject_IS_GC(obj));
    // Let the Immutable GC take over tracking the lifetime
    // of this object. This releases the space for the SCC
    // algorithm.
    if (_PyObject_GC_IS_TRACKED(obj)) {
        _PyObject_GC_UNTRACK(obj);
    }

#if SIZEOF_VOID_P > 4
    // Mark as pending so we can detect back edges in the traversal.
    obj->ob_flags |= _Py_IMMUTABLE_PENDING;
#else
    obj->ob_refcnt |= _Py_IMMUTABLE_PENDING;
#endif
    set_scc_rank(obj, 0);
}

bool scc_is_pending(PyObject* obj)
{
#if SIZEOF_VOID_P > 4
    return (obj->ob_flags & _Py_IMMUTABLE_MASK) == _Py_IMMUTABLE_PENDING;
#else
    return (obj->ob_refcnt & _Py_IMMUTABLE_MASK) == _Py_IMMUTABLE_PENDING;
#endif
}

PyObject* get_representative(PyObject* obj, struct FreezeState *state)
{
#ifdef GIL_DISABLED
    // TODO(Immutable): Union find path compressions.
    void* next = _Py_hashtable_get(state->rep, obj);
    while (((uintptr_t)next & REPRESENTATIVE_FLAG) == 0) {
        obj = (PyObject*)next;
        assert(obj != NULL);
        next = _Py_hashtable_get(state->rep, obj);
    }
    return obj;
#else
    if (is_representative(obj, state)) {
        return obj;
    }
    // Grandparent path compression for union find.
    PyObject* grandparent = obj;
    PyObject* rep = scc_parent(obj);
    while (1) {
        if (is_representative(rep, state)) {
            break;
        }

        PyObject* parent = rep;
        rep = scc_parent(rep);
        set_scc_parent(grandparent, rep);
        grandparent = parent;
    }
    return rep;
#endif
}

bool
union_scc(PyObject* a, PyObject* b, struct FreezeState *state)
{
    // Initialize SCC information for both objects.
    // If they are already in an SCC, this is a no-op.
    scc_init_non_trivial(a);
    scc_init_non_trivial(b);

    // TODO(Immutable): use rank and merge in correct direction.
    PyObject* rep_a = get_representative(a, state);
    PyObject* rep_b = get_representative(b, state);

    if (rep_a == rep_b)
        return false;

#ifndef GIL_DISABLED
    // Determine rank, and switch so that rep_a has higher rank.
    size_t rank_a = scc_rank(rep_a);
    size_t rank_b = scc_rank(rep_b);
    if (rank_a < rank_b) {
        PyObject* temp = rep_a;
        rep_a = rep_b;
        rep_b = temp;
    } else if (rank_a == rank_b) {
        // Increase rank of new representative.
        set_scc_rank(rep_a, rank_a + 1);
    }

    set_scc_parent(rep_b, rep_a);

    // Merge the cyclic lists.
    PyObject* next_a = scc_next(rep_a);
    PyObject* next_b = scc_next(rep_b);
    set_scc_next(rep_a, next_b);
    set_scc_next(rep_b, next_a);
    return true;
#else
    // TODO(Immutable): Can probably throw away SCC code for no-GIL build.

    void** value_a_ref = &(_Py_hashtable_get_entry(state->rep, rep_a)->value);
    void** value_b_ref = &(_Py_hashtable_get_entry(state->rep, rep_b)->value);

    uintptr_t value_a = (uintptr_t)*value_a_ref;
    uintptr_t value_b = (uintptr_t)*value_b_ref;

    // Combined the rc information for the two partial SCCs.
    // Subtract one from the rc as this corresponds to collapsing an internal edge in
    // an SCC
    size_t rc_a = (value_a >> REFCOUNT_SHIFT);
    size_t rc_b = (value_b >> REFCOUNT_SHIFT);
    size_t new_rc = rc_a + rc_b - 1;

    debug("Merging %p and %p, new rc = %zu\n", rep_a, rep_b, new_rc);

    // TODO(Immutable): Use rank to decide which way to merge.
    // Reparent
    *value_b_ref = rep_a;

    // Update root to combine values
    *value_a_ref = (void*)(REPRESENTATIVE_FLAG | (new_rc << REFCOUNT_SHIFT));

    // Merge cyclic lists.
    void* next_a = _Py_hashtable_get(state->next, rep_a);
    void* next_b = _Py_hashtable_get(state->next, rep_b);
    _Py_hashtable_get_entry(state->next, rep_a)->value = next_b;
    _Py_hashtable_get_entry(state->next, rep_b)->value = next_a;

    return true;
#endif
}

PyObject* get_next(PyObject* obj, struct FreezeState *state)
{
#ifdef GIL_DISABLED
    PyObject* next = (PyObject*)_Py_hashtable_get(state->next, obj);
    assert(next != NULL);
#else
    PyObject* next = scc_next(obj);
#endif
    return next;
}

int has_visited(struct FreezeState *state, PyObject* obj)
{
#ifdef GIL_DISABLED
    return _Py_hashtable_get(state->next, obj) != NULL;
#else
    return _Py_IsImmutable(obj);
#endif
}

#ifndef GIL_DISABLED
static PyObject* scc_root(PyObject* obj)
{
    assert(_Py_IsImmutable(obj));
    if (has_direct_rc(obj))
        return obj;

    // If the object is pending, then it is still being explored,
    // the final pass of the SCC algorithm will calculate the whole SCCs RC,
    // apply the ref count directly so we don't accidentally delete an object
    // that is still being explored.
    if (scc_is_pending(obj))
        return obj;

    PyObject* parent = scc_parent(obj);
    if (parent != NULL)
        return parent;

    assert(get_next(obj, NULL) == NULL);
    return obj;
}
#endif

void debug_print_scc(struct FreezeState *state, PyObject* start)
{
#ifdef IMMUTABLE_TRACING
    PyObject* rep = get_representative(start, state);
    PyObject* curr = rep;
    do
    {
        PyObject* next = get_next(curr, state);
        debug_obj("SCC member: %s (%p) rc=%zu\n", curr, _Py_REFCNT(curr));
        curr = next;
    } while (curr != rep);
#else
    (void)state;
    (void)start;
#endif
}

int debug_print_scc_visit(_Py_hashtable_t *ht, const void *key, const void *value, void *user_data)
{
#ifdef IMMUTABLE_TRACING
    struct FreezeState *state = (struct FreezeState *)user_data;
    // Only print representatives.
    if (!is_representative((PyObject*)key, state)) {
        return 0;
    }
    debug("----\n");
    PyObject* start = (PyObject*)key;
    debug_print_scc(state, start);
#else
    (void)ht;
    (void)key;
    (void)value;
    (void)user_data;
#endif
    return 0;
}

void debug_print_all_sccs(struct FreezeState *state)
{
#ifdef IMMUTABLE_TRACING
    // debug ("Printing all SCCs\n");
    // _Py_hashtable_foreach(state->rep, debug_print_scc_visit, state);
    // debug("----\n");
#else
    (void)state;
#endif
}

// During the freeze, we removed the reference counts associated
// with the internal edges of the SCC.  This visitor detects these
// internal edges and re-adds the reference counts to the
// objects in the SCC.
static int scc_add_internal_refcount_visit(PyObject* obj, void* curr_root)
{
    if (obj == NULL)
        return 0;

    // Ignore mutable outgoing edges.
    if (!_Py_IsImmutable(obj))
        return 0;

    // Find the scc root.
    PyObject* root = scc_root(obj);

    // If it is different SCC, then we can ignore it.
    if (root != curr_root)
        return 0;

    // Increase the reference count as we found an interior edge for the SCC.
    debug_obj("Reinstate %s (%p) with rc %zu from %p\n", obj, Py_REFCNT(obj), curr_root);
    obj->ob_refcnt++;

    return 0;
}

struct SCCDetails {
    int has_weakreferences;
    int has_legacy_finalizers;
    int has_finalizers;
};

static void scc_set_refcounts_to_one(PyObject* obj)
{
    PyObject* n = obj;
    do {
        PyObject* c = n;
        n = scc_next(c);
        c->ob_refcnt = 1;
    } while (n != obj);
}

static void scc_reset_root_refcount(PyObject* obj)
{
    assert(scc_root(obj) == obj);
    size_t scc_rc = _Py_REFCNT(obj) * 2;
    PyObject* n = obj;
    do {
        PyObject* c = n;
        n = scc_next(c);
        scc_rc -= _Py_REFCNT(c);
    } while (n != obj);
    obj->ob_refcnt = scc_rc;
}

// This will restore the reference counts for the interior edges of the SCC.
// It calculates some properites of the SCC, to decide how it might be
// finalised.  Adds an RC to every element in the SCC.
static void scc_add_internal_refcounts(PyObject* obj, struct SCCDetails* details)
{
    assert(_Py_IsImmutable(obj));
    PyObject* root = scc_root(obj);

    details->has_weakreferences = 0;
    details->has_legacy_finalizers = 0;
    details->has_finalizers = 0;

    // Add back the reference counts for the interior edges.
    PyObject* n = obj;
    do {
        debug_obj("Unfreezing %s @ %p\n", n);
        PyObject* c = n;
        n = scc_next(c);
        //  WARNING
        //  CHANGES HERE NEED TO BE REFLECTED IN freeze_visit

        if (PyType_Check(c)) {
            // TODO(Immutable): mjp: Special case for types not sure if required. We should review.
            PyTypeObject* type = (PyTypeObject*)obj;

            scc_add_internal_refcount_visit(type->tp_dict, root);
            scc_add_internal_refcount_visit(type->tp_mro, root);
            // We need to freeze the tuple object, even though the types
            // within will have been frozen already.
            scc_add_internal_refcount_visit(type->tp_bases, root);
        }
        else
        {
            traverseproc traverse = Py_TYPE(c)->tp_traverse;
            if (traverse != NULL) {
                traverse(c, (visitproc)scc_add_internal_refcount_visit, root);
            }
        }

        if (PyWeakref_Check(c)) {
            // We followed weakreferences during freeze, so need to here as well.
            PyObject* wr = NULL;
            PyWeakref_GetRef(c, &wr);
            if (wr != NULL) {
                // This will increment the reference if it is in the same SCC
                // and do nothing otherwise.  We are treating the weakref as
                // a strong reference for the immutable state.
                scc_add_internal_refcount_visit(wr, root);
                Py_DECREF(wr);
            }
            details->has_weakreferences++;
        }

        // The default tp_traverse will not visit the type object if it is
        // not heap allocated, so we need to do that manually here to freeze
        // the statically allocated types that are reachable.
        if (!(Py_TYPE(obj)->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
            scc_add_internal_refcount_visit(_PyObject_CAST(Py_TYPE(obj)), root);
        }

        if (Py_TYPE(c)->tp_del != NULL)
            details->has_legacy_finalizers++;
        if (Py_TYPE(c)->tp_finalize != NULL && !_PyGC_FINALIZED(c))
            details->has_finalizers++;
        if (_PyType_SUPPORTS_WEAKREFS(Py_TYPE(c)) &&
            *_PyObject_GET_WEAKREFS_LISTPTR_FROM_OFFSET(c) != NULL) {
            details->has_weakreferences++;
        }
    } while (n != obj);
}


// This takes an SCC and turns it back to mutable.
// Must be called after a call to
// scc_add_internal_refcount, so that the reference counts are correct.
static void scc_make_mutable(PyObject* obj)
{
    PyObject* n = obj;
    do {
        PyObject* c = n;
        n = scc_next(c);
        _Py_CLEAR_IMMUTABLE(c);
        if (PyWeakref_Check(c)) {
            PyObject* wr = NULL;
            PyWeakref_GetRef(c, &wr);
            if (wr != NULL) {
                // Turn back to weak reference. We made the weak references strong during freeze.
                Py_DECREF(wr);
                Py_DECREF(wr);
            }
        }
    } while (n != obj);
}

// Returns all the objects in the SCC to the Python cycle detector.
static void scc_return_to_gc(PyObject* obj, bool decref_required)
{
    PyObject* n = obj;
    do {
        PyObject* c = n;
        n = scc_next(c);
        return_to_gc(c);
        if (decref_required) {
            Py_DECREF(c);
        }
        debug_obj("Returned %s (%p) rc = %zu to GC\n", c, Py_REFCNT(c));
    } while (n != obj);
}

static void unfreeze(PyObject* obj)
{
    debug_obj("Unfreezing SCC starting at %s @ %p\n", obj);
    if (scc_next(obj) == NULL)
    {
        // Clear Immutable flags
        _Py_CLEAR_IMMUTABLE(obj);
        // Return to the GC.
        return_to_gc(obj);
        return;
    }
    debug_obj("Unfreezing %s @ %p\n", obj);
    // Note: We don't need the details of the SCC for a simple unfreeze.
    struct SCCDetails scc_details;
    scc_reset_root_refcount(obj);
    scc_add_internal_refcounts(obj, &scc_details);
    scc_make_mutable(obj);
    scc_return_to_gc(obj, true);
}


static void unfreeze_and_finalize_scc(PyObject* obj)
{
    struct SCCDetails scc_details;
    debug_obj("Unfreezing and finalizing SCC starting at %s @ %p rc = %zd\n", obj, Py_REFCNT(obj));

    scc_set_refcounts_to_one(obj);
    scc_add_internal_refcounts(obj, &scc_details);

    // These are cases that we don't handle.  Return the state as mutable to the
    // cycle detector to handle.
    // TODO(Immutable): Lift the weak references to be handled here.
    if (scc_details.has_weakreferences > 0 || scc_details.has_legacy_finalizers > 0) {
        debug("There are weak references or legacy finalizers in the SCC.  Let cycle detector handle this case.\n");
        debug("Weak references: %d, Legacy finalizers: %d\n", scc_details.has_weakreferences, scc_details.has_legacy_finalizers);
        scc_make_mutable(obj);
        scc_return_to_gc(obj, true);
        return;
    }

    // But leave cyclic list in place for the SCC.
    scc_make_mutable(obj);

    PyObject* n = obj;
    if (scc_details.has_finalizers) {
        // Call the finalizers for all objects in the SCC.
        do {
            PyObject* c = n;
            n = scc_next(c);
            if (_PyGC_FINALIZED(c))
                continue;
            destructor finalize = Py_TYPE(c)->tp_finalize;
            if (finalize == NULL)
                continue;
            // Call the finalizer for the object.
            finalize(c);
            // Mark so we don't finalize it again.
            _PyGC_SET_FINALIZED(c);
        } while (n != obj);
    }

    // tp_clear all elements in the cycle.
    n = obj;
    do {
        debug_obj("Clearing %s (%p)\n", n);
        PyObject* c = n;
        n = scc_next(c);
        inquiry clear;
        if ((clear = Py_TYPE(c)->tp_clear) != NULL) {
            clear(c);
            // TODO(Immutable): Should do something with the error? e.g.
            // if (_PyErr_Occurred(tstate)) {
            //     _PyErr_WriteUnraisableMsg("in tp_clear of",
            //                             (PyObject*)Py_TYPE(op));
            // }
        }
    } while (n != obj);
    // Return objects to the GC state, and drop reference counts on all the
    // elements of the SCC so that they can be reclaimed
    scc_return_to_gc(obj, true);
}


/**
 * The DFS walk for SCC calculations needs to perform actions on both
 * the pre-order and post-order visits to an object.  To achieve this
 * with a single stack we use a marker object (PostOrderMarker) to
 * indicate that the object being popped is a post-order visit.
 *
 * Effectively we do
 *   obj = pop()
 *   if obj is PostOrderMarker:
 *      obj = pop()
 *      post_order_action(obj)
 *   else:
 *      push(obj)
 *      push(PostOrderMarker)
 *      pre_order_action(obj)
 *
 * In pre_order_action, the children of obj can be pushed onto the stack,
 * and once all that work is completed, then the PostOrderMarker will pop out
 * and the post_order_action can be performed.
 *
 * Using a separate object means it cannot conflict with anything
 * in the actual python object graph.
 */
PyObject PostOrderMarkerStruct = _PyObject_HEAD_INIT(&_PyNone_Type);
static PyObject* PostOrderMarker = &PostOrderMarkerStruct;

/*
  When we first visit an object, we create a partial SCC for it,
  this involves:
    * Using the next table, to add it to a cyclic list for its SCC, initially just itself
    * Adding an entry in the representative table marking it as a representative
      that is pending (not complete) with refcount equal to its current refcount.

  Returns -1 if there was a memory error.
  Otherwise returns 0.
*/
int add_visited(PyObject* obj, struct FreezeState *state)
{
    assert (!has_visited(state, obj));

#ifdef Py_DEBUG
    // // We need to add this attribute before traversing, so that if it creates a
    // // dictionary, then this dictionary is frozen.
    // if (state->freeze_location != NULL) {
    //     // Some objects don't have attributes that can be set.
    //     // As this is a Debug only feature, we could potentially increase the object
    //     // size to allow this to be stored directly on the object.
    //     if (PyObject_SetAttrString(obj, "__freeze_location__", state->freeze_location) < 0) {
    //         // Ignore failure to set _freeze_location
    //         PyErr_Clear();
    //         // We still want to freeze the object, so we continue
    //     }
    // }
#endif
#ifdef GIL_DISABLED
    // Mark the object as visited
    if (_Py_hashtable_set(state->next, obj, obj) == -1)
        return -1;

    size_t rc = _Py_REFCNT(obj);
    void* rep_value = (void*)(REPRESENTATIVE_FLAG | (rc << REFCOUNT_SHIFT));

    debug_obj("Adding visited  %s (%p)\n", obj);
    // Use one to represent a pending SCC.
    if (_Py_hashtable_set(state->rep, obj, rep_value) == -1) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to add to hashtable, OOM?");
        return -1;
    }

    return 0;
#else
    debug_obj("Adding visited  %s (%p)\n", obj);
    if (_PyObject_IS_GC(obj))
    {
        scc_init(obj);
    } else {
        set_direct_rc(obj);
    }
    return 0;
#endif
}

/*
    Returns true if the object is part of an SCC that is still pending (not complete).
*/
int
is_pending(PyObject* obj, struct FreezeState *state)
{
#ifdef GIL_DISABLED
    PyObject* rep = get_representative(obj, state);
    uintptr_t result = (uintptr_t)_Py_hashtable_get(state->rep, rep);
    return (result & COMPLETE_FLAG) == 0;
#else
    return scc_is_pending(obj);
#endif
}

/*
    Marks the SCC for the given object as complete.

    Decrements the reference count for the SCC by one, corresponding to
    removing the reference from the edge that initially entered this
    SCC.

    Returns true if the SCC's reference count has become zero.
*/
void
complete_scc(PyObject* obj, struct FreezeState *state)
{
#ifdef GIL_DISABLED
    PyObject* rep = get_representative(obj, state);
    void** value_ref = &(_Py_hashtable_get_entry(state->rep, rep)->value);
    // Mark completed
    *value_ref = (void*)(((uintptr_t)*value_ref) | COMPLETE_FLAG);
    // Decrement reference count.
    *value_ref = (void*)((uintptr_t)*value_ref - (1 << REFCOUNT_SHIFT));
    return ((uintptr_t)*value_ref) >> REFCOUNT_SHIFT == 0;
#else
    PyObject* c = scc_next(obj);
    if (c == NULL) {
        debug_obj("Completing SCC %s (%p) with single member rc = %zd\n", obj, Py_REFCNT(obj));
        // This is not part of a cycle, just make it immutable.
        set_scc_parent(obj, NULL);
        set_direct_rc(obj);
        return;
    }
    size_t rc = Py_REFCNT(obj);
    size_t count = 1;
    while (c != obj)
    {
        debug("Adding %p to SCC %p\n", c, obj);
        rc += Py_REFCNT(c);
        // Set refcnt to zero, and mark as immutable indirect.
        set_indirect_rc(c);
        set_scc_parent(c, obj);
        c = scc_next(c);
        count++;
    }
    // We will have left an RC live for each element in the SCC, so
    // we need to remove that from the SCCs refcount.
    obj->ob_refcnt = rc - (count - 1);
    set_direct_rc(obj);
    // Clear the rank information as we don't need it anymore.
    // TODO use this for backtracking purposes?
    set_scc_parent(obj, NULL);
    debug_obj("Completed SCC %s (%p) with %zu members with rc %zu \n", obj, count, rc - (count - 1));
#endif
}

void add_internal_reference(PyObject* obj, struct FreezeState *state)
{
#ifdef GIL_DISABLED
    // TODO
#else
    obj->ob_refcnt--;
    debug_obj("Decrementing rc of %s (%p) to %zd\n", obj, _Py_REFCNT(obj));
    assert(_Py_REFCNT(obj) > 0);
#endif
}

/*
  Function for use in _Py_hashtable_foreach.
  Marks the key as immutable/frozen.
*/
int mark_frozen(_Py_hashtable_t*, const void* key, const void*, void*)
{
    // Mark as frozen, this can only reach immutable objects so safe.
    _Py_SetImmutable((PyObject*)key);
    return 0;
}

/*
  Marks all the objects visited by the freeze operation as frozen.
*/
void mark_all_frozen(struct FreezeState *state)
{
#ifdef GIL_DISABLED
    _Py_hashtable_foreach(state->rep, mark_frozen, state);
#endif
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

    debug("Shadowing builtins for function %s (%p)\n", f->func_name, f);
    debug(" Original builtins: %p\n", builtins);
    debug(" Shadow builtins:   %p\n", shadow_builtins);

    shadow_globals = PyDict_New();
    if(shadow_globals == NULL){
        goto nomemory;
    }
    debug("Shadowing globals for function %s (%p)\n", f->func_name, f);
    debug(" Original globals: %p\n", globals);
    debug(" Shadow globals:   %p\n", shadow_globals);

    if(PyDict_SetItemString(shadow_globals, "__builtins__", Py_NewRef(shadow_builtins))){
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
            debug(" Copying global %s -> %p\n", PyUnicode_AsUTF8(name), value);
            if(PyDict_SetItem(shadow_globals, Py_NewRef(name), Py_NewRef(value))){
                Py_DECREF(shadow_builtins);
                Py_DECREF(shadow_globals);
                return 0;
            }
        }else if(PyDict_Contains(builtins, name)){
            PyObject* value = PyDict_GetItem(builtins, name);
            debug(" Copying builtin %s -> %p\n", PyUnicode_AsUTF8(name), value);
            if(PyDict_SetItem(shadow_builtins, Py_NewRef(name), Py_NewRef(value))){
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
                debug(" Copying global %s -> %p\n", PyUnicode_AsUTF8(name), value);
                if(PyDict_SetItem(shadow_globals, Py_NewRef(name), Py_NewRef(value))){
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
                debug(" Copying global %s -> %p\n", PyUnicode_AsUTF8(name), value);
                if(PyDict_SetItem(shadow_globals, Py_NewRef(name), Py_NewRef(value))){
                    Py_DECREF(shadow_builtins);
                    Py_DECREF(shadow_globals);
                    return 0;
                }
            }
        }
    }

    if(f->func_annotations == NULL){
        f->func_annotations = PyDict_New();
        if(f->func_annotations == NULL){
            goto nomemory;
        }
    }

    f->func_globals = shadow_globals;
    f->func_builtins = shadow_builtins;

    return 0;

nomemory:
    Py_XDECREF(shadow_builtins);
    Py_XDECREF(shadow_globals);
    PyErr_NoMemory();
    return -1;
}

static int freeze_visit(PyObject* obj, void* freeze_state_untyped)
{
    struct FreezeState* freeze_state = (struct FreezeState *)freeze_state_untyped;
    PyObject* dfs = freeze_state->dfs;
    if (obj == NULL) {
        return 0;
    }

    if (_Py_IsImmutable(obj) && !is_pending(obj, NULL)) {
        return 0;
    }

    debug_obj("-> %s (%p) rc=%zu\n", obj, Py_REFCNT(obj));

    TRACE_MERMAID_EDGE(freeze_state->start, obj);

    if(push(dfs, obj)){
        PyErr_NoMemory();
        return -1;
    }

    return 0;
}

int is_shallow_immutable(PyObject* obj)
{
    if (obj == NULL)
        return 0;

    if (Py_IS_TYPE(obj, &PyBool_Type) ||
        Py_IS_TYPE(obj, &_PyNone_Type) ||
        Py_IS_TYPE(obj, &PyLong_Type) ||
        Py_IS_TYPE(obj, &PyFloat_Type) ||
        Py_IS_TYPE(obj, &PyComplex_Type) ||
        Py_IS_TYPE(obj, &PyBytes_Type) ||
        Py_IS_TYPE(obj, &PyUnicode_Type) ||
        Py_IS_TYPE(obj, &PyTuple_Type) ||
        Py_IS_TYPE(obj, &PyFrozenSet_Type) ||
        Py_IS_TYPE(obj, &PyRange_Type) ||
        Py_IS_TYPE(obj, &PyCode_Type) ||
        Py_IS_TYPE(obj, &PyCFunction_Type) ||
        Py_IS_TYPE(obj, &PyCMethod_Type)
    ) {
        return 1;
    }

    // Types may be immutable, check flag.
    if (PyType_Check(obj))
    {
        PyTypeObject* type = (PyTypeObject*)obj;
        // Assume immutable types are safe to freeze.
        if (type->tp_flags & Py_TPFLAGS_IMMUTABLETYPE) {
            return 1;
        }
    }

    // TODO: Add user defined shallow immutable property
    return 0;
}

static bool
is_freezable_builtin(PyTypeObject *type)
{
    if(
        type == &PyType_Type ||
        type == &PyBaseObject_Type ||
        type == &PyFunction_Type ||
        type == &_PyNone_Type ||
        type == &PyBool_Type ||
        type == &PyLong_Type ||
        type == &PyFloat_Type ||
        type == &PyComplex_Type ||
        type == &PyBytes_Type ||
        type == &PyUnicode_Type ||
        type == &PyTuple_Type ||
        type == &PyList_Type ||
        type == &PyDict_Type ||
        type == &PySet_Type ||
        type == &PyFrozenSet_Type ||
        type == &PyMemoryView_Type ||
        type == &PyByteArray_Type ||
        type == &PyRange_Type ||
        type == &PyGetSetDescr_Type ||
        type == &PyMemberDescr_Type ||
        type == &PyProperty_Type ||
        type == &PyWrapperDescr_Type ||
        type == &PyMethodDescr_Type ||
        type == &PyClassMethod_Type || // TODO(Immutable): mjp I added this, is it correct? Discuss with maj
        type == &PyClassMethodDescr_Type ||
        type == &PyMethod_Type ||
        type == &PyCFunction_Type ||
        type == &PyCapsule_Type ||
        type == &PyCode_Type ||
        type == &PyCell_Type ||
        type == &PyFrame_Type ||
        type == &_PyWeakref_RefType ||
        type == &_PyNotImplemented_Type || // TODO(Immutable): mjp I added this, is it correct? Discuss with maj
        type == &PyModule_Type || // TODO(Immutable): mjp I added this, is it correct? Discuss with maj
        type == &PyImmModule_Type ||
        type == &PyEllipsis_Type
     )
     {
         return true;
     }

     return false;
}

static int
is_explicitly_freezable(struct _Py_immutability_state *state, PyObject *obj)
{
    int result = 0;
    PyObject *ref = type_weakref(state, (PyObject *)obj->ob_type);
    if(ref == NULL){
        return -1;
    }

    result = PySet_Contains(state->freezable_types, ref);
    Py_DECREF(ref);
    return result;
}


static int check_freezable(struct _Py_immutability_state *state, PyObject* obj)
{
    debug_obj("check_freezable  %s (%p)\n", obj);

    // Check is object is subclass of NotFreezable
    // TODO: Would be nice for this to be faster.
    if (PyObject_IsInstance(obj, (PyObject *)&_PyNotFreezable_Type) == 1){
        goto error;
    }

    if(is_freezable_builtin(obj->ob_type)){
        return 0;
    }

    int result = is_explicitly_freezable(state, obj);
    if(result == -1){
        return -1;
    }
    else if(result == 1){
        return 0;
    }

    // TODO(Immutable): Visit what the right balance of making Python types immutable is.
    if(!_PyType_HasExtensionSlots(obj->ob_type)){
        return 0;
    }

error:
    debug_obj("Not freezable  %s (%p)\n", obj);
    PyObject* error_msg = PyUnicode_FromFormat(
        "Cannot freeze instance of type %s",
        (obj->ob_type->tp_name));
    PyErr_SetObject(PyExc_TypeError, error_msg);
    return -1;
}


int _PyImmutability_RegisterFreezable(PyTypeObject* tp)
{
    PyObject *ref;
    int result;
    struct _Py_immutability_state *state = get_immutable_state();
    if(state == NULL){
        PyErr_SetString(PyExc_RuntimeError, "Failed to initialize immutability state");
        return -1;
    }

    ref = type_weakref(state, (PyObject*)tp);
    if(ref == NULL){
        return -1;
    }

    result = PySet_Add(state->freezable_types, ref);
    Py_DECREF(ref);
    return result;
}

// Perform a decref on an immutable object
// returns true if the object should be deallocated.
int _Py_DecRef_Immutable(PyObject *op)
{
    // Decrement the reference count of an immutable object without
    // deallocating it.
    assert(_Py_IsImmutable(op));

#ifdef Py_GIL_DISABLED
    // Put the clear code in DecRefShared.
    _Py_DecRefShared(op);
    return false;
#else

    // Find SCC if required.
    op = scc_root(op);

#if SIZEOF_VOID_P > 4

    Py_ssize_t old = _Py_atomic_add_ssize(&op->ob_refcnt_full, -1);
    // The ssize_t might be too big, so mask to 32 bits as that is the size of
    // ob_refcnt.
    old = old & 0xFFFFFFFF;
#else
    // TODO(Immutable 32): Find SCC if required.

    Py_ssize_t old = _Py_atomic_add_ssize(&op->ob_refcnt, -1);
    old = _Py_IMMUTABLE_FLAG_CLEAR(old);
#endif
    assert(old > 0);

    if (old != 1) {
        assert(_Py_IMMUTABLE_FLAG_CLEAR(op->ob_refcnt) != 0);
        // Context does not to dealloc this object.
        return false;
    }

    debug("DecRef reached zero for immutable %p of type %s\n",  op, op->ob_type->tp_name);

    assert(_Py_IMMUTABLE_FLAG_CLEAR(op->ob_refcnt) == 0);

    if (PyObject_IS_GC(op)) {
        if (scc_next(op) != NULL) {
            // This is part of an SCC, so we need to turn it back into mutable state,
            // and correctly re-establish RCs.
            unfreeze_and_finalize_scc(op);
            return false;
        }
        // This is a GC object, so we need to put it back on the GC list.
        debug("Returning to GC simple case %p\n", op);
        return_to_gc(op);
    }

    _Py_CLEAR_IMMUTABLE(op);

    if (PyWeakref_Check(op)) {
        debug("Handling weak reference %p\n", op);
        PyObject* wr;
        int res = PyWeakref_GetRef(op, &wr);
        if (res == 1) {
            // Make the weak reference weak.
            // Get ref increments the refcount, so we need to decref twice.
            Py_DECREF(wr);
            Py_DECREF(wr);
        }
        // TODO: Don't know how to handle failure here.  It should never happen,
        // as the reference was made strong during freezing.
    }

    return true;
#endif
}

// _Py_RefcntAdd_Immutable(op, 1);
void _Py_RefcntAdd_Immutable(PyObject *op, Py_ssize_t increment)
{
    assert(_Py_IsImmutable(op));
    op = scc_root(op);

    // Increment the reference count of an immutable object.
    assert(_Py_IsImmutable(op));
#if SIZEOF_VOID_P > 4
    _Py_atomic_add_ssize(&op->ob_refcnt_full, increment);
#else
    _Py_atomic_add_ssize(&op->ob_refcnt, increment);
#endif
}


// Macro that jumps to error, if the expression `x` does not succeed.
#define SUCCEEDS(x) { do { int r = (x); if (r != 0) goto error; } while (0); }

int traverse_freeze(PyObject* obj, struct FreezeState* freeze_state)
{
    //  WARNING
    //  CHANGES HERE NEED TO BE REFLECTED IN freeze_visit

#ifdef MERMAID_TRACING
    freeze_state->start = obj;
    TRACE_MERMAID_NODE(obj);
#endif

    debug_obj("%s (%p) rc=%zd\n", obj, Py_REFCNT(obj));

    if(_PyOwnership_is_c_wrapper(obj)) {
        set_direct_rc(obj);
        // C functions are not mutable
        // Types are manually traversed
        return 0;
    }

    PyObject *attr = NULL;
    if (PyObject_GetOptionalAttr(obj, &_Py_ID(__freezable__), &attr) == 1
        && Py_IsFalse(attr))
    {
        PyErr_Format(
            PyExc_TypeError,
            "A object of type %s is marked as unfreezable",
            Py_TYPE(obj)->tp_name);
        Py_XDECREF(attr);
        return -1;
    }
    Py_XDECREF(attr);

    attr = NULL;
    if (PyObject_GetOptionalAttr(obj, &_Py_ID(__pre_freeze__), &attr) == 1)
    {
        PyErr_SetString(PyExc_TypeError, "Pre-freeze hocks are currently WIP");
        Py_XDECREF(attr);
        return -1;
    }
    Py_XDECREF(attr);

    // Function require some work to freeze, so we do not freeze the
    // world as they mention globals and builtins.  This will shadow what they
    // use, and then we can freeze the those components.
    if(PyFunction_Check(obj)){
        SUCCEEDS(shadow_function_globals(obj));
    }

    if (PyModule_Check(obj)) {
        SUCCEEDS(_Py_module_freeze_hook(obj));
    }

    if(PyType_Check(obj)){
        // TODO(Immutable): mjp: Special case for types not sure if required. We should review.
        PyTypeObject* type = (PyTypeObject*)obj;

        SUCCEEDS(freeze_visit(type->tp_dict, freeze_state));
        SUCCEEDS(freeze_visit(type->tp_mro, freeze_state));
        // We need to freeze the tuple object, even though the types
        // within will have been frozen already.
        SUCCEEDS(freeze_visit(type->tp_bases, freeze_state));
    }
    else
    {
        traverseproc traverse = Py_TYPE(obj)->tp_traverse;
        if(traverse != NULL){
            SUCCEEDS(traverse(obj, (visitproc)freeze_visit, freeze_state));
        }
    }

    // Weak references are not followed by the GC, but should be
    // for immutability.  Otherwise, we could share mutable state
    // using a weak reference.
    if (PyWeakref_Check(obj)) {
        // Make the weak reference strong.
        // Get Ref increments the refcount.
        PyObject* wr;
        int res = PyWeakref_GetRef(obj, &wr);
        if (res == -1) {
            goto error;
        }
        if (res == 1) {
            if (freeze_visit(wr, freeze_state)) {
                goto error;
            }
        }
    }

    // The default tp_traverse will not visit the type object if it is
    // not heap allocated, so we need to do that manually here to freeze
    // the statically allocated types that are reachable.
    if (!(Py_TYPE(obj)->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        SUCCEEDS(freeze_visit(_PyObject_CAST(Py_TYPE(obj)), freeze_state));
    }

    return 0;
error:
    return -1;
}

// Main entry point to freeze an object and everything it can reach.
int _PyImmutability_Freeze(PyObject* obj)
{
    if(_Py_IsImmutable(obj)){
        return 0;
    }
    int result = 0;
    TRACE_MERMAID_START();

#ifdef Py_DEBUG
    // This has to be declared early to support the `Py_XDECREF` if any of the
    // `SUCCEEDS` fails
    PyObject* freeze_location = NULL;
#endif

    // Enable the invariant. It has to be enabled at the beginning to allow
    // reentry and failure in internal calls.
    SUCCEEDS(_PyOwnership_invariant_enable());
    // This function incrementally marks new objects as frozen. During this
    // process it is possible that frozen objects point to mutable ones. This
    // therefore needs to pause the invariant. Otherwise we might get an
    // exception when freezing calls into Python and triggers the invariant.
    SUCCEEDS(_PyOwnership_invariant_pause());

    struct FreezeState freeze_state;
    // Initialize the freeze state
    SUCCEEDS(init_freeze_state(&freeze_state));
    struct _Py_immutability_state* imm_state = get_immutable_state();
    if(imm_state == NULL){
        goto error;
    }

#ifdef Py_DEBUG
    // In debug mode, we can set a freeze location for debugging purposes.
    // Get a traceback object to use as the freeze location.
    if (imm_state->traceback_func == NULL) {
        init_traceback_state(imm_state);
    }

    if (imm_state->traceback_func != NULL) {
        PyObject *stack = PyObject_CallFunctionObjArgs(imm_state->traceback_func, NULL);
        if (stack != NULL) {
            // Add the type name to the top of the stack, can be useful.
            PyObject* typename = PyObject_GetAttrString(_PyObject_CAST(Py_TYPE(obj)), "__name__");
            push(stack, typename);
            freeze_state.freeze_location = stack;
        }
    }
#endif

    SUCCEEDS(push(freeze_state.dfs, obj));

    while (PyList_Size(freeze_state.dfs) != 0) {
        PyObject* item = pop(freeze_state.dfs);

        if (item == PostOrderMarker) {
            item = pop(freeze_state.dfs);

            // Have finished traversing graph reachable from item
            PyObject* current_scc = peek(freeze_state.pending);
            if (item == current_scc)
            {
                debug("Completed an SCC\n");
                pop(freeze_state.pending);
                debug_obj("Representative: %s (%p)\n", item);

                // Completed an SCC do the calculation here.
                complete_scc(item, &freeze_state);
            }
            continue;
        }

        if (has_visited(&freeze_state, item)) {
            debug_obj("Already visited: %s (%p)\n", item);
            // Check if it is pending.
            if (is_pending(item, &freeze_state)) {
                while (union_scc(peek(freeze_state.pending), item, &freeze_state)) {
                    debug_obj("Representative: %s (%p)\n", peek(freeze_state.pending));
                    pop(freeze_state.pending);
                }
                // This is an SCC internal edge, we will need to remove
                // it from the internal RC count.
                add_internal_reference(item, &freeze_state);
            }
            continue;
        }

        // New object, check if freezable
        SUCCEEDS(check_freezable(imm_state, item));

        // Add to visited before putting in internal datastructures, so don't have
        // to account of internal RC manipulations.
        add_visited(item, &freeze_state);

        if (_PyObject_IS_GC(item)) {
            // Add postorder step to dfs.
            SUCCEEDS(push(freeze_state.dfs, item));
            SUCCEEDS(push(freeze_state.dfs, PostOrderMarker));
            // Add to the SCC path
            SUCCEEDS(push(freeze_state.pending, item));
        }


        // Traverse the fields of the current object to add to the dfs.
        SUCCEEDS(traverse_freeze(item, &freeze_state));
    }

    // TODO: This disables the self-contained check.
    // should remove properly

    // Every scc we encountered should be closed.
    // if (closed_sccs != total_sccs)
    // {
    //     // Account for initial stack reference that was passed in.
    //     if (add_internal_reference(obj, &freeze_state))
    //     {
    //         closed_sccs++;
    //     }
    //     if (closed_sccs != total_sccs)
    //     {
    //         debug("Failed to freeze!\n");
    //         debug_print_all_sccs(&freeze_state);
    //         debug("Total SCCs %d, closed SCCs %d\n", total_sccs, closed_sccs);
    //         // set an error here.
    //         PyErr_SetString(PyExc_RuntimeError, "Cannot freeze a graph that is not self-contained!");
    //         goto error;
    //     }
    // }
    // Mark all the objects as frozen
    mark_all_frozen(&freeze_state);

    goto finally;

error:
    debug("Error during freeze, unfreezing all frozen objects\n");
    while(PyList_Size(freeze_state.pending) != 0){
        PyObject* item = pop(freeze_state.pending);
        if(item == NULL){
            return -1;
        }
        unfreeze(item);
    }
    result = -1;

finally:
    deallocate_FreezeState(&freeze_state);
    TRACE_MERMAID_END();
    return result;
}
