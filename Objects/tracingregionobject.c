#include "Python.h"
#include "pycore_interp.h"
#include "pycore_gc.h"            // _PyObject_GC_IS_TRACKED()
#include "pycore_object.h"        // _PyObject_GC_TRACK(), _PyDebugAllocatorStats()
#include "pycore_descrobject.h"

// #define REGION_TRACING

#ifdef REGION_TRACING
#define if_trace(...) __VA_ARGS__
#define trace_arg(arg) , (Py_uintptr_t)(arg)
#define trace(msg, region, ...) \
    do { \
        printf(msg "\n", (Py_region_t)(region) __VA_OPT__(,) __VA_ARGS__); \
    } while(0)
#define trace_lrc(...) trace(__VA_ARGS__)
#else
#define if_trace(...)
#define trace_arg(...)
#define trace(...)
#define trace_lrc(...)
#endif

/* Macro that jumps to error, if the expression `x` does not succeed. */
#define SUCCEEDS(x) do { int r = (x); if (r != 0) goto error; } while (0)

// ###################################################################
// Copied from gc.c
// ###################################################################

#ifndef Py_GIL_DISABLED
#define GC_NEXT _PyGCHead_NEXT
#define GC_PREV _PyGCHead_PREV

static inline void
gc_set_old_space(PyGC_Head *g, int space)
{
    assert(space == 0 || space == _PyGC_NEXT_MASK_OLD_SPACE_1);
    g->_gc_next &= ~_PyGC_NEXT_MASK_OLD_SPACE_1;
    g->_gc_next |= space;
}

static inline void
gc_list_init(PyGC_Head *list)
{
    // List header must not have flags.
    // We can assign pointer by simple cast.
    list->_gc_prev = (uintptr_t)list;
    list->_gc_next = (uintptr_t)list;
}

static void
gc_list_move(PyGC_Head *node, PyGC_Head *list)
{
    /* Unlink from current list. */
    PyGC_Head *from_prev = GC_PREV(node);
    PyGC_Head *from_next = GC_NEXT(node);
    _PyGCHead_SET_NEXT(from_prev, from_next);
    _PyGCHead_SET_PREV(from_next, from_prev);

    /* Relink at end of new list. */
    // list must not have flags.  So we can skip macros.
    PyGC_Head *to_prev = (PyGC_Head*)list->_gc_prev;
    _PyGCHead_SET_PREV(node, to_prev);
    _PyGCHead_SET_NEXT(to_prev, node);
    list->_gc_prev = (uintptr_t)node;
    _PyGCHead_SET_NEXT(node, list);
}

static inline int
gc_list_is_empty(PyGC_Head *list)
{
    return (list->_gc_next == (uintptr_t)list);
}

static void
gc_list_merge(PyGC_Head *from, PyGC_Head *to)
{
    assert(from != to);
    if (!gc_list_is_empty(from)) {
        PyGC_Head *to_tail = GC_PREV(to);
        PyGC_Head *from_head = GC_NEXT(from);
        PyGC_Head *from_tail = GC_PREV(from);
        assert(from_head != from);
        assert(from_tail != from);

        _PyGCHead_SET_NEXT(to_tail, from_head);
        _PyGCHead_SET_PREV(from_head, to_tail);

        _PyGCHead_SET_NEXT(from_tail, to);
        _PyGCHead_SET_PREV(to, from_tail);
    }
    gc_list_init(from);
}

static struct _gc_runtime_state*
get_gc_state(void)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    return &interp->gc;
}

static inline void
gc_clear_collecting(PyGC_Head *g)
{
    g->_gc_prev &= ~_PyGC_PREV_MASK_COLLECTING;
}

#elif // Py_GIL_DISABLED
#error "We need GIL"
#endif

// ###################################################################
// Copied from regions-main
// ###################################################################

typedef enum {
    Py_MOVABLE_YES = 0,
    Py_MOVABLE_NO = 1,
    Py_MOVABLE_FREEZE = 2,
} movable_status;

movable_status get_movable_status(PyObject *obj) {
    // FIXME(regions): xFrednet: Currently it's not possible to set
    // the movability per object. This instead returns the default
    // movability for objects. Note that some shallow immutable objects
    // will not return freeze as their movability.

    // Immortal object have no real RC, this makes it infeasible to have them
    // in a region and dynamically track their ownership. Immortal objects are
    // intended to be immutable in Python, so it should be safe to implicitly
    // freeze them.
    if (_Py_IsImmortal(obj)) {
        return Py_MOVABLE_FREEZE;
    }

    // Immutable objects don't need to be moved
    if (_Py_IsImmutable(obj)) {
        return Py_MOVABLE_FREEZE;
    }

    // Types are a pain for regions since it's likely that objects of one type may
    // end up in multiple regions, requiring the type to be frozen. Types also
    // have a lot of reference pointing to them. Let's hope there is no need to
    // keep them freezable
    if (PyType_Check(obj)) {
        return Py_MOVABLE_FREEZE;
    }

    // Module objects are also complicated. Freezing them should turn most modules
    // into proxys which should make them mostly usable.
    if (PyModule_Check(obj)) {
        return Py_MOVABLE_FREEZE;
    }

    // Functions are a mess as well, making the entire system reachable. Freezing
    // them should again just magically make most things work
    if (PyFunction_Check(obj)) {
        return Py_MOVABLE_FREEZE;
    }

    // CWrappers can't really be owned, but need some special handling since
    // interpreters could still race on their RC. Solution, throw them in the
    // freezer
    if (PyCFunction_Check(obj)
        || Py_IS_TYPE(obj, &_PyMethodWrapper_Type)
        || Py_IS_TYPE(obj, &PyWrapperDescr_Type)
    ) {
        return Py_MOVABLE_FREEZE;
    }

    // Freezing or moving these objects is... complicated. In some cases it is
    // possible but more hassle than it's probably worth. For not we mark them
    // all as unmovable.
    if (PyFrame_Check(obj)
        || PyGen_CheckExact(obj)
        || PyCoro_CheckExact(obj)
        || PyAsyncGen_CheckExact(obj)
        || PyAsyncGenASend_CheckExact(obj)
    ) {
        return Py_MOVABLE_NO;
    }

    // Exceptions don't hold anything obviously problematic preventing them
    // from being moved into a region. The actual problem is that the runtime
    // stores references to them and that these are already emitted on an
    // error path. Moving them into a region could add more problems.
    // We should discuss how to handle these, maybe freezing is the correct
    // approach?
    if (PyExceptionInstance_Check(obj)) {
        return Py_MOVABLE_NO;
    }

    // For now, we define all other objects as movable by default. (Surely
    // this will not backfire)
    return Py_MOVABLE_YES;
}

// ###################################################################
// Tracing Impl
// ###################################################################

typedef struct {
    Py_ssize_t objs;
    Py_ssize_t incoming_refs;
} trace_res;

static trace_res trace_object(PyObject* obj) {
    trace_res res = {
        .objs = 1,
        .incoming_refs = 2,
    };

    return res;
}

// ###################################################################
// Region Object
// ###################################################################

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

static PyObject* TracingRegion_trace(PyObject *op) {
    trace_res res = trace_object(op);

    PyObject *t = Py_BuildValue("(ii)", res.objs, res.incoming_refs);
    if (t == NULL) {
        return NULL;  // propagate Python exception
    }

    return t;
}

static PyMethodDef TracingRegion_methods[] = {
    {"trace", _PyCFunction_CAST(TracingRegion_trace), METH_NOARGS,
        "This traces the region and returns the number of incoming references"},
    {NULL,              NULL}           /* sentinel */
};

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
    .tp_methods = TracingRegion_methods,
    .tp_dictoffset = offsetof(TracingRegionObject, dict),
    .tp_new = PyType_GenericNew,
    .tp_reachable = _PyObject_ReachableVisitTypeAndTraverse,
};


