#include "Python.h"
#include <stdbool.h>
#include "object.h"             // _Py_IsImmutable
#include "pycore_descrobject.h" // _PyMethodWrapper_Type
#include "pycore_gc.h"          // _PyGCHead_NEXT, _PyGCHead_PREV, _Py_FROM_GC
#include "pycore_interp.h"      // PyThreadState_Get
#include "pycore_ownership.h"
#include "pycore_pyerrors.h"
#include "pycore_runtime.h"
#include "pyerrors.h"
#include "refcount.h"

// Macro that jumps to error, if the expression `x` does not succeed.
#define SUCCEEDS(x) { do { int r = (x); if (r != 0) goto error; } while (0); }

static int init_state(_Py_ownership_state *state)
{
    state->is_initilized = true;
#ifdef Py_OWNERSHIP_INVARIANT
    state->invariant_state = Py_OWNERSHIP_INVARIANT_DISABLED;
#endif
    return 0;
}

static _Py_ownership_state* get_ownership_state()
{
    PyInterpreterState *interp = PyInterpreterState_Get();
    if (interp == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to get the interpreter state");
        return NULL;
    }

    _Py_ownership_state *state = &interp->ownership;
    if (state->is_initilized == false) {
        if (init_state(state) == -1) {
            PyErr_SetString(PyExc_RuntimeError, "Failed to initialize ownership state");
            return NULL;
        }
    }

    return state;
}

int _PyOwnership_is_c_wrapper(PyObject* obj){
    return PyCFunction_Check(obj) || Py_IS_TYPE(obj, &_PyMethodWrapper_Type) || Py_IS_TYPE(obj, &PyWrapperDescr_Type);
}
int _PyOwnership_traverse_obj(PyObject *obj, visitproc visit, void *data) {
    if (PyType_Check(obj)) {
        // TODO(Immutable): mjp: Special case for types not sure if required. We should review.
        // Additional Note: xFrednet: It looks like each type has a handle to
        // its module, so this is probably needed unless we want to freeze or
        // replace the module pointer?
        PyTypeObject* type = (PyTypeObject*)obj;

        SUCCEEDS(visit(type->tp_dict, data));
        SUCCEEDS(visit(type->tp_mro, data));
        // We need to freeze the tuple object, even though the types
        // within will have been frozen already.
        SUCCEEDS(visit(type->tp_bases, data));
    }
    else
    {
        traverseproc traverse = Py_TYPE(obj)->tp_traverse;
        if(traverse != NULL){
            SUCCEEDS(traverse(obj, visit, data));
        }
    }

    // tp_traverse doesn't cover the object type, this therefore needs
    // to explicitly visit the type.
    SUCCEEDS(visit(_PyObject_CAST(Py_TYPE(obj)), data));

    return 0;
error:
    return -1;
}

// All code belonging to the invariant
#if Py_OWNERSHIP_INVARIANT

// FIXME(Pyrona): This should be on a "Per interpreter state"
bool is_invariant_enabled = false;

static void throw_invariant_error(
    PyObject* src,
    PyObject* tgt,
    const char *format_str,
    PyObject *format_arg
) {
    // Don't stomp existing exception
    PyThreadState *tstate = PyThreadState_Get();
    if (!tstate || _PyErr_Occurred(tstate)) {
        return;
    }

    // Create the error, this sets the error value in `tstate`
    PyErr_Format(PyExc_RuntimeError, format_str, format_arg);

    // Set source and target fields
    // Get the current exception (should be a RuntimeError)
    PyObject *exc = PyErr_GetRaisedException();
    assert(exc && PyObject_TypeCheck(exc, (PyTypeObject *)PyExc_RuntimeError));

    // Add 'source' and 'target' attributes to the exception
    PyObject_SetAttr(exc, &_Py_ID(source), src ? src : Py_None);
    PyObject_SetAttr(exc, &_Py_ID(target), tgt ? tgt : Py_None);

    PyErr_SetRaisedException((PyObject*)exc);
}

// Lifted from Python/gc.c
//******************************** */
typedef struct _gc_runtime_state GCState;
#define GEN_HEAD(gcstate, n) ((n == 0) ? (&(gcstate)->young.head) : (&(gcstate)->old[n - 1].head))
#define GC_NEXT _PyGCHead_NEXT
#define GC_PREV _PyGCHead_PREV
#define FROM_GC _Py_FROM_GC
//******************************** */

static int check_invariant_visit_immutable(PyObject* tgt, void* src_void) {
    PyObject* src = (PyObject*)src_void;

    // C wrappers are special and allowed
    if (_PyOwnership_is_c_wrapper(tgt)) {
        return 0;
    }

    // Make sure the immutable source only points to immutable objects
    if (!_Py_IsImmutable(tgt)) {
        throw_invariant_error(
            src, tgt,
            "Invariant Error: An immutable objects points to a mutable one",
            Py_None);
        return -1;
    }

    return 0;
}

int _PyOwnership_check_invariant(PyThreadState *tstate) {
    _Py_ownership_state *state = get_ownership_state();
    if (state == NULL) {
        return -1;
    }

    // Only run the invariant if it's actully enabled and there is no
    // function which paused the invariant
    if (state->invariant_state != Py_OWNERSHIP_INVARIANT_ENABLED) {
        return 0;
    }

    // Don't run during shutdown. Python needs to mutate data in this state
    // and any breakage will not really matter, since this universe is at
    // its end.
    if (Py_IsFinalizing()) {
        state->invariant_state = Py_OWNERSHIP_INVARIANT_DISABLED;
        return 0;
    }

    // Don't stomp existing exceptions
    if (_PyErr_Occurred(tstate)) {
        return 0;
    }

    // Use the GC data to find all the objects, and traverse them to
    // confirm all their references satisfy the invariant.
    GCState *gcstate = &tstate->interp->gc;

    // There is an cyclic doubly linked list per generation of all the objects
    // in that generation.
    for (int i = NUM_GENERATIONS-1; i >= 0; i--) {
        PyGC_Head *containers = GEN_HEAD(gcstate, i);
        PyGC_Head *gc = GC_NEXT(containers);
        // Walk doubly linked list of objects.
        for (; gc != containers; gc = GC_NEXT(gc)) {
            PyObject *ob = FROM_GC(gc);

            // C wrappers are complicated see description of the called
            // function. We treat them as immutable objects. But we
            // don't traverse them.
            if (_PyOwnership_is_c_wrapper(ob)) {
                continue;
            }

            // Select which validation function should be used, based on the
            // current object.
            visitproc visit = NULL;
            if (_Py_IsImmutable(ob)) {
                visit = (visitproc)check_invariant_visit_immutable;
            } else {
                // The object shouldn't be validated.
                // (This surely won't backfire on us)
                continue;
            }

            // Use traverse proceduce to visit each field of the object.
            SUCCEEDS(_PyOwnership_traverse_obj(ob, visit, ob));
        }
    }

    return 0;

error:
    // Disable the invariant
    state->invariant_state = Py_OWNERSHIP_INVARIANT_DISABLED;
    // Return -1 to indicate an error
    return -1;
}

int _PyOwnership_invariant_enable(void) {
    _Py_ownership_state *state = get_ownership_state();
    if (state == NULL) {
        return -1;
    }

    if (state->invariant_state == Py_OWNERSHIP_INVARIANT_DISABLED) {
        state->invariant_state = Py_OWNERSHIP_INVARIANT_ENABLED;
    }

    return 0;
}

int _PyOwnership_invariant_pause(void) {
    _Py_ownership_state *state = get_ownership_state();
    if (state == NULL) {
        return -1;
    }

    if (state->invariant_state != Py_OWNERSHIP_INVARIANT_DISABLED) {
        state->invariant_state += 1;
    }

    return 0;
}

int _PyOwnership_invariant_resume(void) {
    _Py_ownership_state *state = get_ownership_state();
    if (state == NULL) {
        return -1;
    }

    if (state->invariant_state != Py_OWNERSHIP_INVARIANT_DISABLED) {
        state->invariant_state -= 1;
    }

    return 0;
}
#endif /* Py_OWNERSHIP_INVARIANT */
