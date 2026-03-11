/*[clinic input]
preserve
[clinic start generated code]*/

#include "pycore_modsupport.h"    // _PyArg_CheckPositional()

PyDoc_STRVAR(_immutable_register_freezable__doc__,
"register_freezable($module, obj, /)\n"
"--\n"
"\n"
"Register a type as freezable.");

#define _IMMUTABLE_REGISTER_FREEZABLE_METHODDEF    \
    {"register_freezable", (PyCFunction)_immutable_register_freezable, METH_O, _immutable_register_freezable__doc__},

PyDoc_STRVAR(_immutable_freeze__doc__,
"freeze($module, /, *args)\n"
"--\n"
"\n"
"Freeze one or more objects and their graphs.");

#define _IMMUTABLE_FREEZE_METHODDEF    \
    {"freeze", _PyCFunction_CAST(_immutable_freeze), METH_FASTCALL, _immutable_freeze__doc__},

PyDoc_STRVAR(_immutable_isfrozen__doc__,
"isfrozen($module, obj, /)\n"
"--\n"
"\n"
"Check if an object is frozen (or can be viewed as immutable).\n"
"\n"
"If the object graph can be viewed as immutable, it will be frozen as a\n"
"side effect and True is returned.");

#define _IMMUTABLE_ISFROZEN_METHODDEF    \
    {"isfrozen", (PyCFunction)_immutable_isfrozen, METH_O, _immutable_isfrozen__doc__},

PyDoc_STRVAR(_immutable_set_freezable__doc__,
"set_freezable($module, obj, status, /)\n"
"--\n"
"\n"
"Set the freezable status of an object.\n"
"\n"
"Status values:\n"
"  FREEZABLE_YES (0): always freezable\n"
"  FREEZABLE_NO (1): never freezable\n"
"  FREEZABLE_EXPLICIT (2): freezable only when freeze() is called directly on it\n"
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
/*[clinic end generated code: output=6ccbbe8cca58c3bb input=a9049054013a1b77]*/
