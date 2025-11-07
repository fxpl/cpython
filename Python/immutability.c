
#include "Python.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include "pycore_descrobject.h"
#include "pycore_gc.h"
#include "pycore_object.h"
#include "pycore_immutability.h"
#include "pycore_list.h"

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
    PyObject* frozen_importlib = NULL;

    frozen_importlib = PyImport_ImportModule("_frozen_importlib");
    if(frozen_importlib == NULL){
        return -1;
    }

    state->module_locks = PyObject_GetAttrString(frozen_importlib, "_module_locks");
    if(state->module_locks == NULL){
        Py_DECREF(frozen_importlib);
        return -1;
    }

    state->blocking_on = PyObject_GetAttrString(frozen_importlib, "_blocking_on");
    if(state->blocking_on == NULL){
        Py_DECREF(frozen_importlib);
        return -1;
    }

    state->freezable_types = PySet_New(NULL);
    if(state->freezable_types == NULL){
        Py_DECREF(frozen_importlib);
        return -1;
    }

    Py_DECREF(frozen_importlib);

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

static PyObject* pop(PyObject* s){
    PyObject* item;
    Py_ssize_t size = PyList_Size(s);
    if(size == 0){
        return NULL;
    }

    item = PyList_GetItem(s, size - 1);
    if(item == NULL){
        return NULL;
    }
    // SetSlice will decref the item removed,
    // but we didn't incref when pushing, so newref it
    // here to balance the refcount.
    // TODO(Immutable): Optimize by avoiding the newref/decf pair.
    Py_NewRef(item);
    if(PyList_SetSlice(s, size - 1, size, NULL)){
        return NULL;
    }

    return item;
}

static bool is_c_wrapper(PyObject* obj){
    return PyCFunction_Check(obj) || Py_IS_TYPE(obj, &_PyMethodWrapper_Type) || Py_IS_TYPE(obj, &PyWrapperDescr_Type);
}

static inline void _Py_SetImmutable(PyObject *op)
{
    if(op) {
#if SIZEOF_VOID_P > 4
        op->ob_flags |= _Py_IMMUTABLE_FLAG;
#else
        op->ob_refcnt |= _Py_IMMUTABLE_FLAG;
#endif
    }
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

#ifdef Py_DEBUG
    // For debugging, track the stack trace of the freeze operation.
    PyObject* freeze_location;
#endif
};

#define MARKED_MUTABLE_REACHABLE_FLAG 1
#define REPRESENTATIVE_FLAG 2
#define COMPLETE_FLAG 4
#define REFCOUNT_SHIFT 3

int init_freeze_state(struct FreezeState *state)
{
    state->dfs = PyList_New(0);
    state->pending = PyList_New(0);
    state->rep = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    state->next = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
#ifdef Py_DEBUG
    state->freeze_location = NULL;
#endif

    // TODO detect failure?
    return 0;
}

void deallocate_FreezeState(struct FreezeState *state)
{
    _Py_hashtable_destroy(state->rep);
    _Py_hashtable_destroy(state->next);

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

int is_representative(PyObject* obj, struct FreezeState *state)
{
    void* result = _Py_hashtable_get(state->rep, obj);
    return ((uintptr_t)result & REPRESENTATIVE_FLAG) != 0;
}

PyObject* get_representative(PyObject* obj, struct FreezeState *state)
{
    // TODO(Immutable): Union find path compressions.
    void* next = _Py_hashtable_get(state->rep, obj);
    while (((uintptr_t)next & REPRESENTATIVE_FLAG) == 0) {
        obj = (PyObject*)next;
        assert(obj != NULL);
        next = _Py_hashtable_get(state->rep, obj);
    }
    return obj;
}

bool
union_scc(PyObject* a, PyObject* b, struct FreezeState *state)
{
    // TODO(Immutable): use rank and merge in correct direction.
    PyObject* rep_a = get_representative(a, state);
    PyObject* rep_b = get_representative(b, state);

    if (rep_a == rep_b)
        return false;

    void** value_a_ref = &(_Py_hashtable_get_entry(state->rep, rep_a)->value);
    void** value_b_ref = &(_Py_hashtable_get_entry(state->rep, rep_b)->value);

    uintptr_t value_a = (uintptr_t)*value_a_ref;
    uintptr_t value_b = (uintptr_t)*value_b_ref;

    // If any either part of the SCC has reached a mutable object, then the combined
    // one can reach mutable objects.
    uintptr_t reach_mutable_a = value_a & MARKED_MUTABLE_REACHABLE_FLAG;
    uintptr_t reach_mutable_b = value_b & MARKED_MUTABLE_REACHABLE_FLAG;
    uintptr_t reach_mutable = reach_mutable_a | reach_mutable_b;


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
    *value_a_ref = (void*)(reach_mutable | REPRESENTATIVE_FLAG | (new_rc << REFCOUNT_SHIFT));

    // Merge cyclic lists.
    void* next_a = _Py_hashtable_get(state->next, rep_a);
    void* next_b = _Py_hashtable_get(state->next, rep_b);
    _Py_hashtable_get_entry(state->next, rep_a)->value = next_b;
    _Py_hashtable_get_entry(state->next, rep_b)->value = next_a;

    return true;
}

PyObject* get_next(PyObject* obj, struct FreezeState *state)
{
    void* next = _Py_hashtable_get(state->next, obj);
    assert(next != NULL);
    return (PyObject*)next;
}

int has_visited(struct FreezeState *state, PyObject* obj)
{
    return _Py_hashtable_get(state->next, obj) != NULL;
}

void debug_print_scc(struct FreezeState *state, PyObject* start)
{
#ifdef IMMUTABLE_TRACING
    PyObject* rep = get_representative(start, state);
    PyObject* curr = rep;
    do
    {
        PyObject* next = get_next(curr, state);
        debug_obj("SCC member: %s (%p) rc=%d\n", curr, _Py_REFCNT(curr));
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
    debug ("Printing all SCCs\n");
    _Py_hashtable_foreach(state->rep, debug_print_scc_visit, state);
    debug("----\n");
#else
    (void)state;
#endif
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
}

/*
    Returns true if the object is part of an SCC that is still pending (not complete).
*/
int
is_pending(PyObject* obj, struct FreezeState *state)
{
    PyObject* rep = get_representative(obj, state);
    uintptr_t result = (uintptr_t)_Py_hashtable_get(state->rep, rep);
    return (result & COMPLETE_FLAG) == 0;
}

/*
    Marks the SCC for the given object as complete.
    
    Decrements the reference count for the SCC by one, corresponding to
    removing the reference from the edge that initially entered this
    SCC.
    
    Returns true if the SCC's reference count has become zero.
*/
bool
complete_scc(PyObject* obj, struct FreezeState *state)
{
    PyObject* rep = get_representative(obj, state);
    void** value_ref = &(_Py_hashtable_get_entry(state->rep, rep)->value);
    // Mark completed
    *value_ref = (void*)(((uintptr_t)*value_ref) | COMPLETE_FLAG);
    // Decrement reference count.
    *value_ref = (void*)((uintptr_t)*value_ref - (1 << REFCOUNT_SHIFT));
    return ((uintptr_t)*value_ref) >> REFCOUNT_SHIFT == 0;
}

/*
    Returns true if the given objects SCC cannot reach any state that
    was mutable at the start of the freeze operation.
*/
bool is_marked_implicitly_immutable(PyObject* obj, struct FreezeState *state)
{
    PyObject* rep = get_representative(obj, state);
    assert(rep != NULL);

    _Py_hashtable_entry_t* entry = _Py_hashtable_get_entry(state->rep, rep);
    assert(entry != NULL);
    return ((uintptr_t)entry->value & MARKED_MUTABLE_REACHABLE_FLAG) == 0;
}


bool mark_not_implicitly_immutable(PyObject* obj, struct FreezeState *state)
{
    PyObject* rep = get_representative(obj, state);
    
    // Mark the representative as not implicitly immutable
    _Py_hashtable_entry_t* entry = _Py_hashtable_get_entry(state->rep, rep);
    assert(entry != NULL);
    if (((uintptr_t)entry->value & MARKED_MUTABLE_REACHABLE_FLAG) == 0) {
        entry->value = (void*)((uintptr_t)entry->value | MARKED_MUTABLE_REACHABLE_FLAG);
        return false;
    }
    return true;
}

/*
    Mark all the pending SCCs as not implicitly immutable.
*/
void mark_pending_not_implicitly_immutable(struct FreezeState *state)
{
    Py_ssize_t size = PyList_Size(state->pending);
    if(size == 0){
        return;
    }

    for (int i = size - 1; i >= 0; i--) {
        PyObject* item = PyList_GetItem(state->pending, i);
        if (mark_not_implicitly_immutable(item, state))
        {
            // Invariant: If there is a mark in pending, all higher things
            // will also be marked.
            // Hence, early return if it was already marked.
            return;
        }
    }
}

// Returns true if this reference makes the SCC self contained.
bool add_internal_reference(PyObject* obj, struct FreezeState *state)
{
    PyObject* rep = get_representative(obj, state);
    void** value = &(_Py_hashtable_get_entry(state->rep, rep)->value);
    *value = (void*)((uintptr_t)*value - (1 << REFCOUNT_SHIFT));

    debug_obj("Decrementing rc of %s (%p) to %zu\n", rep, ((uintptr_t)*value) >> REFCOUNT_SHIFT);

    // Check if the refcount has become zero.
    bool result = ((uintptr_t)*value >> REFCOUNT_SHIFT) == 0;
    if(result){
        debug_obj("SCC - Self contained: %s (%p)\n", rep);
        debug_print_scc(state, rep);
    }
    return result;
}

/*
  Marks all the objects in the SCC containing start as immutable

  All objects in the SCC must be implicitly immutable, and only reach
  other implicitly immutable objects.

  The objects are removed from the visited structures as their is
  no need to back track marking as deeply immutable, as it was true
  before this freeze operation started.
*/
void immutable_by_construction(PyObject* start, struct FreezeState *state)
{
    PyObject* curr = start;
    do
    {
        PyObject* next = get_next(curr, state);

        // Mark as frozen, this can only reach immutable objects so safe.
        _Py_SetImmutable(curr);

        debug_obj("Immutable by construction: %s (%p)\n", curr);

        // Remove from the visited structures
        // as this does not need backtracking if it is not
        // self-contained.
        _Py_hashtable_steal(state->next, curr);
        _Py_hashtable_steal(state->rep, curr);

        curr = next;
    } while (curr != start);
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
    _Py_hashtable_foreach(state->rep, mark_frozen, state);
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

static int freeze_visit(PyObject* obj, void* dfs)
{
    if (obj == NULL) {
        debug("-> nullptr\n");
        return 0;
    }

    if (_Py_IsImmutable(obj))
        return 0;

    debug_obj("-> %s (%p) rc=%u\n", obj, Py_REFCNT(obj));

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

    if(obj == (PyObject *)&_PyNotFreezable_Type){
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
    // TODO(Immutable): This will need to be atomic.
#if SIZEOF_VOID_P > 4
    Py_ssize_t old = _Py_atomic_add_ssize(&op->ob_refcnt_full, -1);
    // The ssize_t might be too big, so mask to 32 bits as that is the size of
    // ob_refcnt.
    old = old & 0xFFFFFFFF;
#else
    Py_ssize_t old = _Py_atomic_add_ssize(&op->ob_refcnt, -1);
    old = _Py_IMMUTABLE_FLAG_CLEAR(old);
#endif
    
    if (old != 1) {
        assert(_Py_IMMUTABLE_FLAG_CLEAR(op->ob_refcnt) != 0);
        // Context does not to dealloc this object.
        return false;
    }

    assert(_Py_IMMUTABLE_FLAG_CLEAR(op->ob_refcnt) == 0);

    _Py_CLEAR_IMMUTABLE(op);

    if (PyWeakref_Check(op)) {
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

int traverse_freeze(PyObject* obj, PyObject* dfs)
{
    debug_obj("%s (%p) rc=%u\n", obj, Py_REFCNT(obj));

    if(is_c_wrapper(obj)) {
        // C functions are not mutable
        // Types are manually traversed
        return 0;
    }

    // Function require some work to freeze, so we do not freeze the
    // world as they mention globals and builtins.  This will shadow what they
    // use, and then we can freeze the those components.
    if(PyFunction_Check(obj)){
        SUCCEEDS(shadow_function_globals(obj));
    }

    if(PyType_Check(obj)){
        // TODO(Immutable): mjp: Special case for types not sure if required. We should review.
        PyTypeObject* type = (PyTypeObject*)obj;

        SUCCEEDS(freeze_visit(type->tp_dict, dfs));
        SUCCEEDS(freeze_visit(type->tp_mro, dfs));
        // We need to freeze the tuple object, even though the types
        // within will have been frozen already.
        SUCCEEDS(freeze_visit(type->tp_bases, dfs));
    }
    else
    {
        traverseproc traverse = Py_TYPE(obj)->tp_traverse;
        if(traverse != NULL){
            SUCCEEDS(traverse(obj, (visitproc)freeze_visit, dfs));
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
            if (freeze_visit(wr, dfs)) {
                goto error;
            }
        }
    }

    // The default tp_traverse will not visit the type object if it is
    // not heap allocated, so we need to do that manually here to freeze
    // the statically allocated types that are reachable.
    if (!(Py_TYPE(obj)->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        SUCCEEDS(freeze_visit(_PyObject_CAST(Py_TYPE(obj)), dfs));
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

    int total_sccs = 0;
    int closed_sccs = 0;
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

                if (is_marked_implicitly_immutable(item, &freeze_state))
                {
                    immutable_by_construction(item, &freeze_state);
                    continue;
                }

                total_sccs++;
                // Completed an SCC do the calculation here.
                if (complete_scc(item, &freeze_state))
                {
                    debug("Marked as closed!\n");
                    debug_print_scc(&freeze_state, item);
                    closed_sccs++;
                }
            }
            continue;
        }

        if (_Py_IsImmutable(item)) {
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
                bool result = add_internal_reference(item, &freeze_state);
                if (result)
                {
                    debug_print_scc(&freeze_state, item);
                    closed_sccs++;
                }
                // assert(!result || (item == obj));
                continue;
            }

            // Not pending, so this objects graph has been fully explored.
            // If this was implicitly immutable it would have been removed
            // from the visited structure, so it can reach mutable state.
            if (add_internal_reference(item, &freeze_state))
                closed_sccs++;
            continue;
        }

        // New object, check if freezable
        SUCCEEDS(check_freezable(imm_state, item));

        // Add to visited before putting in internal datastructures, so don't have
        // to account of internal RC manipulations.
        add_visited(item, &freeze_state);
        // Add postorder step to dfs.
        SUCCEEDS(push(freeze_state.dfs, item));
        SUCCEEDS(push(freeze_state.dfs, PostOrderMarker));
        // Add to the SCC path
        SUCCEEDS(push(freeze_state.pending, item));

        if (!is_shallow_immutable(item)) {
            debug_obj("Not implicitly immutable: %s (%p)\n", item);
            // Mark pending stack as not immutable by construction.
            mark_pending_not_implicitly_immutable(&freeze_state);
        }

#ifdef Py_DEBUG
        // We need to add this attribute before traversing, so that if it creates a 
        // dictionary, then this dictionary is frozen.
        if (freeze_state.freeze_location != NULL) {
            // Some objects don't have attributes that can be set.
            // As this is a Debug only feature, we could potentially increase the object
            // size to allow this to be stored directly on the object.
            if (PyObject_SetAttrString(item, "__freeze_location__", freeze_state.freeze_location) < 0) {
                // Ignore failure to set _freeze_location
                PyErr_Clear();
                // We still want to freeze the object, so we continue
            }
        }
#endif


        // Traverse the fields of the current object to add to the dfs.
        SUCCEEDS(traverse_freeze(item, freeze_state.dfs));
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
    result = -1;

finally:
    deallocate_FreezeState(&freeze_state);

    return result;
}