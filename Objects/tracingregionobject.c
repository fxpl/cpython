#include "Python.h"
#include "pycore_gc.h"            // _PyObject_GC_IS_TRACKED()
#include "pycore_object.h"        // _PyObject_GC_TRACK(), _PyDebugAllocatorStats()

typedef struct {
    PyObject_HEAD
    PyObject *dict;
} TracingRegionObject;


static int
TracingRegion_traverse(TracingRegionObject *self, visitproc visit, void *arg)
{
    Py_VISIT(self->dict);
    return 0;
}

static int
TracingRegion_clear(TracingRegionObject *self)
{
    Py_CLEAR(self->dict);
    return 0;
}

static void
TracingRegion_dealloc(TracingRegionObject *self)
{
    PyObject_GC_UnTrack(self);
    TracingRegion_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
TracingRegion_init(TracingRegionObject *self, PyObject *args, PyObject *kwds)
{
    return 0;
}

static PyMemberDef TracingRegion_members[] = {
    {"__dict__", _Py_T_OBJECT, offsetof(TracingRegionObject, dict), Py_READONLY},
    {NULL}
};

PyTypeObject _PyTracingRegion_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "TracingRegion",
    .tp_basicsize = sizeof(TracingRegionObject),
    .tp_dealloc = (destructor)TracingRegion_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .tp_traverse = (traverseproc)TracingRegion_traverse,
    .tp_clear = (inquiry)TracingRegion_clear,
    .tp_members = TracingRegion_members,
    .tp_dictoffset = offsetof(TracingRegionObject, dict),
    .tp_init = (initproc)TracingRegion_init,
    .tp_new = PyType_GenericNew,
    .tp_reachable = _PyObject_ReachableVisitTypeAndTraverse,
};
