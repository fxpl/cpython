/*
 * _test_reachable - Test module for tp_reachable freeze warnings.
 *
 * Provides types that deliberately lack tp_reachable so we can test
 * the freeze-time warnings in traverse_freeze().
 *
 *   HasTraverseNoReachable  - has tp_traverse, no tp_reachable
 *   NoTraverseNoReachable   - no  tp_traverse, no tp_reachable
 *   HasReachable            - has both tp_traverse and tp_reachable (no warning)
 */

#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#include "Python.h"

/* ---- HasTraverseNoReachable ------------------------------------------- */

typedef struct {
    PyObject_HEAD
    PyObject *value;
} HasTraverseNoReachableObject;

static int
htnr_traverse(PyObject *self, visitproc visit, void *arg)
{
    HasTraverseNoReachableObject *obj = (HasTraverseNoReachableObject *)self;
    Py_VISIT(obj->value);
    return 0;
}

static int
htnr_clear(PyObject *self)
{
    HasTraverseNoReachableObject *obj = (HasTraverseNoReachableObject *)self;
    Py_CLEAR(obj->value);
    return 0;
}

static void
htnr_dealloc(PyObject *self)
{
    PyObject_GC_UnTrack(self);
    htnr_clear(self);
    Py_TYPE(self)->tp_free(self);
}

static int
htnr_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    HasTraverseNoReachableObject *obj = (HasTraverseNoReachableObject *)self;
    PyObject *value = Py_None;
    if (!PyArg_ParseTuple(args, "|O", &value))
        return -1;
    Py_XSETREF(obj->value, Py_NewRef(value));
    return 0;
}

static PyTypeObject HasTraverseNoReachable_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_test_reachable.HasTraverseNoReachable",
    .tp_basicsize = sizeof(HasTraverseNoReachableObject),
    .tp_dealloc = htnr_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_doc = "Type with tp_traverse but no tp_reachable.",
    .tp_traverse = htnr_traverse,
    .tp_clear = htnr_clear,
    .tp_init = htnr_init,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = PyType_GenericNew,
    .tp_free = PyObject_GC_Del,
    /* tp_reachable deliberately left NULL */
};

/* ---- NoTraverseNoReachable ------------------------------------------- */

typedef struct {
    PyObject_HEAD
    int dummy;
} NoTraverseNoReachableObject;

static int
ntnr_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    return 0;
}

static PyTypeObject NoTraverseNoReachable_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_test_reachable.NoTraverseNoReachable",
    .tp_basicsize = sizeof(NoTraverseNoReachableObject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Type with no tp_traverse and no tp_reachable.",
    .tp_init = ntnr_init,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = PyType_GenericNew,
    /* tp_traverse deliberately left NULL */
    /* tp_reachable deliberately left NULL */
};

/* ---- HasReachable ---------------------------------------------------- */

typedef struct {
    PyObject_HEAD
    PyObject *value;
} HasReachableObject;

static int
hr_traverse(PyObject *self, visitproc visit, void *arg)
{
    HasReachableObject *obj = (HasReachableObject *)self;
    Py_VISIT(obj->value);
    return 0;
}

static int
hr_reachable(PyObject *self, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(self));
    return hr_traverse(self, visit, arg);
}

static int
hr_clear(PyObject *self)
{
    HasReachableObject *obj = (HasReachableObject *)self;
    Py_CLEAR(obj->value);
    return 0;
}

static void
hr_dealloc(PyObject *self)
{
    PyObject_GC_UnTrack(self);
    hr_clear(self);
    Py_TYPE(self)->tp_free(self);
}

static int
hr_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    HasReachableObject *obj = (HasReachableObject *)self;
    PyObject *value = Py_None;
    if (!PyArg_ParseTuple(args, "|O", &value))
        return -1;
    Py_XSETREF(obj->value, Py_NewRef(value));
    return 0;
}

static PyTypeObject HasReachable_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_test_reachable.HasReachable",
    .tp_basicsize = sizeof(HasReachableObject),
    .tp_dealloc = hr_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_doc = "Type with both tp_traverse and tp_reachable.",
    .tp_traverse = hr_traverse,
    .tp_clear = hr_clear,
    .tp_init = hr_init,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = PyType_GenericNew,
    .tp_free = PyObject_GC_Del,
    .tp_reachable = hr_reachable,
};

/* ---- Module ---------------------------------------------------------- */

/* ---- ShallowImmutable ------------------------------------------------ */
/*
 * A C-level type registered as shallow immutable via
 * _PyImmutability_RegisterShallowImmutable.  Instances hold a single
 * PyObject* but are declared shallow-immutable, meaning the implicit
 * check trusts that the instance itself won't be mutated.
 */

typedef struct {
    PyObject_HEAD
    PyObject *value;
} ShallowImmutableObject;

static int
si_traverse(PyObject *self, visitproc visit, void *arg)
{
    ShallowImmutableObject *obj = (ShallowImmutableObject *)self;
    Py_VISIT(obj->value);
    return 0;
}

static int
si_reachable(PyObject *self, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(self));
    return si_traverse(self, visit, arg);
}

static int
si_clear(PyObject *self)
{
    ShallowImmutableObject *obj = (ShallowImmutableObject *)self;
    Py_CLEAR(obj->value);
    return 0;
}

static void
si_dealloc(PyObject *self)
{
    PyObject_GC_UnTrack(self);
    si_clear(self);
    Py_TYPE(self)->tp_free(self);
}

static int
si_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    ShallowImmutableObject *obj = (ShallowImmutableObject *)self;
    PyObject *value = Py_None;
    if (!PyArg_ParseTuple(args, "|O", &value))
        return -1;
    Py_XSETREF(obj->value, Py_NewRef(value));
    return 0;
}

static PyTypeObject ShallowImmutable_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_test_reachable.ShallowImmutable",
    .tp_basicsize = sizeof(ShallowImmutableObject),
    .tp_dealloc = si_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .tp_doc = "C-level type registered as shallow immutable.",
    .tp_traverse = si_traverse,
    .tp_clear = si_clear,
    .tp_init = si_init,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = PyType_GenericNew,
    .tp_free = PyObject_GC_Del,
    .tp_reachable = si_reachable,
};

static int
_test_reachable_exec(PyObject *module)
{
    /* Ready both types and register them as freezable */
    if (PyType_Ready(&HasTraverseNoReachable_Type) < 0)
        return -1;
    /* Reset tp_reachable to NULL in case Ready inherited one */
    HasTraverseNoReachable_Type.tp_reachable = NULL;
    if (PyModule_AddType(module, &HasTraverseNoReachable_Type) != 0)
        return -1;
    if (_PyImmutability_RegisterFreezable(&HasTraverseNoReachable_Type) < 0)
        return -1;

    if (PyType_Ready(&NoTraverseNoReachable_Type) < 0)
        return -1;
    /* Reset tp_reachable to NULL in case Ready inherited one */
    NoTraverseNoReachable_Type.tp_reachable = NULL;
    if (PyModule_AddType(module, &NoTraverseNoReachable_Type) != 0)
        return -1;
    if (_PyImmutability_RegisterFreezable(&NoTraverseNoReachable_Type) < 0)
        return -1;

    if (PyType_Ready(&HasReachable_Type) < 0)
        return -1;
    if (PyModule_AddType(module, &HasReachable_Type) != 0)
        return -1;
    if (_PyImmutability_RegisterFreezable(&HasReachable_Type) < 0)
        return -1;

    if (PyType_Ready(&ShallowImmutable_Type) < 0)
        return -1;
    if (PyModule_AddType(module, &ShallowImmutable_Type) != 0)
        return -1;
    if (_PyImmutability_RegisterFreezable(&ShallowImmutable_Type) < 0)
        return -1;
    if (_PyImmutability_RegisterShallowImmutable(&ShallowImmutable_Type) < 0)
        return -1;

    return 0;
}

static PyModuleDef_Slot _test_reachable_slots[] = {
    {Py_mod_exec, _test_reachable_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL},
};

static struct PyModuleDef _test_reachable_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_test_reachable",
    .m_doc = "Test module for tp_reachable freeze warnings.",
    .m_size = 0,
    .m_slots = _test_reachable_slots,
};

PyMODINIT_FUNC
PyInit__test_reachable(void)
{
    return PyModuleDef_Init(&_test_reachable_module);
}
