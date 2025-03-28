
#include "Python.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include "pycore_descrobject.h"
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

    // Clear the immutable flag so that finalisers can run correctly.
#if SIZEOF_VOID_P > 4
    op->ob_flags &= ~_Py_IMMUTABLE_FLAG;
#else
    op->ob_refcnt = 0;
#endif
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

    while(PyList_Size(frontier) != 0){
        PyObject* item = pop(frontier);
        FreezableCheck check;

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

        // TODO(Immutable):  mjp: This should be earlier once we have backtracking of freeze.
        // Putting it here makes some things fail the second time they are attempted to be frozen.
        if(_Py_IsImmutable(item)){
            continue;
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

    goto finally;

error:
    result = -1;

finally:
    Py_XDECREF(frontier);

    return result;
}