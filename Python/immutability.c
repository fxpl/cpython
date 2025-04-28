
#include "Python.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include "pycore_object.h"
#include "pycore_immutability.h"


PyDoc_STRVAR(notfreezable_doc,
    "NotFreezable()\n\
    \n\
    Indicate that a type cannot be frozen.");


PyTypeObject PyNotFreezable_Type = {
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

    if(check_globals){
        // if the code calls the globals() builtin, then any
        // cellvar or const in the function could, potentially, refer to
        // a global variable. As such, we need to check if the globals
        // dictionary contains that key and then make it immutable
        // from this point forwards.
        // we need to check the closure for any cellvars that are not
        // referenced in the code object, but are still used in the function
        size = 0;
        if(f->func_closure != NULL)
            size = PySequence_Fast_GET_SIZE(f->func_closure);

        for(Py_ssize_t i=0; i < size; ++i){
            PyObject* cellvar = PySequence_Fast_GET_ITEM(f->func_closure, i);
            PyObject* value = PyCell_GET(cellvar);

            if(PyUnicode_Check(value)){
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

static bool can_freeze(PyTypeObject* type, PyObject* freezable_types)
{
    PyObject* type_op = _PyObject_CAST(type);
    if(_Py_IsImmutable(type_op)){
        return true;
    }

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
       type == &PyMemoryView_Type ||
       type == &PyByteArray_Type ||
       type == &PyRange_Type ||
       type == &PyGetSetDescr_Type ||
       type == &PyMemberDescr_Type ||
       type == &PyProperty_Type ||
       type == &PyWrapperDescr_Type ||
       type == &PyMethodDescr_Type ||
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

    if(type == &PyNotFreezable_Type || PyType_IsSubtype(type, &PyNotFreezable_Type)){
        PyErr_SetString(PyExc_TypeError, "Cannot freeze NotFreezable type");
        return false;
    }

    if(freezable_types == NULL){
        return false;
    }

    type_op = _PyObject_CAST(type);
    if (PySet_Contains(freezable_types, type_op) == 1) {
        return true;
    }

    if(Py_IsFalse(_PyType_UsesDefaultSlots(type))){
        return false;
    }

    return true;
}


static int init_state(struct _Py_immutability_state *state)
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

    return 0;
}

static struct _Py_immutability_state* get_immutable_state(PyObject* module)
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

PyObject* _PyImmutability_RegisterFreezable(PyObject* obj)
{
    struct _Py_immutability_state *state = get_immutable_state(obj);
    if(state == NULL){
        PyErr_SetString(PyExc_RuntimeError, "Failed to initialize immutability state");
        return NULL;
    }

    if(!PyType_Check(obj)){
        PyErr_SetString(PyExc_TypeError, "Expected a type");
        return NULL;
    }

    if(PySet_Add(state->freezable_types, obj) == -1){
        return NULL;
    }

    Py_RETURN_NONE;
}


PyObject* _PyImmutability_Freeze(PyObject* obj)
{
    PyObject* frontier = NULL;
    PyObject* result = Py_None;
    struct _Py_immutability_state* state = get_immutable_state(obj);
    if(state == NULL){
        PyErr_SetString(PyExc_RuntimeError, "Failed to initialize immutability state");
        return NULL;
    }

    if(_Py_IsImmutable(obj)){
        return result;
    }

    frontier = PyList_New(0);
    if(frontier == NULL){
        result = PyErr_NoMemory();
        goto cleanup;
    }

    if(push(frontier, obj)){
        result = PyErr_NoMemory();
        goto cleanup;
    }

    while(PyList_Size(frontier) != 0){
        PyObject* item = pop(frontier);

        if(item == state->blocking_on ||
           item == state->module_locks){
            continue;
        }

        if(!can_freeze(item->ob_type, state->freezable_types)){
            PyObject* error_msg = PyUnicode_FromFormat("Cannot freeze object of type %s", Py_TYPE(item)->tp_name);
            PyErr_SetObject(PyExc_TypeError, error_msg);
            result = NULL;
            goto cleanup;
        }

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
                goto cleanup;
            }
        }

        if(PyType_Check(item)){
            PyTypeObject* type = (PyTypeObject*)item;
            if(push(frontier, type->tp_dict))
            {
                result = PyErr_NoMemory();
                goto cleanup;
            }

            if(PySet_Contains(state->freezable_types, item) != 1){
                // type is not explicit freezable, so we need to check its bases
                if(push(frontier, type->tp_mro))
                {
                    result = PyErr_NoMemory();
                    goto cleanup;
                }
            }
        }
        else
        {
            traverseproc traverse = Py_TYPE(item)->tp_traverse;
            if(traverse != NULL){
                if(traverse(item, (visitproc)freeze_visit, frontier)){
                    result = NULL;
                    goto cleanup;
                }
            }

            if(push(frontier, _PyObject_CAST(Py_TYPE(item)))){
                result = PyErr_NoMemory();
                goto cleanup;
            }
        }
    }

cleanup:
    Py_XDECREF(frontier);

    return result;
}