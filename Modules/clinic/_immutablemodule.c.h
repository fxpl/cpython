/*[clinic input]
preserve
[clinic start generated code]*/

#if defined(Py_BUILD_CORE) && !defined(Py_BUILD_CORE_MODULE)
#  include "pycore_gc.h"          // PyGC_Head
#  include "pycore_runtime.h"     // _Py_ID()
#endif
#include "pycore_modsupport.h"    // _PyArg_UnpackKeywords()

PyDoc_STRVAR(_immutable_shallow_freeze__doc__,
"shallow_freeze($module, /, *args)\n"
"--\n"
"\n"
"Shallowly freeze one or more objects.\n"
"\n"
"Each object\'s own state becomes immutable, but the objects it references\n"
"are left untouched and may still be mutable. Every argument counts as a\n"
"root of this call, so objects marked FREEZABLE_EXPLICIT are frozen.\n"
"\n"
"Freezing cannot be undone. Arguments are frozen in order, so if one of\n"
"them cannot be frozen, the ones before it stay frozen.\n"
"\n"
"On failure a TypeError is raised with the object that could not be frozen\n"
"attached to it, so you can inspect it as err.obj.\n"
"\n"
"Returns the first argument.");

#define _IMMUTABLE_SHALLOW_FREEZE_METHODDEF    \
    {"shallow_freeze", _PyCFunction_CAST(_immutable_shallow_freeze), METH_FASTCALL, _immutable_shallow_freeze__doc__},

static PyObject *
_immutable_shallow_freeze_impl(PyObject *module, PyObject * const *args,
                               Py_ssize_t args_length);

static PyObject *
_immutable_shallow_freeze(PyObject *module, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *return_value = NULL;
    PyObject * const *__clinic_args;
    Py_ssize_t args_length;

    __clinic_args = args;
    args_length = nargs;
    return_value = _immutable_shallow_freeze_impl(module, __clinic_args, args_length);

    return return_value;
}

PyDoc_STRVAR(_immutable_deep_freeze__doc__,
"deep_freeze($module, /, *args, atomic=False)\n"
"--\n"
"\n"
"Deeply freeze one or more objects and their graphs.\n"
"\n"
"The objects and everything reachable from them become immutable. Only\n"
"deeply frozen objects can be shared between interpreters. Every argument\n"
"counts as a root of this call, so objects marked FREEZABLE_EXPLICIT are\n"
"frozen.\n"
"\n"
"Freezing is not atomic and cannot be undone. Objects are shallow frozen\n"
"as the graph is walked, so if the call fails part way through, any subset\n"
"of the reachable objects may be left shallow frozen, and they stay that\n"
"way. Nothing is deep frozen unless the whole call succeeds.\n"
"\n"
"On failure a TypeError is raised with the object that could not be frozen\n"
"attached to it, so you can inspect it as err.obj. Once that object is\n"
"dealt with, calling deep_freeze again will finish the job.\n"
"\n"
"Returns the first argument.");

#define _IMMUTABLE_DEEP_FREEZE_METHODDEF    \
    {"deep_freeze", _PyCFunction_CAST(_immutable_deep_freeze), METH_FASTCALL|METH_KEYWORDS, _immutable_deep_freeze__doc__},

static PyObject *
_immutable_deep_freeze_impl(PyObject *module, PyObject * const *args,
                            Py_ssize_t args_length, int atomic);

static PyObject *
_immutable_deep_freeze(PyObject *module, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PyObject *return_value = NULL;
    #if defined(Py_BUILD_CORE) && !defined(Py_BUILD_CORE_MODULE)

    #define NUM_KEYWORDS 1
    static struct {
        PyGC_Head _this_is_not_used;
        PyObject_VAR_HEAD
        Py_hash_t ob_hash;
        PyObject *ob_item[NUM_KEYWORDS];
    } _kwtuple = {
        .ob_base = PyVarObject_HEAD_INIT(&PyTuple_Type, NUM_KEYWORDS)
        .ob_hash = -1,
        .ob_item = { &_Py_ID(atomic), },
    };
    #undef NUM_KEYWORDS
    #define KWTUPLE (&_kwtuple.ob_base.ob_base)

    #else  // !Py_BUILD_CORE
    #  define KWTUPLE NULL
    #endif  // !Py_BUILD_CORE

    static const char * const _keywords[] = {"atomic", NULL};
    static _PyArg_Parser _parser = {
        .keywords = _keywords,
        .fname = "deep_freeze",
        .kwtuple = KWTUPLE,
    };
    #undef KWTUPLE
    PyObject *argsbuf[1];
    PyObject * const *fastargs;
    Py_ssize_t noptargs = 0 + (kwnames ? PyTuple_GET_SIZE(kwnames) : 0) - 0;
    PyObject * const *__clinic_args;
    Py_ssize_t args_length;
    int atomic = 0;

    fastargs = _PyArg_UnpackKeywords(args, nargs, NULL, kwnames, &_parser,
            /*minpos*/ 0, /*maxpos*/ 0, /*minkw*/ 0, /*varpos*/ 1, argsbuf);
    if (!fastargs) {
        goto exit;
    }
    if (!noptargs) {
        goto skip_optional_kwonly;
    }
    atomic = PyObject_IsTrue(fastargs[0]);
    if (atomic < 0) {
        goto exit;
    }
skip_optional_kwonly:
    __clinic_args = args;
    args_length = nargs;
    return_value = _immutable_deep_freeze_impl(module, __clinic_args, args_length, atomic);

exit:
    return return_value;
}

PyDoc_STRVAR(_immutable_is_shallow_frozen__doc__,
"is_shallow_frozen($module, obj, /)\n"
"--\n"
"\n"
"Check if an object is shallowly frozen.\n"
"\n"
"Says nothing about what the object references; use is_deep_frozen() for\n"
"that. If the object is immutable by construction, it will be marked\n"
"shallowly frozen as a side effect and True is returned.");

#define _IMMUTABLE_IS_SHALLOW_FROZEN_METHODDEF    \
    {"is_shallow_frozen", (PyCFunction)_immutable_is_shallow_frozen, METH_O, _immutable_is_shallow_frozen__doc__},

PyDoc_STRVAR(_immutable_is_deep_frozen__doc__,
"is_deep_frozen($module, obj, /)\n"
"--\n"
"\n"
"Check if an object and everything it reaches is frozen.\n"
"\n"
"If the object graph is immutable by construction, it will be deeply\n"
"frozen as a side effect and True is returned.");

#define _IMMUTABLE_IS_DEEP_FROZEN_METHODDEF    \
    {"is_deep_frozen", (PyCFunction)_immutable_is_deep_frozen, METH_O, _immutable_is_deep_frozen__doc__},

PyDoc_STRVAR(_immutable_set_freezable__doc__,
"set_freezable($module, obj, status, /)\n"
"--\n"
"\n"
"Set the freezable status of an object.\n"
"\n"
"Status values:\n"
"  FREEZABLE_YES (0): always freezable\n"
"  FREEZABLE_NO (1): never freezable\n"
"  FREEZABLE_EXPLICIT (2): freezable only when shallow_freeze() or\n"
"                          deep_freeze() is called directly on it\n"
"  FREEZABLE_PROXY (3): reserved for future use");

#define _IMMUTABLE_SET_FREEZABLE_METHODDEF    \
    {"set_freezable", _PyCFunction_CAST(_immutable_set_freezable), METH_FASTCALL, _immutable_set_freezable__doc__},

static PyObject *
_immutable_set_freezable_impl(PyObject *module, PyObject *obj, int status);

static PyObject *
_immutable_set_freezable(PyObject *module, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *return_value = NULL;
    PyObject *obj;
    int status;

    if (!_PyArg_CheckPositional("set_freezable", nargs, 2, 2)) {
        goto exit;
    }
    obj = args[0];
    status = PyLong_AsInt(args[1]);
    if (status == -1 && PyErr_Occurred()) {
        goto exit;
    }
    return_value = _immutable_set_freezable_impl(module, obj, status);

exit:
    return return_value;
}

PyDoc_STRVAR(_immutable_get_freezable__doc__,
"get_freezable($module, obj, /)\n"
"--\n"
"\n"
"Get the freezable status of an object.\n"
"\n"
"Returns the freezable status, or -1 if no status has been set.\n"
"Status values:\n"
"  FREEZABLE_YES (0): always freezable\n"
"  FREEZABLE_NO (1): never freezable\n"
"  FREEZABLE_EXPLICIT (2): freezable only when shallow_freeze() or\n"
"                          deep_freeze() is called directly on it\n"
"  FREEZABLE_PROXY (3): reserved for future use");

#define _IMMUTABLE_GET_FREEZABLE_METHODDEF    \
    {"get_freezable", (PyCFunction)_immutable_get_freezable, METH_O, _immutable_get_freezable__doc__},

PyDoc_STRVAR(_immutable_unset_freezable__doc__,
"unset_freezable($module, obj, /)\n"
"--\n"
"\n"
"Remove any explicitly set freezable status from an object.\n"
"\n"
"After this call, get_freezable(obj) will no longer reflect a\n"
"per-object status and will fall back to the type\'s status (or\n"
"return -1 if neither has been set).");

#define _IMMUTABLE_UNSET_FREEZABLE_METHODDEF    \
    {"unset_freezable", (PyCFunction)_immutable_unset_freezable, METH_O, _immutable_unset_freezable__doc__},
/*[clinic end generated code: output=955bb7f6a2d3b575 input=a9049054013a1b77]*/
