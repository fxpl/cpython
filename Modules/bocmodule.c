#define PY_SSIZE_T_CLEAN  // Not necessary since Python 3.13
#include <Python.h>

// SpamError exception
static PyObject *SpamError = NULL;

static int
spam_module_exec(PyObject *m)
{
    if (SpamError != NULL) {
        PyErr_SetString(PyExc_ImportError,
                        "cannot initialise spam module more than once");
        return -1;
    }
    SpamError = PyErr_NewException("spam.error", NULL, NULL);
    if (PyModule_AddObjectRef(m, "SpamError", SpamError)) {
        return -1;
    }

    return 0;
}

static PyObject *
spam_system(PyObject *self, PyObject *args)
{
    const char *command;

    if (!PyArg_ParseTuple(args, "s", &command)) {
        return NULL;
    }

    int sts = system(command);
    if (sts < 0) {
        PyErr_SetString(SpamError, "System command failed");
        return NULL;
    }

    return PyLong_FromLong(sts);
}

static PyMethodDef spam_methods[] = {
    { "system", spam_system, METH_VARARGS, "Execute a shell command." },
    { NULL,     NULL,        0,            NULL                       }  // Sentinel
};

static PyModuleDef_Slot spam_module_slots[] = {
    { Py_mod_exec, spam_module_exec },
    { 0,           NULL             }
};

static struct PyModuleDef spam_module = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "spam",
    .m_size = 0,  // non-negative
    .m_methods = spam_methods,
    .m_slots = spam_module_slots,
};

PyMODINIT_FUNC
PyInit_spam(void)
{
    return PyModuleDef_Init(&spam_module);
}
