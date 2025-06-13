
#include "Python.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include "pycore_descrobject.h"
#include "pycore_gc.h"
#include "pycore_object.h"
#include "pycore_immutability.h"
#include "pycore_list.h"


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

    // TODO(Immutable): mjp: Why is this here?  I can find anyone using it.  Can we remove it?
    //  Commented out for now, but we should remove if MAJ agrees.
    // if(PyDict_SetItemString(PyModule_GetDict(frozen_importlib), "_freezable_types", state->freezable_types)){
    //     Py_DECREF(frozen_importlib);
    //     return -1;
    // }

    Py_DECREF(frozen_importlib);

    return 0;
}

// This is separate to the previous init as it depends on the traceback
// module being available, and can cause a circular import if it is
// called during register freezable.
static
void init_traceback_state(struct _Py_immutability_state *state)
{
#ifdef Py_DEBUG
    PyObject *traceback_module = PyImport_ImportModule("traceback");
    if (traceback_module != NULL) {
        state->traceback_func = PyObject_GetAttrString(traceback_module, "format_stack");
        Py_DECREF(traceback_module);
    }
#endif
}

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

static bool is_c_wrapper(PyObject* obj){
    return PyCFunction_Check(obj) || Py_IS_TYPE(obj, &_PyMethodWrapper_Type) || Py_IS_TYPE(obj, &PyWrapperDescr_Type);
}

// Lifted from Python/gc.c
//******************************** */
#ifndef Py_GIL_DISABLED
#define GC_NEXT _PyGCHead_NEXT
#define GC_PREV _PyGCHead_PREV

static inline void
gc_list_init(PyGC_Head *list)
{
    // List header must not have flags.
    // We can assign pointer by simple cast.
    list->_gc_prev = (uintptr_t)list;
    list->_gc_next = (uintptr_t)list;
}

static inline int
gc_list_is_empty(PyGC_Head *list)
{
    return (list->_gc_next == (uintptr_t)list);
}

/* Append `node` to `list`. */
static inline void
gc_list_append(PyGC_Head *node, PyGC_Head *list)
{
    assert((list->_gc_prev & ~_PyGC_PREV_MASK) == 0);
    PyGC_Head *last = (PyGC_Head *)list->_gc_prev;

    // last <-> node
    _PyGCHead_SET_PREV(node, last);
    _PyGCHead_SET_NEXT(last, node);

    // node <-> list
    _PyGCHead_SET_NEXT(node, list);
    list->_gc_prev = (uintptr_t)node;
}

/* Move `node` from the gc list it's currently in (which is not explicitly
 * named here) to the end of `list`.  This is semantically the same as
 * gc_list_remove(node) followed by gc_list_append(node, list).
 */
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

/* append list `from` onto list `to`; `from` becomes an empty list */
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

struct _gc_runtime_state*
get_gc_state(void)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    return &interp->gc;
}
#endif // Py_GIL_DISABLED
/**
 * Used to track the state of an in progress freeze operation.
 * We track the objects that have been visited so far using three lists:
 *   -  visited - a list of objects that have been visited and were being tracked by the GC
 *                we use the GC header to thread this list.
 *   -  visited_untracked - a list of objects that have been visited but were not tracked by the GC
 *               we use the GC header to thread this list.
 *   -  visited_list - a list of objects that do not have GC space, so we track them separately using
 *               a Python list.  In No-GIL builds, this is the only list that is used as the GC header
 *               has been repurposed for biased reference counting.
 */
struct FreezeState {
#ifndef Py_GIL_DISABLED
    PyGC_Head visited;  // Set of objects that have been visited
    PyGC_Head visited_untracked; // Set of objects that have been visited and are immortal
#endif
    PyObject* visited_list; // Some objects don't have GC space, so we need to track them separately.

    PyObject* dfs; // The DFS stack used to traverse the object graph during freezing.
};


//******************************** */


int
init_freeze_state(struct FreezeState *state)
{
#ifndef Py_GIL_DISABLED
    gc_list_init(&(state->visited));
    gc_list_init(&(state->visited_untracked));
#endif
    state->visited_list = NULL;
    state->dfs = NULL;

    state->dfs = PyList_New(0);
    if (state->dfs == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to create DFS stack for freeze operation");
        return -1;
    }

    return 0;
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

int has_visited(struct FreezeState*, PyObject *op)
{
    // TODO(Immutable): In NoGIL builds we will need to use a side data structure
    // as we will need to handle multiple threads freezing overlapping object graphs.
    if (_Py_IsImmutable(op))
        return true;
    return false;
}

int
add_visited_set(struct FreezeState *state, PyObject *op)
{
    // Note that we should only set immutable once this cannot fail.
    // Failure would require us to backtrack the immutability, but
    // if we failed to add to the list, the caller wouldn't know what to undo.

#ifndef Py_GIL_DISABLED
    if (_PyObject_IS_GC(op)) {
        _Py_SetImmutable(op);
        if (_PyObject_GC_IS_TRACKED(op)) {
            gc_list_move(_Py_AS_GC(op), &(state->visited));
            return 0;
        }
        // If the object is not tracked by the GC, we can just add it to the visited_untracked list.
        gc_list_append(_Py_AS_GC(op), &(state->visited_untracked));
        return 0;
    }
#endif

    // Only create the visited_list if it is needed.
    if (state->visited_list == NULL) {
        state->visited_list = PyList_New(0);
        if (state->visited_list == NULL) {
            goto error;
        }
    }

    if (push(state->visited_list, op) != 0)
    {
        // If we fail to add the item to the visited set, then we
        // will not be able to backtrack, so go to error case.
        goto error;
    }
    
    _Py_SetImmutable(op);
    return 0;

error:
    PyErr_SetString(PyExc_RuntimeError, "Failed to add item to visited set");
    return -1;
}

// Called on the failure of a freeze operation.
// This unsets the immutability of all the objects that were visited.
void fail_freeze(struct FreezeState *state)
{
    Py_XDECREF(state->dfs);
#ifdef Py_DEBUG
    Py_XDECREF(state->freeze_location);
#endif

#ifndef Py_GIL_DISABLED
    PyGC_Head *gc;
    for (gc = _PyGCHead_NEXT(&(state->visited)); gc != &(state->visited); gc = _PyGCHead_NEXT(gc)) {
        _Py_CLEAR_IMMUTABLE(_Py_FROM_GC(gc));
    }
    struct _gc_runtime_state* gc_state = get_gc_state();
    gc_list_merge(&(state->visited), &(gc_state->old[1].head));


    PyGC_Head *next;
    for (gc = _PyGCHead_NEXT(&(state->visited_untracked)); gc != &(state->visited_untracked); gc = next) {
        next = _PyGCHead_NEXT(gc);
        _Py_CLEAR_IMMUTABLE(_Py_FROM_GC(gc));
        // Object was not tracked in the GC, so we don't need to merge it back.
        _PyGCHead_SET_PREV(gc, NULL);
        _PyGCHead_SET_NEXT(gc, NULL);
    }
#endif

    if (state->visited_list == NULL) {
        return; // Nothing to do
    }

    while (PyList_Size(state->visited_list) > 0) {
        // Pop doesn't return a newref, but we know the object is still live
        // as we didn't change anything.
        PyObject* item = pop(state->visited_list);
        _Py_CLEAR_IMMUTABLE(item);
    }

    // Tidy up the visited set
    Py_DECREF(state->visited_list);
}

// Called on the successful completion of a freeze operation.
// This merges the visited set back into the GC's old generation, and clears
// the visited_untracked set, which contains objects that were not tracked
// by the GC, but were visited during the freeze operation.
// It also decrements the reference count of the visited_list, which is used
// to track objects that do not have GC space, so we need to clear it up
// after the freeze operation is complete.
void finish_freeze(struct FreezeState *state)
{
#ifndef Py_GIL_DISABLED
    struct _gc_runtime_state* gc_state = get_gc_state();
    gc_list_merge(&(state->visited), &(gc_state->old[1].head));

    PyGC_Head *gc;
    PyGC_Head *next;
    for (gc = _PyGCHead_NEXT(&(state->visited_untracked)); gc != &(state->visited_untracked); gc = next) {
        next = _PyGCHead_NEXT(gc);
        // Object was not tracked in the GC, so we don't need to merge it back.
        _PyGCHead_SET_PREV(gc, NULL);
        _PyGCHead_SET_NEXT(gc, NULL);
    }
#endif

    Py_XDECREF(state->visited_list);
    Py_XDECREF(state->dfs);
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
    if (obj == NULL)
        return 0;

    if (_Py_IsImmutable(obj))
        return 0;

    if(push(dfs, obj)){
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
    /*
    TODO(Immutable): mjp: Not sure the following is true anymore.
    Immutable(TODO)
    This is technically all that is needed, but without the ability to back out
    the immutability, the instance will still be frozen, which is why the alternative code
    is used for now.
    if(obj == (PyObject *)&_PyNotFreezable_Type){
        return INVALID_NOT_FREEZABLE;
    }
    */
    int result = PyObject_IsInstance(obj, (PyObject *)&_PyNotFreezable_Type);
    if(result == -1){
        return -1;
    }
    else if(result == 1){
        PyErr_SetString(PyExc_TypeError, "Invalid freeze request: instance of NotFreezable");
        return -1;
    }

    if(is_freezable_builtin(obj->ob_type)){
        return 0;
    }

    result = is_explicitly_freezable(state, obj);
    if(result == -1){
        return -1;
    }
    else if(result == 1){
        return 0;
    }

    if(_PyType_HasExtensionSlots(obj->ob_type)){
        PyObject* error_msg = PyUnicode_FromFormat(
            "Cannot freeze instance of type %s due to custom functionality implemented in C",
            (obj->ob_type->tp_name));
        PyErr_SetObject(PyExc_TypeError, error_msg);
        return -1;
    }

    return 0;
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
    // TODO(Immutable): This needs to be atomic.
    op->ob_refcnt -= 1;
    if (_Py_IMMUTABLE_FLAG_CLEAR(op->ob_refcnt) != 0)
        // Context does not to dealloc this object.
        return false;

    assert(_Py_IMMUTABLE_FLAG_CLEAR(op->ob_refcnt) == 0);

    _Py_CLEAR_IMMUTABLE(op);

    return true;
#endif
}

// Macro that jumps to error, if the expression `x` does not succeed.
#define SUCCEEDS(x) { do { int r = (x); if (r != 0) goto error; } while (0); }

int traverse_freeze(PyObject* obj, PyObject* dfs)
{
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
        // TODO(Immutable): Special case for types not sure if required.
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

int _PyImmutability_Freeze(PyObject* obj)
{
    if(_Py_IsImmutable(obj)){
        return 0;
    }
    int result = 0;

    struct FreezeState freeze_state;
    // Initialize the freeze state
    SUCCEEDS(init_freeze_state(&freeze_state));

    struct _Py_immutability_state* state = get_immutable_state();
    if(state == NULL){
        goto error;
    }


#ifdef Py_DEBUG
    PyObject* freeze_location = NULL;
    // In debug mode, we can set a freeze location for debugging purposes.
    // Get a traceback object to use as the freeze location.
    if (state->traceback_func == NULL) {
        init_traceback_state(state);
    }

    if (state->traceback_func != NULL) {
        PyObject *stack = PyObject_CallFunctionObjArgs(state->traceback_func, NULL);
        if (stack != NULL) {
            // Add the type name to the top of the stack, can be useful.
            PyObject* typename = PyObject_GetAttrString(_PyObject_CAST(Py_TYPE(obj)), "__name__");
            push(stack, typename);
            freeze_location = stack;
        }
    }
#endif

    SUCCEEDS(push(freeze_state.dfs, obj));

    while(PyList_Size(freeze_state.dfs) != 0){
        PyObject* item = pop(freeze_state.dfs);

        if(has_visited(&freeze_state, item)){
            continue;
        }

        if(item == state->blocking_on ||
           item == state->module_locks){
            continue;
        }

        SUCCEEDS(check_freezable(state, item));

#ifdef Py_DEBUG
        if (freeze_location != NULL) {
            // TODO(Immutable): Some objects don't have attributes that can be set.
            // As this is a Debug only feature, we could potentially increase the object
            // size to allow this to be stored directly on the object.
            if (PyObject_SetAttrString(item, "__freeze_location__", freeze_location) < 0) {
                // Ignore failure to set _freeze_location
                PyErr_Clear();
                // We still want to freeze the object, so we continue
            }
        }
#endif
        SUCCEEDS(add_visited_set(&freeze_state, item));

        SUCCEEDS(traverse_freeze(item, freeze_state.dfs));
    }

    finish_freeze(&freeze_state);
    goto finally;

error:
    fail_freeze(&freeze_state);
    result = -1;

finally:
#ifdef Py_DEBUG
    Py_XDECREF(freeze_location);
#endif
    return result;
}