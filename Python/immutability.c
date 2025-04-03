
#include "Python.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include "pycore_dict.h"
#include "pycore_object.h"
#include "pycore_immutability.h"


static PyObject* pop(PyObject* s){
    Py_ssize_t size = PyList_Size(s);
    if(size == 0){
        return NULL;
    }

    PyObject* item = PyList_GetItem(s, size - 1);
    if(item == NULL){
        return NULL;
    }

    Py_INCREF(item);

    if(PyList_SetSlice(s, size - 1, size, NULL)){
        Py_DECREF(item);
        return NULL;
    }

    return item;
}

static bool is_c_wrapper(PyObject* obj){
    return PyCFunction_Check(obj) || Py_IS_TYPE(obj, &_PyMethodWrapper_Type) || Py_IS_TYPE(obj, &PyWrapperDescr_Type);
}

#define _Py_VISIT_FUNC_ATTR(attr, frontier) do { \
    if(attr != NULL && !_Py_IsImmutable(attr)){ \
        if(PyList_Append(frontier, attr)){ \
            return PyErr_NoMemory(); \
        } \
    } \
} while(0)


/**
 * Special function for walking the reachable graph of a function object.
 *
 * This is necessary because the function object has a pointer to the global
 * dictionary, and this is problematic because freezing any function directly
 * (as we do with other objects) would make all globals immutable.
 *
 * Instead, we walk the function and find any places where it references
 * global variables or builtins, and then freeze just those objects. The globals
 * and builtins dictionaries for the function are then replaced with frozen
 * copies containing just those globals and builtins we were able to determine
 * the function uses.
 */
static PyObject* walk_function(PyObject* op, PyObject* frontier)
{
    PyObject* builtins = NULL;
    PyObject* frozen_builtins = NULL;
    PyObject* globals = NULL;
    PyObject* frozen_globals = NULL;
    PyObject* module = NULL;
    PyObject* module_dict = NULL;
    PyFunctionObject* f = NULL;
    PyObject* f_ptr = NULL;
    PyCodeObject* f_code = NULL;
    Py_ssize_t size;
    PyObject* f_stack = NULL;
    bool check_globals = false;

    _PyObject_ASSERT(op, PyFunction_Check(op));

    _Py_SetImmutable(op);

    f = (PyFunctionObject*)op;

    globals = f->func_globals;
    builtins = f->func_builtins;

    module = PyImport_Import(f->func_module);
    if(module == NULL){
        // clear the exception so we can check if the module is a namedtuple
        PyObject* exc = PyErr_GetRaisedException();
        _Py_DECLARE_STR(namedtuple, "namedtuple");
        _Py_DECLARE_STR(startswith, "startswith");
        PyObject* res = PyObject_CallMethodOneArg(f->func_module, &_Py_STR(startswith), &_Py_STR(namedtuple));
        if(Py_IsTrue(res)){
            // namedtuple creates a fake module, which cannot be imported
            Py_RETURN_NONE;
        }

        // not a namedtuple, so we need to set the exception
        PyErr_SetRaisedException(exc);
        return NULL;
    }

    if(PyModule_Check(module)){
        module_dict = PyModule_GetDict(module);
    }else{
        module_dict = NULL;
    }

    _Py_VISIT_FUNC_ATTR(f->func_defaults, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_kwdefaults, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_doc, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_name, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_dict, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_closure, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_annotations, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_typeparams, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_qualname, frontier);

    f_stack = PyList_New(0);
    if(f_stack == NULL){
        return PyErr_NoMemory();
    }

    f_ptr = f->func_code;
    if(PyList_Append(f_stack, f_ptr)){
        goto nomemory;
    }

    frozen_builtins = PyDict_New();
    if(frozen_builtins == NULL){
        goto nomemory;
    }

    frozen_globals = PyDict_New();
    if(frozen_globals == NULL){
        goto nomemory;
    }

    while(PyList_Size(f_stack) != 0){
        f_ptr = pop(f_stack);
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
                if(PyDict_SetItem(frozen_globals, name, value)){
                    Py_DECREF(frozen_builtins);
                    Py_DECREF(frozen_globals);
                    Py_DECREF(f_stack);
                    return NULL;
                }
            }else if(PyDict_Contains(builtins, name)){
                PyObject* value = PyDict_GetItem(builtins, name);
                if(PyDict_SetItem(frozen_builtins, name, value)){
                    Py_DECREF(frozen_builtins);
                    Py_DECREF(frozen_globals);
                    Py_DECREF(f_stack);
                    return NULL;
                }
            }else if(PyDict_Contains(module_dict, name)){
                PyObject* value = PyDict_GetItem(module_dict, name);

                _PyDict_SetKeyImmutable((PyDictObject*)module_dict, name);

                if(!_Py_IsImmutable(value)){
                    if(PyList_Append(frontier, value)){
                        goto nomemory;
                    }
                }
            }
        }

        size = PySequence_Fast_GET_SIZE(f_code->co_consts);
        for(Py_ssize_t i = 0; i < size; i++){
            PyObject* value = PySequence_Fast_GET_ITEM(f_code->co_consts, i);
            if(!_Py_IsImmutable(value)){
                if(PyCode_Check(value)){
                    _Py_SetImmutable(value);

                    if(PyList_Append(f_stack, value)){
                        goto nomemory;
                    }
                }else{
                    if(PyList_Append(frontier, value)){
                        goto nomemory;
                    }
                }
            }

            if(check_globals && PyUnicode_Check(value)){
                // if the code calls the globals() builtin, then any
                // cellvar or const in the function could, potentially, refer to
                // a global variable. As such, we need to check if the globals
                // dictionary contains that key and then make it immutable
                // from this point forwards.
                PyObject* name = value;
                if(PyDict_Contains(globals, name)){
                    value = PyDict_GetItem(globals, name);
                    if(PyDict_SetItem(frozen_globals, name, value)){
                        Py_DECREF(frozen_builtins);
                        Py_DECREF(frozen_globals);
                        Py_DECREF(f_stack);
                        return NULL;
                    }
                }
            }
        }
    }

    Py_DECREF(f_stack);

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
                    if(PyDict_SetItem(frozen_globals, name, value)){
                        Py_DECREF(frozen_builtins);
                        Py_DECREF(frozen_globals);
                        return NULL;
                    }
                }
            }
        }
    }

    if(PyList_Append(frontier, frozen_globals)){
        goto nomemory;
    }

    f->func_globals = frozen_globals;
    Py_DECREF(globals);

    if(PyList_Append(frontier, frozen_builtins)){
        goto nomemory;
    }

    f->func_builtins = frozen_builtins;
    Py_DECREF(builtins);

    Py_RETURN_NONE;

nomemory:
    Py_XDECREF(frozen_builtins);
    Py_XDECREF(frozen_globals);
    Py_XDECREF(f_stack);
    return PyErr_NoMemory();
}

static int freeze_visit(PyObject* obj, void* frontier)
{
    if(!_Py_IsImmutable(obj)){
        if(PyList_Append((PyObject*)frontier, obj)){
            PyErr_NoMemory();
            return -1;
        }
    }

    return 0;
}

PyObject* _Py_Freeze(PyObject* obj)
{
    if(_Py_IsImmutable(obj)){
        Py_RETURN_NONE;
    }

    PyObject* frontier = PyList_New(0);
    if(frontier == NULL){
        return PyErr_NoMemory();
    }

    if(PyList_Append(frontier, obj)){
        Py_DECREF(frontier);
        return PyErr_NoMemory();
    }

    PyObject* frozen_importlib = PyImport_ImportModule("_frozen_importlib");
    if(frozen_importlib == NULL){
        Py_DECREF(frontier);
        return NULL;
    }

    PyObject* blocking_on = PyObject_GetAttrString(frozen_importlib, "_blocking_on");
    if(blocking_on == NULL){
        Py_DECREF(frozen_importlib);
        Py_DECREF(frontier);
        return NULL;
    }

    PyObject* module_locks = PyObject_GetAttrString(frozen_importlib, "_module_locks");
    if(module_locks == NULL){
        Py_DECREF(blocking_on);
        Py_DECREF(frozen_importlib);
        Py_DECREF(frontier);
        return NULL;
    }

    Py_DECREF(frozen_importlib);

    while(PyList_Size(frontier) != 0){
        PyObject* item = pop(frontier);

        if(item == blocking_on ||
           item == module_locks){
            // the module lock and blocking on dictionaries must remain mutable or else
            // we will not be able to import modules
            continue;
        }

        PyTypeObject* type = Py_TYPE(item);
        traverseproc traverse;
        PyObject* type_op = NULL;

        if(_Py_IsImmutable(item)){
            continue;
        }

        _Py_SetImmutable(item);

        if(is_c_wrapper(item)) {
            // C functions are not mutable, so we can skip them.
            continue;
        }

        if(PyFunction_Check(item)){
            PyObject* err = walk_function(item, frontier);
            if(!Py_IsNone(err)){
                Py_DECREF(blocking_on);
                Py_DECREF(module_locks);
                Py_DECREF(frontier);
                return err;
            }
        }
        else
        {
            traverse = type->tp_traverse;
            if(traverse != NULL){
                if(traverse(item, (visitproc)freeze_visit, frontier)){
                    Py_DECREF(blocking_on);
                    Py_DECREF(module_locks);
                    Py_DECREF(frontier);
                    return NULL;
                }
            }
        }

        type_op = _PyObject_CAST(item->ob_type);
        if (!_Py_IsImmutable(type_op)){
            if (PyList_Append(frontier, type_op))
            {
                Py_DECREF(blocking_on);
                Py_DECREF(module_locks);
                Py_DECREF(frontier);
                return PyErr_NoMemory();
            }
        }
    }

    Py_DECREF(blocking_on);
    Py_DECREF(module_locks);
    Py_DECREF(frontier);

    Py_RETURN_NONE;
}