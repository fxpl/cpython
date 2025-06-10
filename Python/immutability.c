
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

// Lifted fro mPython/gc.c
//******************************** */
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

/**
 * Used to track the state of an in progress freeze operation.
 *
 */
struct FreezeState {
#ifndef Py_GIL_DISABLED
    PyGC_Head visited;  // Set of objects that have been visited
    PyGC_Head visited_untracked; // Set of objects that have been visited and are immortal
#endif
    PyObject* visited_list; // Some objects don't have GC space, so we need to track them separately.
};


//******************************** */


void
init_freeze_state(struct FreezeState *state)
{
#ifndef Py_GIL_DISABLED
    gc_list_init(&(state->visited));
    gc_list_init(&(state->visited_untracked));
#endif
    state->visited_list = NULL;
}

int
add_visited_set(struct FreezeState *state, PyObject *op)
{
#ifndef Py_GIL_DISABLED
    if (_PyObject_IS_GC(op)) {
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
            return -1; // Memory error
        }
    }

    return push(state->visited_list, op);
}

void fail_freeze(struct FreezeState *state)
{
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
    if(!_Py_IsImmutable(obj)){
        if(push(frontier, obj)){
            PyErr_NoMemory();
            return -1;
        }
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

typedef enum {
    VALID_BUILTIN,
    VALID_EXPLICIT,
    VALID_IMPLICIT,
    INVALID_NOT_FREEZABLE,
    INVALID_C_EXTENSIONS,
    FREEZABLE_ERROR
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
        return FREEZABLE_ERROR;
    }
    else if(result == 1){
        return INVALID_NOT_FREEZABLE;
    }

    if(is_freezable_builtin(obj->ob_type)){
        return VALID_BUILTIN;
    }

    result = is_explicitly_freezable(state, obj);
    if(result == -1){
        return FREEZABLE_ERROR;
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

int _PyImmutability_Freeze(PyObject* obj)
{
    PyObject* frontier = NULL;
    int result = 0;
    struct FreezeState freeze_state;
    // Initialize the freeze state
    init_freeze_state(&freeze_state);

    struct _Py_immutability_state* state = get_immutable_state();
    if(state == NULL){
        PyErr_SetString(PyExc_RuntimeError, "Failed to initialize immutability state");
        return -1;
    }

    PyObject* freeze_location = NULL;
#ifdef Py_DEBUG
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

    while(PyList_Size(frontier) != 0){
        PyObject* item = pop(frontier);
        FreezableCheck check;

        if(_Py_IsImmutable(item)){
            continue;
        }

        if(item == state->blocking_on ||
           item == state->module_locks){
            continue;
        }

        check = check_freezable(state, item);
        switch(check){
            case INVALID_NOT_FREEZABLE:
                PyErr_SetString(PyExc_TypeError, "Invalid freeze request: instance of NotFreezable");
                goto error;

            case INVALID_C_EXTENSIONS:
            {
                PyObject* error_msg = PyUnicode_FromFormat(
                    "Cannot freeze instance of type %s due to custom functionality implemented in C",
                    (item->ob_type->tp_name));
                PyErr_SetObject(PyExc_TypeError, error_msg);
                goto error;
            }

            case VALID_BUILTIN:
            case VALID_EXPLICIT:
            case VALID_IMPLICIT:
                break;

            case FREEZABLE_ERROR:
                goto error;

            default:
                PyErr_SetString(PyExc_RuntimeError, "Unknown freezable check value");
                goto error;
        }

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
        if (add_visited_set(&freeze_state, item) != 0) {
            // If we fail to add the item to the visited set, then we
            // will not be able to backtrack, so go to error case.
            PyErr_SetString(PyExc_RuntimeError, "Failed to add item to visited set");
            goto error;
        }
        _Py_SetImmutable(item);

        if(is_c_wrapper(item)) {
            // C functions are not mutable
            // Types are manually traversed
            continue;
        }

        if(PyFunction_Check(item)){
            if(shadow_function_globals(item) == NULL){
                goto error;
            }
        }

        if(PyType_Check(item)){
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

                // We need to freeze the tuple object, even though the types
                // within will have been frozen already.
                if(push(frontier, type->tp_bases))
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

            if(push(frontier, _PyObject_CAST(Py_TYPE(item)))){
                goto error;
            }
        }
    }

    finish_freeze(&freeze_state);
    goto finally;

error:
    fail_freeze(&freeze_state);
    result = -1;

finally:
    Py_XDECREF(frontier);

    return result;
}