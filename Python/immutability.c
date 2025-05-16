
#include "Python.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include "pycore_object.h"
#include "pycore_immutability.h"


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

    if(PyDict_SetItemString(PyModule_GetDict(frozen_importlib), "_freezable_types", state->freezable_types)){
        Py_DECREF(frozen_importlib);
        return -1;
    }

    Py_DECREF(frozen_importlib);

    return 0;
}

static struct _Py_immutability_state* get_immutable_state(void)
{
    PyInterpreterState* interp = PyInterpreterState_Get();
    struct _Py_immutability_state *state = &interp->immutability;
    if(state->freezable_types == NULL){
        if(init_state(state) == -1){
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

// Remove temporary debugging code.
//#define DebugSCC printf
#define DebugSCC(...) do{}while(0)


static int push(PyObject* s, PyObject* item){
    if(item == NULL){
        return 0;
    }

    if(!PyList_Check(s)){
        PyErr_SetString(PyExc_TypeError, "Expected a list");
        return -1;
    }

    return _PyList_AppendTakeRef(_PyList_CAST(s), Py_NewRef(item));
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

    if(PyList_SetSlice(s, size - 1, size, NULL)){
        return NULL;
    }

    return item;
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

static bool is_c_wrapper(PyObject* obj){
    return PyCFunction_Check(obj) || Py_IS_TYPE(obj, &_PyMethodWrapper_Type) || Py_IS_TYPE(obj, &PyWrapperDescr_Type);
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
static PyObject* shadow_function_globals(PyObject* op)
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
        return NULL;
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
                return NULL;
            }
        }else if(PyDict_Contains(builtins, name)){
            PyObject* value = PyDict_GetItem(builtins, name);
            if(PyDict_SetItem(shadow_builtins, name, value)){
                Py_DECREF(shadow_builtins);
                Py_DECREF(shadow_globals);
                return NULL;
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
                    return NULL;
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
            return NULL;
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
            return NULL;
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
                    return NULL;
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

    Py_RETURN_NONE;

nomemory:
    Py_XDECREF(shadow_builtins);
    Py_XDECREF(shadow_globals);
    return PyErr_NoMemory();
}

static int freeze_visit(PyObject* obj, void* frontier)
{
    DebugSCC("   to %p\n", obj);
    if(push(frontier, obj)){
        PyErr_NoMemory();
        return -1;
    }

    return 0;
}

static bool
is_freezable_builtin(PyTypeObject *type)
{
    if(type == &PyType_Type ||
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
        type == &PyClassMethodDescr_Type ||
        type == &PyMethod_Type ||
        type == &PyCFunction_Type ||
        type == &PyCapsule_Type ||
        type == &PyCode_Type ||
        type == &PyCell_Type ||
        type == &PyFrame_Type ||
        type == &_PyWeakref_RefType)
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

// We will need to preserve the FINALIZER flag, so use the mark
// flag to distinguish between RANK and Parent pointers stored
// in the _gc_prev field.
#define SCC_RANK_FLAG _PyGC_PREV_MASK_COLLECTING

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
    DebugSCC("   set_scc_next %p -> %p\n", obj, next);
    // Use GC space for the next pointer.
    _Py_AS_GC(obj)->_gc_next = (uintptr_t)next;
}

PyObject* scc_next(PyObject* obj)
{
    // Use GC space for the next pointer.
    return _Py_CAST(PyObject*, _Py_AS_GC(obj)->_gc_next);
}

void scc_init_cycle(PyObject* obj)
{
    // Check if this not been part of an SCC yet.
    if (scc_next(obj) == NULL) {
        // Set up a new SCC with a single element.
        set_scc_rank(obj, 0);
        set_scc_next(obj, obj);
    }
}

PyObject* scc_find(PyObject* obj)
{
    if ((_Py_AS_GC(obj)->_gc_prev & SCC_RANK_FLAG) == SCC_RANK_FLAG) {
        return obj;
    }

    // TODO(Immutable): This is where we need to compress parent paths.
    return scc_find(_Py_CAST(PyObject*, scc_parent(obj)));
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
}

bool scc_union(PyObject* a, PyObject* b)
{
    // We know a and b are definitely part of a cycle
    // even if a and b are equal, then this represents
    // a self edge.
    scc_init_cycle(a);
    scc_init_cycle(b);

    PyObject* a_root = scc_find(a);
    PyObject* b_root = scc_find(b);

    if(a_root == b_root){
        // Same SCC so no work is required.
        return false;
    }

    // Determine the rank
    size_t a_rank = scc_rank(a_root);
    size_t b_rank = scc_rank(b_root);
    // switch order if necessary.
    if (a_rank < b_rank) {
        PyObject* tmp = a_root;
        a_root = b_root;
        b_root = tmp;
    } else if (a_rank == b_rank) {
        // If they are the same rank, then we need to increment the rank.
        set_scc_rank(a_root, a_rank + 1);
    }

    // Update the parent pointer to union the two sccs.
    set_scc_parent(b_root, a_root);

    // Cyclic list merge
    PyObject* a_next = scc_next(a_root);
    PyObject* b_next = scc_next(b_root);
    set_scc_next(a_root, b_next);
    set_scc_next(b_root, a_next);

    return true;
}

static PyObject* scc_root(PyObject* obj)
{
    assert(_Py_IsImmutable(obj));
    if ((obj->ob_refcnt & _Py_IMMUTABLE_MASK) == _Py_IMMUTABLE_DIRECT)
        return obj;

    if ((obj->ob_refcnt & _Py_IMMUTABLE_MASK) != _Py_IMMUTABLE_PENDING) {
        return scc_parent(obj);
    }

    return scc_find(obj);
}

void return_to_gc(PyObject* op)
{
    set_scc_next(op, NULL);
    PyObject_GC_Track(op);
}


typedef enum {
    VALID_BUILTIN,
    VALID_EXPLICIT,
    VALID_IMPLICIT,
    INVALID_NOT_FREEZABLE,
    INVALID_C_EXTENSIONS,
    ERROR
} FreezableCheck;


static FreezableCheck check_freezable(struct _Py_immutability_state *state, PyObject* obj)
{
    int result = 0;

    /*
    Immutable(TODO)
    This is technically all that is needed, but without the ability to back out
    the immutability, the instance will still be frozen, which is why the alternative code
    is used for now.
    if(obj == (PyObject *)&_PyNotFreezable_Type){
        return INVALID_NOT_FREEZABLE;
    }
    */
    result = PyObject_IsInstance(obj, (PyObject *)&_PyNotFreezable_Type);
    if(result == -1){
        return ERROR;
    }
    else if(result == 1){
        return INVALID_NOT_FREEZABLE;
    }

    if(is_freezable_builtin(obj->ob_type)){
        return VALID_BUILTIN;
    }

    result = is_explicitly_freezable(state, obj);
    if(result == -1){
        return ERROR;
    }
    else if(result == 1){
        return VALID_EXPLICIT;
    }

    if(_PyType_HasExtensionSlots(obj->ob_type)){
        return INVALID_C_EXTENSIONS;
    }

    return VALID_IMPLICIT;
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

/**
 * Used to represent that the next element in the DFS walk is a
 * post-order step, rather than pre-order.
 *
 * Using a separate object means it cannot conflict with anything
 * in the actual python object graph.
 */
static PyObject PostOrderMarkerStruct = {
    _PyObject_EXTRA_INIT
    { _Py_IMMORTAL_REFCNT },
    &_PyNone_Type
};
static PyObject* PostOrderMarker = &PostOrderMarkerStruct;


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
    DebugSCC("Reinstate %p to %p\n", curr_root, obj);
    obj->ob_refcnt++;

    return 0;
}

struct SCCDetails {
    int has_weakreferences;
    int has_legacy_finalizers;
    int has_finalizers;
};

// This will restore the reference counts for the interior edges of the SCC.
// It calculates some properites of the SCC, to decide how it might be
// finalised.  Adds an RC to every element in the SCC.
static void scc_add_internal_refcounts(PyObject* obj, struct SCCDetails* details)
{
    DebugSCC("Adding internal refcount for %s @ %p\n", Py_TYPE(obj)->tp_name, obj);
    assert(_Py_IsImmutable(obj));
    PyObject* root = scc_root(obj);

    details->has_weakreferences = 0;
    details->has_legacy_finalizers = 0;
    details->has_finalizers = 0;

    // Add back the reference counts for the interior edges.
    PyObject* n = obj;
    do {
        DebugSCC("Unfreezing %s @ %p\n", Py_TYPE(n)->tp_name, n);
        PyObject* c = n;
        n = scc_next(c);
        // Add a reference count so that the object is not reclaimed by any of the finalizers.
        c->ob_refcnt++;
        // TODO (Immutable):  This needs to mirror what freeze does for traversal.
        // TODO (Immutable):  Special cases like type, and others?, are missing.
        traverseproc traverse = Py_TYPE(c)->tp_traverse;
        if (traverse != NULL) {
            traverse(c, (visitproc)scc_add_internal_refcount_visit, root);
        }
        if (Py_TYPE(c)->tp_del != NULL)
          details->has_legacy_finalizers++;
        if (Py_TYPE(c)->tp_finalize != NULL && !_PyGCHead_FINALIZED(_Py_AS_GC(c)))
          details->has_finalizers++;
        if (PyWeakref_Check(c)) {
            // We followed weakreferences during freeze, so need to here as well.
            PyObject* wr = PyWeakref_GET_OBJECT(c);
            if (wr != NULL) {
                // This will increment the reference if it is in the same SCC
                // and do nothing otherwise.  We are treating the weakref as
                // a strong reference for the immutable state.
                scc_add_internal_refcount_visit(wr, root);
            }
            details->has_weakreferences++;
            DebugSCC("Weakref %p to %p\n", c, wr);
        }
        if (_PyType_SUPPORTS_WEAKREFS(Py_TYPE(c)) &&
            *_PyObject_GET_WEAKREFS_LISTPTR_FROM_OFFSET(c) != NULL) {
          DebugSCC("Weakref list %p\n", *_PyObject_GET_WEAKREFS_LISTPTR_FROM_OFFSET(c));
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
        c->ob_refcnt &= ~_Py_IMMUTABLE_MASK;
        if (PyWeakref_Check(c)) {
            PyObject* wr = PyWeakref_GET_OBJECT(c);
            if (wr != NULL) {
                // Turn back to weak reference. We made the weak references strong during freeze.
                Py_DECREF(wr);
            }
        }
    } while (n != obj);
}

// Returns all the objects in the SCC to the Python cycle detector.
static void scc_return_to_gc(PyObject* obj)
{
    PyObject* n = obj;
    do {
        PyObject* c = n;
        n = scc_next(c);
        set_scc_next(c, NULL);
        set_scc_parent(c, NULL);
        return_to_gc(c);
        Py_DECREF(c);
    } while (n != obj);
}

static void unfreeze(PyObject* obj)
{
    if (scc_next(obj) == NULL)
    {
        // Clear Immutable flags
        obj->ob_refcnt &= _Py_REFCNT_MASK;
        // Return to the GC.
        return_to_gc(obj);
        return;
    }
    DebugSCC("Unfreezing %s @ %p\n", Py_TYPE(obj)->tp_name, obj);
    // Note: We don't need the details of the SCC for a simple unfreeze.
    struct SCCDetails scc_details;
    scc_add_internal_refcounts(obj, &scc_details);
    scc_make_mutable(obj);
    scc_return_to_gc(obj);
}

static void unfreeze_and_finalize_scc(PyObject* obj)
{
    struct SCCDetails scc_details;

    scc_add_internal_refcounts(obj, &scc_details);

    // These are cases that we don't handle.  Return the state as mutable to the
    // cycle detector to handle.
    // TODO(Immutable): Lift the weak references to be handled here.
    if (scc_details.has_weakreferences>0 || scc_details.has_legacy_finalizers>0) {
        DebugSCC("There are weak references or legacy finalizers in the SCC.  Let cycle detector handle this case.\n");
        DebugSCC("Weak references: %d, Legacy finalizers: %d\n", scc_details.has_weakreferences, scc_details.has_legacy_finalizers);

        scc_make_mutable(obj);
        scc_return_to_gc(obj);
        return;
    }

    // Make all objects mutable
    // But leave cyclic list in place for the SCC.
    scc_make_mutable(obj);

    PyObject* n = obj;
    if (scc_details.has_finalizers) {
        // Call the finalizers for all objects in the SCC.
        do {
            PyObject* c = n;
            n = scc_next(c);
            if (_PyGCHead_FINALIZED(_Py_AS_GC(c)))
                continue;
            destructor finalize = Py_TYPE(c)->tp_finalize;
            if (finalize == NULL)
                continue;
            // Call the finalizer for the object.
            finalize(c);
            // Mark so we don't finalize it again.
            _PyGCHead_SET_FINALIZED(_Py_AS_GC(c));
        } while (n != obj);
    }

    // tp_clear all elements in the cycle, and drop reference counts on all the
    // elements of the SCC so that they can be reclaimed.
    n = obj;
    do {
        DebugSCC("Clearing %s @ %p\n", Py_TYPE(n)->tp_name, n);
        PyObject* c = n;
        n = scc_next(c);
        inquiry clear;
        if ((clear = Py_TYPE(obj)->tp_clear) != NULL) {
            // TODO(Immutable): Should do something with the error? e.g.
            // if (_PyErr_Occurred(tstate)) {
            //     _PyErr_WriteUnraisableMsg("in tp_clear of",
            //                             (PyObject*)Py_TYPE(op));
            // }
        }
    } while (n != obj);

    scc_return_to_gc(obj);
}

// Perform a decref on an immutable object
// returns true if the object should be deallocated.
int _Py_DecRef_Immutable(PyObject *op)
{
    // Decrement the reference count of an immutable object without
    // deallocating it.
    assert(_Py_IsImmutable(op));

    if ((op->ob_refcnt & _Py_IMMUTABLE_MASK) == _Py_IMMUTABLE_INDIRECT) {
        // The object is part of an SCC, and another object in the SCC
        // carries the reference count.
        DebugSCC("Decrefing %p an Indirect Imm from %zu @ %p\n", op, op->ob_refcnt & _Py_REFCNT_MASK, scc_parent(op));
        op = scc_parent(op);
    }
    else
      DebugSCC("Decrefing an Direct Imm from %zu @ %p\n", op->ob_refcnt & _Py_REFCNT_MASK, op);

    // TODO(Immutable): This needs to be atomic.
    op->ob_refcnt -= 1;
    if ((op->ob_refcnt & _Py_REFCNT_MASK) != 0)
        // Context does not to dealloc this object.
        return false;

    // We should not be deallocating a partial SCC here. Things we go very wrong.
    assert((op->ob_refcnt & _Py_IMMUTABLE_MASK) != _Py_IMMUTABLE_PENDING);

    DebugSCC("Deallocating an immutable object with a reference count of 0\n");
    if (PyObject_IS_GC(op)) {
        if (scc_next(op) != NULL) {
            // This is part of an SCC, so we need to turn it back into mutable state,
            // and correctly re-establish RCs.
            unfreeze_and_finalize_scc(op);
            return false;
        }
        // This is a GC object, so we need to put it back on the GC list.
        return_to_gc(op);
    }
    // Clear the immutable flag so that finalisers can run correctly.
    assert((op->ob_refcnt & _Py_REFCNT_MASK) == 0);
    op->ob_refcnt = 0;
    return true;
}

void _Py_RefcntAdd_Immutable(PyObject *op, int delta)
{
    // Increment the reference count of an immutable object.
    assert(_Py_IsImmutable(op));
    if ((op->ob_refcnt & _Py_IMMUTABLE_MASK) == _Py_IMMUTABLE_INDIRECT) {
        // The object is part of an SCC, and another object in the SCC
        // carries the reference count.
        DebugSCC("Increfing %p an Indirect Imm from %zu @ %p\n", op, op->ob_refcnt & _Py_REFCNT_MASK, scc_parent(op));
        op = scc_parent(op);
    }
    else
        DebugSCC("Increfing an Direct Imm from %zu @ %p\n", op->ob_refcnt & _Py_REFCNT_MASK, op);
    // TODO(Immutable): This needs to be atomic.
    op->ob_refcnt += delta;
}


int _PyImmutability_Freeze(PyObject* obj)
{
    /*
        This code performs a DFS walk from obj.
        The SCC algorithm requires both a pre-order and a post-order
        step to be applied.
     */
    PyObject* frontier = NULL;
    /*
        This represents the current stack of partial SCCs that have
        been explored, but that might still have additional elements
        added to them.
     */
    PyObject* pending = NULL;
    int result = 0;
    DebugSCC("Freezing %s @ %p\n", Py_TYPE(obj)->tp_name, obj);

    struct _Py_immutability_state* state = get_immutable_state();
    if(state == NULL){
        PyErr_SetString(PyExc_RuntimeError, "Failed to initialize immutability state");
        return -1;
    }

    if(_Py_IsImmutable(obj)){
        return result;
    }

    frontier = PyList_New(0);
    if(frontier == NULL){
        goto error;
    }

    if(push(frontier, obj)){
        goto error;
    }

    pending = PyList_New(0);
    if(pending == NULL){
        goto error;
    }

    while(PyList_Size(frontier) != 0){
        PyObject* item = pop(frontier);
        FreezableCheck check;

        DebugSCC("Freeze at %s @ %p\n", Py_TYPE(item)->tp_name, item);

        if(item == state->blocking_on ||
           item == state->module_locks){
            continue;
        }

        check = check_freezable(state, item);
        switch(check){
            case INVALID_NOT_FREEZABLE:
                PyErr_SetString(PyExc_TypeError, "Invalid freeze request: instance of NotFreezable");
                goto unfreeze;

            case INVALID_C_EXTENSIONS:
                PyObject* error_msg = PyUnicode_FromFormat(
                    "Cannot freeze instance of type %s due to custom functionality implemented in C",
                    (item->ob_type->tp_name));
                PyErr_SetObject(PyExc_TypeError, error_msg);
                Py_DECREF(error_msg);
                goto unfreeze;

            case VALID_BUILTIN:
            case VALID_EXPLICIT:
            case VALID_IMPLICIT:
                break;

            case ERROR:
                goto error;

            default:
                PyErr_SetString(PyExc_RuntimeError, "Unknown freezable check value");
                goto error;
        }

        if (item == PostOrderMarker) {
            // This is a post-order step, so we need to check if this is a completed SCC.
            item = peek(frontier);
            DebugSCC("PostOrder for %p\n", item);
            if (peek(pending) == item)
            {
                pop(pending);
                // We need to complete the current SCC.
                size_t rc = item->ob_refcnt & _Py_REFCNT_MASK;
                PyObject* c = scc_next(item);
                if (c == NULL) {
                    // This is not part of a cycle, just make it immutable.
                    item->ob_refcnt &= ~_Py_IMMUTABLE_MASK;
                    item->ob_refcnt |= _Py_IMMUTABLE_DIRECT;
                    continue;
                }
                size_t count = 1;
                while (c != item)
                {
                    DebugSCC("Adding %p @ %p to SCC\n", c, item);
                    c->ob_refcnt &= _Py_REFCNT_MASK;
                    rc += c->ob_refcnt;
                    // Set refcnt to zero, and mark as immutable indirect.
                    c->ob_refcnt = _Py_IMMUTABLE_INDIRECT;
                    set_scc_parent(c, item);
                    c = scc_next(c);
                    count++;
                }
                // We will have left an RC live for each element in the SCC, so
                // we need to remove that from the SCCs refcount.
                item->ob_refcnt = rc - (count - 1);
                item->ob_refcnt &= ~_Py_IMMUTABLE_MASK;
                item->ob_refcnt |= _Py_IMMUTABLE_DIRECT;
                // Clear the rank information as we don't need it anymore.
                set_scc_parent(item, NULL);
                DebugSCC("Completed SCC %s @ %p with %zu members with rc %zu \n", Py_TYPE(item)->tp_name, item, count, rc - (count - 1));
            }
            pop(frontier);
            continue;
        }

        if(_Py_IsImmutable(item)){
            DebugSCC("Already immutable %s @ %p\n", Py_TYPE(item)->tp_name, item);

            if ((item->ob_refcnt & _Py_IMMUTABLE_MASK) == _Py_IMMUTABLE_PENDING) {
                DebugSCC("Found a cycle %s @ %p\n", Py_TYPE(item)->tp_name, item);
                // this is an internal edge
                // Remove internal refcount
                _Py_DECREF_NO_DEALLOC(item);
                // Unify the various pending SCCs that this reaches.
                while (scc_union(peek(pending), item)) {
                    DebugSCC("Unifying %s @ %p with %s @ %p\n", Py_TYPE(item)->tp_name, item, peek(pending)->ob_type->tp_name, peek(pending));
                    pop(pending);
                }
                continue;
            }

            // This is a completed SCC, we have already calculated the incoming RC
            // so we can skip this edge.
            continue;
        }

        if(is_c_wrapper(item)) {

            // C functions are not mutable, so we can skip them.
            item->ob_refcnt |= _Py_IMMUTABLE_DIRECT;
            if (_PyObject_IS_GC(item))
              scc_init(item);
            continue;
        }

        if(PyFunction_Check(item)){
            if(shadow_function_globals(item) == NULL){
                goto error;
            }
        }

        if (_PyObject_IS_GC(item))
        {
            // This is a previously not visited object, and it has space to
            // be part of the SCC algorith.  We need to explore its object graph.
            DebugSCC("Freezing GC object %s @ %p\n", Py_TYPE(item)->tp_name, item);
            // Add this item back to the list with a PostOrderMarker, so that we
            // can detect when we have explored its reachable subgraph.
            push(frontier, item);
            push(frontier, PostOrderMarker);
            // Add to pending as this is a new single element SCC, but
            // this could be part of a larger SCC.
            push(pending, item);
            scc_init(item);
            // Mark as pending so we can detect back edges in the traversal.
            item->ob_refcnt |= _Py_IMMUTABLE_PENDING;
        }
        else
        {
            DebugSCC("Freezing non-GC object %s @ %p\n", Py_TYPE(item)->tp_name, item);
            // If there is no GC space, then we cannot do the SCC stuff, so
            // assume it is not part of a cycle, and just make it immutable.
            item->ob_refcnt |= _Py_IMMUTABLE_DIRECT;
        }

        // Explore the fields of this object as this is a pre-order step in the dfs.
        DebugSCC("Edges from %s @ %p\n", Py_TYPE(item)->tp_name, item);

        if(PyType_Check(item)){
            // For types we do not explore all the fields. (TODO: Explain why!)
            PyTypeObject* type = (PyTypeObject*)item;

            if(push(frontier, type->tp_dict))
            {
                goto error;
            }

            if(check != VALID_EXPLICIT)
            {
                if(push(frontier, type->tp_mro))
                {
                    goto error;
                }
            }
        }
        else
        {
            traverseproc traverse = Py_TYPE(item)->tp_traverse;
            if(traverse != NULL){
                if(traverse(item, (visitproc)freeze_visit, frontier)){
                    goto error;
                }
            }
        }

        // Weak references are not followed by the GC, but should be
        // for immutability.  Otherwise, we could share mutable state
        // using a weak refernce.
        if (PyWeakref_Check(item)) {
            PyObject* wr = PyWeakref_GET_OBJECT(item);
            if (wr != NULL) {
                DebugSCC("   Weakref %s @ %p from %s @ %p\n", Py_TYPE(wr)->tp_name, wr, Py_TYPE(item)->tp_name, item);
                Py_INCREF(wr);
                // Make the weak reference strong.
                if (freeze_visit(PyWeakref_GET_OBJECT(item), frontier)) {
                    goto error;
                }
            }
        }

        // If the type is not heap allocated, then the traverse function
        // will not pass the type object to us, so we need to add it manually.
        if ((item->ob_type->tp_flags & Py_TPFLAGS_HEAPTYPE) == 0) {
            PyObject* type_op = _PyObject_CAST(item->ob_type);
            if(freeze_visit(type_op, frontier)){
                goto error;
            }
        }
    }

    goto finally;

unfreeze:
    while(PyList_Size(pending) != 0){
        PyObject* item = pop(pending);
        if(item == NULL){
            goto error;
        }
        unfreeze(item);
    }

error:
    result = -1;

finally:
    Py_XDECREF(pending);
    Py_XDECREF(frontier);

    return result;
}