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
/*[clinic end generated code: output=4d408aceaaaa05ff input=a9049054013a1b77]*/
