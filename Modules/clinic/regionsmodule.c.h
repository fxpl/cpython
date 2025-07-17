/*[clinic input]
preserve
[clinic start generated code]*/

PyDoc_STRVAR(regions_is_local__doc__,
"is_local($module, obj, /)\n"
"--\n"
"\n"
"Return True the object is in the local region.");

#define REGIONS_IS_LOCAL_METHODDEF    \
    {"is_local", (PyCFunction)regions_is_local, METH_O, regions_is_local__doc__},

static int
regions_is_local_impl(PyObject *module, PyObject *obj);

static PyObject *
regions_is_local(PyObject *module, PyObject *obj)
{
    PyObject *return_value = NULL;
    int _return_value;

    _return_value = regions_is_local_impl(module, obj);
    if ((_return_value == -1) && PyErr_Occurred()) {
        goto exit;
    }
    return_value = PyBool_FromLong((long)_return_value);

exit:
    return return_value;
}

PyDoc_STRVAR(regions_get_last_dirty_reason__doc__,
"get_last_dirty_reason($module, /)\n"
"--\n"
"\n"
"Returns the last reason for marking open regions as dirty.\n"
"\n"
"Return value: str");

#define REGIONS_GET_LAST_DIRTY_REASON_METHODDEF    \
    {"get_last_dirty_reason", (PyCFunction)regions_get_last_dirty_reason, METH_NOARGS, regions_get_last_dirty_reason__doc__},

static PyObject *
regions_get_last_dirty_reason_impl(PyObject *module);

static PyObject *
regions_get_last_dirty_reason(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    return regions_get_last_dirty_reason_impl(module);
}
/*[clinic end generated code: output=42af3f0e45d27e2f input=a9049054013a1b77]*/
