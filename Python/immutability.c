
#include "Python.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include "pycore_descrobject.h"
#include "pycore_gc.h"
#include "pycore_object.h"
#include "pycore_immutability.h"
#include "pycore_interp.h"
#include "pycore_list.h"
#include "pycore_weakref.h"
#include "pycore_setobject.h"

// This file has many in progress aspects
//
// 1. Support GIL disabled mode properly.
// 2. Improve storage of freeze_location
// 3. Improve Mermaid output to handle re-entrancy


// #define IMMUTABLE_TRACING

#ifdef IMMUTABLE_TRACING
#define debug(msg, ...) \
   do { \
       printf(msg __VA_OPT__(,) __VA_ARGS__); \
   } while(0)
#define debug_obj(msg, obj, ...) \
   do { \
       PyObject* repr = PyObject_Repr(obj); \
       printf(msg, PyUnicode_AsUTF8(repr), obj __VA_OPT__(,) __VA_ARGS__); \
       Py_DECREF(repr); \
   } while(0)
#else
#define debug(...)
#define debug_obj(...)
#endif

// #define MERMAID_TRACING
#ifdef MERMAID_TRACING
#define TRACE_MERMAID_START() \
    do { \
        FILE* f = fopen("freeze_trace.md", "w"); \
        if (f != NULL) { \
            fprintf(f, "```mermaid\n"); \
            fprintf(f, "graph LR\n"); \
            fclose(f); \
        } \
    } while(0)

#define TRACE_MERMAID_NODE(obj) \
    do { \
        FILE* f = fopen("freeze_trace.md", "a"); \
        if (f != NULL) { \
            fprintf(f, "    %p[\"%s (rc=%zd) - %p\"]\n", \
                (void*)obj, (PyObject*)obj->ob_type->tp_name, \
                Py_REFCNT(obj), (void*)obj); \
            fclose(f); \
        } \
    } while(0)

#define TRACE_MERMAID_EDGE(from, to) \
    do { \
        FILE* f = fopen("freeze_trace.md", "a"); \
        if (f != NULL) { \
            fprintf(f, "    %p --> %p\n", (void*)from, (void*)to); \
            fclose(f); \
        } \
    } while(0)

#define TRACE_MERMAID_END() \
    do { \
        FILE* f = fopen("freeze_trace.md", "a"); \
        if (f != NULL) { \
            fprintf(f, "```\n"); \
            fclose(f); \
        } \
    } while(0)
#else
#define TRACE_MERMAID_START()
#define TRACE_MERMAID_NODE(obj)
#define TRACE_MERMAID_EDGE(from, to)
#define TRACE_MERMAID_END()
#endif

#define IMMUTABLE_FLAG_FIELD(op) (op->ob_flags)

// Macro that jumps to error, if the expression `x` does not succeed.
#define SUCCEEDS(x) { do { int r = (x); if (r != 0) goto error; } while (0); }

static
int init_state(struct _Py_immutability_state *state)
{
    state->warned_types = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if(state->warned_types == NULL){
        return -1;
    }

    state->immutable_by_construction_types = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if(state->immutable_by_construction_types == NULL){
        _Py_hashtable_destroy(state->warned_types);
        state->warned_types = NULL;
        return -1;
    }

    // Register built-in shallow immutable types.
    // These types produce objects that are individually immutable
    // but may reference other objects (e.g. tuple elements).
    PyTypeObject *shallow_types[] = {
        &PyTuple_Type,
        &PyFrozenSet_Type,
        &PyCode_Type,
        &PyRange_Type,
        &PyBytes_Type,
        &PyUnicode_Type,
        &PyLong_Type,
        &PyFloat_Type,
        &PyComplex_Type,
        &PyBool_Type,
        &_PyNone_Type,
        &PyEllipsis_Type,
        &_PyNotImplemented_Type,
        &PyCFunction_Type,
        NULL
    };
    for (int i = 0; shallow_types[i] != NULL; i++) {
        if (_PyImmutability_RegisterImmutableByConstruction(shallow_types[i])) {
            return -1;
        }
    }

    PyTypeObject *builtin_freezable_types[] = {
        &PyType_Type,
        &PyBaseObject_Type,
        &PyFunction_Type,
        &PyList_Type,
        &PyDict_Type,
        &PySet_Type,
        &PyMemoryView_Type,
        &PyByteArray_Type,
        &PyGetSetDescr_Type,
        &PyMemberDescr_Type,
        &PyProperty_Type,
        &PyWrapperDescr_Type,
        &PyMethodDescr_Type,
        &PyClassMethod_Type, // TODO(Immutable): mjp I added this, is it correct? Discuss with maj
        &PyClassMethodDescr_Type,
        &PyStaticMethod_Type,
        &PyMethod_Type,
        &PyCapsule_Type,
        &PyCode_Type,
        &PyCell_Type,
        &PyFrame_Type,
        &_PyWeakref_RefType,
        &PyModule_Type, // TODO(Immutable): mjp I added this, is it correct? Discuss with maj
        &_PyImmModule_Type,
        &PyCFunction_Type,
        &_PyMethodWrapper_Type,
        NULL
    };
    for (int i = 0; builtin_freezable_types[i] != NULL; i++) {
        if (_PyImmutability_SetFreezable((PyObject*)builtin_freezable_types[i], _Py_FREEZABLE_YES)) {
            return -1;
        }
    }

    if (_PyImmutability_SetFreezable((PyObject*)&PyModule_Type, _Py_FREEZABLE_PROXY)) {
        return -1;
    }
    return 0;
}

static struct _Py_immutability_state* get_immutable_state(void)
{
    PyInterpreterState* interp = PyInterpreterState_Get();
    struct _Py_immutability_state *state = &interp->immutability;
    if(state->immutable_by_construction_types == NULL){
        if(init_state(state) == -1){
            PyErr_SetString(PyExc_RuntimeError, "Failed to initialize immutability state");
            return NULL;
        }
    }

    return state;
}


static int push_borrow(PyObject* s, PyObject* item){
    if(item == NULL){
        return 0;
    }

    if(!PyList_Check(s)){
        PyErr_SetString(PyExc_TypeError, "Expected a list");
        return -1;
    }

    return _PyList_AppendTakeRef(_PyList_CAST(s), item);
}
static int push(PyObject* s, PyObject* item){
    return push_borrow(s, _Py_NewRef(item));
}

// Depend on internal list pop implementation to avoid
// unnecessary refcount operations.
static PyObject* pop(PyObject* s){
    PyObject* item;
    Py_ssize_t size = PyList_Size(s);
    if(size == 0){
        return NULL;
    }

    // The push doesn't incref, so can avoid the extra
    // incref/decref here by using the internal pop.
    item = _Py_ListPop((PyListObject *)s, size - 1);
    if(item == NULL){
        PyErr_SetString(PyExc_RuntimeError, "Internal error: Failed to pop from list");
        return NULL;
    }

    return item;
}

static bool is_c_wrapper(PyObject* obj){
    return PyCFunction_Check(obj) || Py_IS_TYPE(obj, &_PyMethodWrapper_Type) || Py_IS_TYPE(obj, &PyWrapperDescr_Type);
}

typedef struct shallow_freeze_state_t {
    // A PyList used to track what objects still need to be frozen
    PyObject *pending;

    // A hashtable with all objects visited by this freeze.
    //
    // These are weakreferences, since we hold references to the root
    // and all object are immutable, it should be safe.
    _Py_hashtable_t *visited;

    // The objects that freeze() was called directly on.
    _Py_hashtable_t *roots;

#ifdef Py_DEBUG
    // For debugging, track the stack trace of the freeze operation.
    PyObject* freeze_location;
#endif
} shallow_freeze_state_t;

static int
is_root(shallow_freeze_state_t *state, PyObject *obj)
{
    return _Py_hashtable_get(state->roots, obj) != NULL;
}

static void dealloc_shallow_freeze_state(shallow_freeze_state_t *state) {
    Py_CLEAR(state->pending);

    if (state->visited != NULL) {
        _Py_hashtable_destroy(state->visited);
        state->visited = NULL;
    }

    if (state->roots != NULL) {
        _Py_hashtable_destroy(state->roots);
        state->roots = NULL;
    }
}

static int init_shallow_freeze_state(shallow_freeze_state_t *state) {
    state->pending = NULL;
    state->visited = NULL;
    state->roots = NULL;

    state->pending = PyList_New(0);
    if (state->pending == NULL) {
        goto error;
    }

    state->visited = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if (state->visited == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    state->roots = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if (state->roots == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    return 0;
error:
    dealloc_shallow_freeze_state(state);
    return -1;
}

// Wrapper around tp_traverse that also visits the type object.
// tp_traverse does not visit the type for non-heap types, but
// tp_reachable should visit all reachable objects including the type.
static int
traverse_via_tp_traverse(PyObject *obj, visitproc visit, void *freeze_state_untyped)
{
    PyTypeObject *tp = Py_TYPE(obj);

    // `tp_traverse` of heap types *should* include a
    // `Py_VISIT(Py_TYPE(self));` since around Python 2.7 but
    // there are still plenty of types that don't. LLMs currently
    // also don't do this consistently. So, instead of visiting the
    // type directly we throw it on to the DFS stack to check the
    // correct behavior on back traversal.
    //
    // Only push the type if it's still mutable and not pending
    if (!_Py_IsDeepImmutable(tp)) {
        shallow_freeze_state_t *freeze_state = (shallow_freeze_state_t *)freeze_state_untyped;
        SUCCEEDS(push(freeze_state->pending, _PyObject_CAST(tp)));
    }

    traverseproc traverse = tp->tp_traverse;
    if (traverse != NULL) {
        int err = traverse(obj, visit, freeze_state_untyped);
        if (err) {
            return err;
        }
    }

    // Manually visit the type if it's a static type
    if (!(tp->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        return visit((PyObject *)Py_TYPE(obj), freeze_state_untyped);
    }

    return 0;

error:
    return -1;
}

// Returns the appropriate traversal function for reaching all references
// from an object. Prefers tp_reachable, falls back to tp_traverse wrapped
// to also visit the type. Emits a warning once per type on fallback.
static traverseproc
get_reachable_proc(PyTypeObject *tp)
{
    if (tp->tp_reachable != NULL) {
        return tp->tp_reachable;
    }

    struct _Py_immutability_state *imm_state = get_immutable_state();
    if (imm_state != NULL &&
        _Py_hashtable_get(imm_state->warned_types, (void *)tp) == NULL)
    {
        _Py_hashtable_set(imm_state->warned_types, (void *)tp, (void *)1);
        if (tp->tp_traverse != NULL) {
            PySys_FormatStderr(
                "freeze: type '%.100s' has tp_traverse but no tp_reachable\n",
                tp->tp_name);
        } else {
            PySys_FormatStderr(
                "freeze: type '%.100s' has no tp_traverse and no tp_reachable\n",
                tp->tp_name);
        }
    }

    // Always return the wrapper; even when tp_traverse is NULL, the wrapper
    // will still visit the type object which tp_reachable is expected to do.
    return traverse_via_tp_traverse;
}

static inline void _Py_SetShallowImmutable(PyObject *op)
{
    if (op) {
        op->ob_flags |= _Py_IMMUTABLE_FLAG;
    }
}
static inline void _Py_SetDeepImmutable(PyObject *op)
{
    assert(_Py_IsShallowImmutable(op));
    op->ob_flags |= _Py_IMMUTABLE_DEPTH_FLAG;
}

// Copy-pasted from weakrefobject.c
static void weakref_handle_callback(PyWeakReference* ref, PyObject* callback)
{
    PyObject* cbresult = PyObject_CallOneArg(callback, (PyObject*)ref);

    if (cbresult == NULL) {
        PyErr_FormatUnraisable("Exception ignored while "
                               "calling weakref callback %R", callback);
    }
    else {
        Py_DECREF(cbresult);
    }
}

// Copy-pasted from weakrefobject.c
static void weakref_insert_head(PyWeakReference* newref, PyWeakReference** list)
{
    PyWeakReference* next = *list;

    newref->wr_prev = NULL;
    newref->wr_next = next;
    if (next != NULL)
        next->wr_prev = newref;
    *list = newref;
}

static void weakref_remove(PyWeakReference* self, PyWeakReference** list)
{
    if (*list == self) {
        *list = self->wr_next;
    }
    if (self->wr_prev != NULL) {
        self->wr_prev->wr_next = self->wr_next;
    }
    if (self->wr_next != NULL) {
        self->wr_next->wr_prev = self->wr_prev;
    }
    self->wr_prev = NULL;
    self->wr_next = NULL;
}

static void weakref_decref_weakrefs(PyWeakReference* head)
{
    while (head != NULL) {
        PyWeakReference* weakref = head;
        head = weakref->wr_next;
        weakref->wr_next = NULL;
        weakref->wr_prev = NULL;
        Py_DECREF(weakref);
    }
}

typedef struct {
    int32_t interpreters_remaining;
    PyObject* to_dealloc;
} callback_progress;

typedef struct {
    PyWeakReference* head;
    callback_progress* progress;
} pending_callbacks;

/* Signal that the current interpreter handled the callbacks.
 * If all interpreters have handled the callbacks, deallocate the object.
 */
static void weakref_signal_handled(callback_progress* progress)
{
    int32_t old = _Py_atomic_add_int32(
        &progress->interpreters_remaining, -1);
    if (old == 1) {
        // All callbacks handled, trigger deallocation again.
        Py_INCREF(progress->to_dealloc);
        Py_DECREF(progress->to_dealloc);
        PyMem_Free(progress);
    }
}

/* Call the pending callbacks.
 * This function can be executed asynchronously using Py_AddPendingCall.
 */
static int weakref_call_callbacks(void* arg)
{
    pending_callbacks* pending = (pending_callbacks*)arg;
    PyWeakReference* head = pending->head;
    debug("Interpreter %zd handling callbacks for dying object %p\n",
        PyInterpreterState_GetID(PyInterpreterState_Get()),
        pending->progress->to_dealloc);

    while (head != NULL) {
        PyWeakReference* weakref = head;
        PyObject* callback = weakref->wr_callback;
        assert(callback != NULL);
        weakref->wr_callback = NULL;
        weakref_handle_callback(weakref, callback);
        Py_DECREF(callback);
        head = weakref->wr_next;
        weakref->wr_next = NULL;
        weakref->wr_prev = NULL;
        Py_DECREF(weakref);
    }

    weakref_signal_handled(pending->progress);
    PyMem_Free(pending);
    // Report success as per Py_AddPendingCall contract
    return 0;
}

/* Schedule the callbacks on the given interpreter. */
static void weakref_schedule_callbacks(int64_t ipid, pending_callbacks* pending)
{
    // FIXME(Immutable): Can the interpreter go away in the middle of scheduling?
    PyInterpreterState* target_is = _PyInterpreterState_LookUpID(ipid);
    if (target_is == NULL) {
        // Interpreter is already gone.
        goto abort;
    }
    // We just need to get any thread state to schedule the call.
    PyThreadState* tstate_target = PyInterpreterState_ThreadHead(target_is);
    if (tstate_target == NULL) {
        goto abort;
    }
    PyThreadState* tstate_old = PyThreadState_Swap(tstate_target);
    int schedule_res = Py_AddPendingCall(weakref_call_callbacks, (void*)pending);
    PyThreadState_Swap(tstate_old);
    if (schedule_res != 0) {
        goto abort;
    }
    return;

abort:
    PyMem_Free(pending);
    weakref_decref_weakrefs(pending->head);
    return;
}

/* Remove callbacks with the given ipid from the list.
 * Return them as a new list.
 */
static PyWeakReference* weakref_separate_ipid(PyWeakReference** list, int64_t ipid)
{
    PyWeakReference* result = NULL;
    PyWeakReference* next = *list;
    while (next != NULL) {
        PyWeakReference* current = next;
        next = next->wr_next;
        if (current->callback_ipid == ipid) {
            weakref_remove(current, list);
            weakref_insert_head(current, &result);
        }
    }
    return result;
}

/* Distribute the callbacks to their original interpreters.
 * Returns:
 * (true) The caller can proceed with deallocating 'to_dealloc'.
 * (false) Callbacks were scheduled, deallocation will be triggered again.
 */
static int weakref_distribute_callbacks(PyWeakReference* head, PyObject* to_dealloc)
{
    if (head == NULL) {
        return true;
    }

    debug("Clearing weakrefs of %p.\n", to_dealloc);
    // We want to continue with deallocation after calling all the callbacks.
    callback_progress* progress = PyMem_Malloc(sizeof(callback_progress));
    if (progress == NULL) {
        // Give up calling callbacks.
        weakref_decref_weakrefs(head);
        return true;
    }
    // Start with 1, decremented at the end of this function.
    // This way, we prevent hitting zero before all callbacks are scheduled.
    progress->interpreters_remaining = 1;
    progress->to_dealloc = to_dealloc;

    // Schedule the callbacks on their original interpreters.
    while (head != NULL) {
        int64_t ipid = head->callback_ipid;
        PyWeakReference* ip_callbacks = weakref_separate_ipid(&head, ipid);
        // Create a data structure to hold arguments for the async call.
        pending_callbacks* pending = PyMem_Malloc(sizeof(pending_callbacks));
        if (pending == NULL) {
            // Give up calling callbacks.
            weakref_decref_weakrefs(ip_callbacks);
            continue;
        }

        _Py_atomic_add_int32(&progress->interpreters_remaining, 1);
        pending->head = ip_callbacks;
        pending->progress = progress;
        if (PyInterpreterState_GetID(PyInterpreterState_Get()) == ipid) {
            // We can run the callback here.
            weakref_call_callbacks((void*)pending);
        }
        else {
            // We need to schedule the callback on the target interpreter.
            weakref_schedule_callbacks(ipid, pending);
        }
    }

    weakref_signal_handled(progress);
    return false;
}

/* Clear weakrefs with callbacks for a single object, and call them.
 * Returns:
 * (true) Deallocation can continue.
 * (false) Callbacks were scheduled, deallocation will be triggered again.
 */
static int weakref_handle_callbacks_single(PyObject* obj)
{
    if (!_PyType_SUPPORTS_WEAKREFS(Py_TYPE(obj))) {
        return true;
    }
    // Collect weakrefs with callbacks into a list.
    PyWeakReference* head = NULL;
    _PyImmutability_ClearWeakRefsWithCallback(obj, &head);
    return weakref_distribute_callbacks(head, obj);
}

static int freeze_visit(PyObject *obj, void *freeze_state_untyped)
{
    shallow_freeze_state_t *freeze_state = (shallow_freeze_state_t *)freeze_state_untyped;
    if (obj == NULL) {
        return 0;
    }

    if (_Py_IsDeepImmutable(obj)) {
        return 0;
    }

    TRACE_MERMAID_EDGE(freeze_state->start, obj);

    if(push(freeze_state->pending, obj)){
        PyErr_NoMemory();
        return -1;
    }

    return 0;
}

static int check_freezable(
    struct _Py_immutability_state *state,
    PyObject* obj,
    shallow_freeze_state_t *freeze_state
) {
    debug_obj("check_freezable  %s (%p)\n", obj);

    // Check per-object freezable status set via set_freezable().
    int obj_status = _PyImmutability_GetFreezable(obj);
    if (obj_status >= 0) {
        switch (obj_status) {
        case _Py_FREEZABLE_YES:
            return 0;
        case _Py_FREEZABLE_NO:
            goto error;
        case _Py_FREEZABLE_EXPLICIT:
            if (freeze_state != NULL && is_root(freeze_state, obj)) {
                return 0;
            }
            goto error;
        case _Py_FREEZABLE_PROXY:
            assert(PyModule_Check(obj) || obj == _PyObject_CAST(&PyModule_Type));
            return 0;
        }
    }

    // TODO(Immutable): Visit what the right balance of making Python types immutable is.
    if(!_PyType_HasExtensionSlots(obj->ob_type)){
        return 0;
    }

error:
    debug_obj("Not freezable  %s (%p)\n", obj);
    // FIXME(immutable): If obj is a type, we should print the type name not the super type
    PyObject* error_msg = PyUnicode_FromFormat(
        "Cannot freeze instance of type %s",
        (obj->ob_type->tp_name));
    PyErr_SetObject(PyExc_TypeError, error_msg);
    Py_DECREF(error_msg);
    return -1;
}


int _PyImmutability_SetFreezable(PyObject *obj, _Py_freezable_status status)
{
    if (status < _Py_FREEZABLE_YES || status > _Py_FREEZABLE_PROXY) {
        PyErr_Format(PyExc_ValueError,
                     "Invalid freezable status: %d", status);
        return -1;
    }

    if (status == _Py_FREEZABLE_PROXY
        && !(PyModule_Check(obj) || obj == _PyObject_CAST(&PyModule_Type))
    ) {
        PyErr_SetString(PyExc_TypeError,
                        "FREEZABLE_PROXY can only be set on module objects");
        return -1;
    }

    // Try setting __freezable__ attribute on the object.
    PyObject *value = PyLong_FromLong(status);
    if (value == NULL) {
        return -1;
    }

    int rc = PyObject_SetAttr(obj, &_Py_ID(__freezable__), value);
    Py_DECREF(value);
    if (rc == 0) {
        return 0;
    }

    // If setting the attribute failed, only fall back to ob_flags for
    // "attribute not supported / read-only" cases. Propagate all other
    // exceptions to the caller.
    if (PyErr_ExceptionMatches(PyExc_AttributeError) ||
        PyErr_ExceptionMatches(PyExc_TypeError))
    {
        PyErr_Clear();
    }
    else {
        // Preserve the original error (e.g. MemoryError or a custom
        // tp_setattro exception).
        return -1;
    }

    // If the object doesn't support attribute setting, fall back to ob_flags.
    uint16_t flags = obj->ob_flags;
    flags &= ~(_Py_FREEZABLE_SET_FLAG | _Py_FREEZABLE_STATUS_MASK);
    flags |= _Py_FREEZABLE_SET_FLAG |
             ((status << _Py_FREEZABLE_STATUS_SHIFT) & _Py_FREEZABLE_STATUS_MASK);
    obj->ob_flags = flags;
    return 0;
}


int _PyImmutability_UnsetFreezable(PyObject *obj)
{
    // Try deleting the __freezable__ attribute.
    int rc = PyObject_SetAttr(obj, &_Py_ID(__freezable__), NULL);
    if (rc == 0) {
        goto clear_flags;
    }

    // If deletion failed with AttributeError/TypeError, the object
    // doesn't support attributes — fall through to ob_flags.
    if (PyErr_ExceptionMatches(PyExc_AttributeError) ||
        PyErr_ExceptionMatches(PyExc_TypeError))
    {
        PyErr_Clear();
    }
    else {
        return -1;
    }

clear_flags:
    {
        uint16_t flags = obj->ob_flags;
        flags &= ~(_Py_FREEZABLE_SET_FLAG | _Py_FREEZABLE_STATUS_MASK);
        obj->ob_flags = flags;
    }
    return 0;
}


// Read the freezable status from ob_flags.
// Returns the status if set, or -1 if not set.
static inline int
_get_freezable_from_flags(PyObject *obj)
{
    uint16_t flags = obj->ob_flags;
    if (flags & _Py_FREEZABLE_SET_FLAG) {
        return (flags & _Py_FREEZABLE_STATUS_MASK) >> _Py_FREEZABLE_STATUS_SHIFT;
    }
    return -1;
}

int _PyImmutability_GetFreezable(PyObject *obj)
{
    // First, check for a __freezable__ attribute on the object.
    PyObject *attr = NULL;
    int found = PyObject_GetOptionalAttr(obj, &_Py_ID(__freezable__), &attr);
    if (found == 1) {
        int status = (int)PyLong_AsLong(attr);
        Py_DECREF(attr);
        if (status == -1 && PyErr_Occurred()) {
            return -2;
        }
        return status;
    }
    if (found == -1) {
        return -2;
    }

    // Check ob_flags for the object.
    int flags_status = _get_freezable_from_flags(obj);
    if (flags_status >= 0) {
        return flags_status;
    }

    // Not found for the object itself — check the object's type.
    PyObject *type_obj = (PyObject *)Py_TYPE(obj);
    PyObject *type_attr = NULL;
    int type_found = PyObject_GetOptionalAttr(type_obj,
                                              &_Py_ID(__freezable__),
                                              &type_attr);
    if (type_found == 1) {
        int status = (int)PyLong_AsLong(type_attr);
        Py_DECREF(type_attr);
        if (status == -1 && PyErr_Occurred()) {
            return -2;
        }
        return status;
    }
    if (type_found == -1) {
        return -2;
    }

    // Check ob_flags for the type.
    flags_status = _get_freezable_from_flags(type_obj);
    if (flags_status >= 0) {
        return flags_status;
    }

    return -1;  // Not found.
}


static int
_mark_deep_immutable_cb(_Py_hashtable_t *ht, const void *key, const void *value, void *user_data)
{
    PyObject *item = (PyObject *)key;
    _Py_SetDeepImmutable(item);
    return 0;
}

static int mark_deep_immutable(_Py_hashtable_t *visited_set) {
    return _Py_hashtable_foreach(visited_set, _mark_deep_immutable_cb, NULL);
}

static int
is_immutable_by_construction_type(struct _Py_immutability_state *state, PyTypeObject *tp)
{
    return _Py_hashtable_get(state->immutable_by_construction_types, (void *)tp) != NULL;
}

int _PyImmutability_RegisterImmutableByConstruction(PyTypeObject* tp)
{
    struct _Py_immutability_state *state = get_immutable_state();
    if (state == NULL) {
        return -1;
    }

    // Idempotent — already registered is fine.
    if (is_immutable_by_construction_type(state, tp)) {
        return 0;
    }

    if (_Py_hashtable_set(state->immutable_by_construction_types,
                          (void *)tp, (void *)1) < 0) {
        PyErr_NoMemory();
        return -1;
    }

    // Mark the type also as freezable
    if (_PyImmutability_SetFreezable((PyObject*)tp, _Py_FREEZABLE_YES)) {
        return -1;
    }
    return 0;
}

// Check if a specific object is immutable by construction.
// (a) Its type is registered as immutable by construction
//     (e.g. tuple instances, float instances), OR
// (b) It is itself a type object with Py_TPFLAGS_IMMUTABLETYPE set
//     (e.g. the float type object — but not a mutable heap type).
static int
is_immutable_by_construction(struct _Py_immutability_state *state, PyObject *obj)
{
    if (is_immutable_by_construction_type(state, Py_TYPE(obj))) {
        return 1;
    }
    if (PyType_Check(obj)) {
        PyTypeObject *tp = (PyTypeObject *)obj;
        if (tp->tp_flags & Py_TPFLAGS_IMMUTABLETYPE) {
            return 1;
        }
    }
    return 0;
}

typedef struct {
    // Here it's safe to use a hashtable without incrementing the refcount
    // since we have a owning reference to the root and all objects in this
    // table are immutable.
    _Py_hashtable_t *visited;
    struct _Py_immutability_state *imm_state;
    // PyList object of pending objects
    PyObject *pending;
} implicit_freeze_state_t;

static void dealloc_implicit_freeze_state(implicit_freeze_state_t *state) {
    if (state->visited) {
        _Py_hashtable_destroy(state->visited);
        state->visited = NULL;
    }
    Py_CLEAR(state->pending);
}

static int init_implicit_freeze_state(implicit_freeze_state_t *state) {
    state->visited = NULL;
    state->pending = NULL;

    state->visited = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if (state->visited == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    state->pending = PyList_New(0);
    if (state->pending == NULL) {
        goto error;
    }

    return 0;
error:
    dealloc_implicit_freeze_state(state);
    return -1;
}

// Visitor callback that adds objects to the worklist for iterative processing.
// Returns 0 if the object can be viewed as immutable and was added to the
// worklist, 1 if a mutable object was found, -1 on error.
static int
can_view_as_immutable_visit(PyObject *obj, void *arg)
{
    implicit_freeze_state_t *state = (implicit_freeze_state_t *)arg;
    if (obj == NULL) {
        return 0;
    }

    // Already frozen — skip.
    if (_Py_IsDeepImmutable(obj)) {
        return 0;
    }

    // Already visited — skip.
    if (_Py_hashtable_get(state->visited, obj) != NULL) {
        return 0;
    }

    // Check if the object can be viewed as immutable.
    if (!is_immutable_by_construction(state->imm_state, obj)) {
        // Found a mutable object — graph cannot be viewed as immutable.
        return 1;
    }

    // Mark visited.
    if (_Py_hashtable_set(state->visited, obj, (void *)1) < 0) {
        PyErr_NoMemory();
        return -1;
    }

    // Add to worklist for traversal of referents.
    if (push(state->pending, obj) < 0) {
        return -1;
    }

    return 0;
}

int _PyImmutability_CanViewAsImmutable(PyObject *obj)
{
    // Check if the object graph rooted at obj can be viewed as immutable.
    // An object graph can be viewed as immutable if every reachable object
    // is either already frozen, or is shallow immutable (its own state
    // cannot be mutated, though it may reference other objects).
    //
    // If the graph can be viewed as immutable, it is frozen (to set up
    // proper refcount management) and 1 is returned.
    // Returns 0 if the graph cannot be viewed as immutable, -1 on error.

    // Already frozen — trivially yes.
    if (_Py_IsDeepImmutable(obj)) {
        return 1;
    }

    struct _Py_immutability_state *imm_state = get_immutable_state();
    if (imm_state == NULL) {
        return -1;
    }

    // The root must itself be immutable by construction to be viewed as immutable.
    if (!is_immutable_by_construction(imm_state, obj)) {
        return 0;
    }

    int result = 0;
    implicit_freeze_state_t state;
    SUCCEEDS(init_implicit_freeze_state(&state));

    // Mark root visited and seed the worklist.
    if (_Py_hashtable_set(state.visited, obj, (void *)1) < 0) {
        PyErr_NoMemory();
        goto error;
    }
    SUCCEEDS(push(state.pending, obj));

    // Iterative DFS: pop from worklist, traverse referents.
    while (PyList_GET_SIZE(state.pending) > 0) {
        PyObject *item = pop(state.pending);

        // Traverse the item
        traverseproc reachable = get_reachable_proc(Py_TYPE(item));
        result = reachable(item, can_view_as_immutable_visit, &state);
        Py_DECREF(item);

        // Stop on error.
        if (result < 0) {
            goto error;
        }

        // Stop if a mutable object was found
        if (result > 0) {
            result = 0;
            goto finally;
        }
    }

    SUCCEEDS(mark_deep_immutable(state.visited));

    result = 1;
    goto finally;
error:
    result = -1;
finally:
    dealloc_implicit_freeze_state(&state);
    return result;
}

// Perform a decref on an immutable object
// returns true if the object should be deallocated.
int _Py_DecRef_Immutable(PyObject *op)
{
    // pass
}

// _Py_RefcntAdd_Immutable(op, 1);
void _Py_RefcntAdd_Immutable(PyObject *op, Py_ssize_t increment)
{
    // pass
}

/* Tries to incref op and returns 1 if successful or 0 otherwise.
 * Used when creating a strong reference from a weak reference.
 * Needs to hold the weakref list lock (LOCK_WEAKREFS).
 */
int _Py_TryIncref_Immutable(PyObject *op)
{
    // pass
}

/* Returns 1 if there are no references to the object's SCC. */
int _Py_IsDead_Immutable(PyObject *op)
{
    // pass
}

static int _run_pre_freeze_hook(struct _Py_immutability_state *imm_state, PyObject* obj) {
    // 1. Check for the `__pre_freeze__` name
    PyObject *attr = NULL;
    int res = PyObject_GetOptionalAttr(obj, &_Py_ID(__pre_freeze__), &attr);
    if (res == -1) {
        return -1;
    } else if (res == 1) {
        if (!PyCallable_Check(attr)) {
            PyErr_Format(
                PyExc_TypeError,
                "'%.200s.__pre_freeze__' is not callable",
                Py_TYPE(obj)->tp_name);
            Py_DECREF(attr);
            return -1;
        }
        PyObject *result = PyObject_CallNoArgs(attr);
        Py_DECREF(attr);
        if (result == NULL) {
            return -1;
        }
        Py_DECREF(result);
    }

    // 2. Check the type for `tp_prefreeze`
    prefreezeproc prefreeze = Py_TYPE(obj)->tp_prefreeze;
    if (prefreeze != NULL) {
        return prefreeze(obj);
    }

    // No pre-freeze hook, so we're good to go.
    return 0;
}

static int check_pre_freeze_hook(struct _Py_immutability_state *imm_state, PyObject* obj) {
    // Skip Python-level hook lookup for type objects. For classes,
    // `__pre_freeze__` resolves to an unbound function and calling it as a
    // normal bound method would fail with a missing 'self' argument.
    if (PyType_Check(obj)) {
        return 0;
    }

    // Pre-freeze hooks are never called for shallow immutable objects
    if (is_immutable_by_construction(imm_state, obj)) {
        return 0;
    }

    // Check if the pre-freeze hook already ran for this object
    if ((obj->ob_flags & _Py_PREFREEZE_RAN_FLAG) != 0) {
        return 0;
    }

    // Mark pre-freeze hook as completed. This has to be set before calling
    // the pre-freeze hook in case the pre-freeze hook reenters to prevent
    // an infinite loop.
    obj->ob_flags |= _Py_PREFREEZE_RAN_FLAG;

    // Run the pre-freeze hook if it's present.
    return _run_pre_freeze_hook(imm_state, obj);
}

static int traverse_freeze(PyObject *obj, shallow_freeze_state_t *freeze_state)
{
    int result = 0;

#ifdef MERMAID_TRACING
    freeze_state->start = obj;
    TRACE_MERMAID_NODE(obj);
#endif

    debug_obj("Traversing %s (%p) rc=%zd\n", obj, Py_REFCNT(obj));

    if (is_c_wrapper(obj)) {
        return 1;
    }

    Py_BEGIN_CRITICAL_SECTION(obj);
    traverseproc reachable = get_reachable_proc(Py_TYPE(obj));
    SUCCEEDS(reachable(obj, (visitproc)freeze_visit, freeze_state));
    Py_END_CRITICAL_SECTION();

    // Weak references are not followed by the GC, but should be
    // for immutability.  Otherwise, we could share mutable state
    // using a weak reference.
    if (PyWeakref_Check(obj)) {
        PyObject* wr;
        int res = PyWeakref_GetRef(obj, &wr);
        if (res == -1) {
            goto error;
        }
        if (res == 1) {
            if (freeze_visit(wr, freeze_state)) {
                // freeze_visit() passes wr to push(), which consumes the
                // reference even when appending to the DFS stack fails.
                goto error;
            }
        }
        Py_DECREF(wr);
    }

    goto finally;
error:
    result = -1;
finally:
    return result;
}

// Mark importlib's mutable state as not freezable.
// Separated from init_state because _frozen_importlib is not
// available during early interpreter startup.
static void
late_init(struct _Py_immutability_state *state)
{
    state->late_init_done = true;

    PyObject *frozen_importlib = PyImport_ImportModule("_frozen_importlib");
    if (frozen_importlib == NULL) {
        PyErr_Clear();
        return;
    }

    PyObject *module_locks = PyObject_GetAttrString(frozen_importlib,
                                                    "_module_locks");
    if (module_locks != NULL) {
        if (_PyImmutability_SetFreezable(module_locks,
                                         _Py_FREEZABLE_NO) < 0) {
            PyErr_Clear();
        }
        Py_DECREF(module_locks);
    } else {
        PyErr_Clear();
    }

    PyObject *blocking_on = PyObject_GetAttrString(frozen_importlib,
                                                   "_blocking_on");
    if (blocking_on != NULL) {
        if (_PyImmutability_SetFreezable(blocking_on,
                                         _Py_FREEZABLE_NO) < 0) {
            PyErr_Clear();
        }
        Py_DECREF(blocking_on);
    } else {
        PyErr_Clear();
    }

    Py_DECREF(frozen_importlib);

#ifdef Py_DEBUG
    PyObject *traceback_module = PyImport_ImportModule("traceback");
    if (traceback_module != NULL) {
        state->traceback_func = PyObject_GetAttrString(traceback_module,
                                                       "format_stack");
        Py_DECREF(traceback_module);
    } else {
        PyErr_Clear();
    }
#endif
}

static int
freeze_impl(PyObject *const *objs, Py_ssize_t nobjs)
{
    struct _Py_immutability_state* imm_state = NULL;
    imm_state = get_immutable_state();
    if (imm_state == NULL) {
        return -1;
    }

    int result = 0;
    PyObject *item;
    TRACE_MERMAID_START();
    
    // Initialize the freeze state
    shallow_freeze_state_t state;
    SUCCEEDS(init_shallow_freeze_state(&state));

    // Register all roots and push onto the DFS stack
    for (Py_ssize_t i = 0; i < nobjs; i++) {
        if (_Py_IsShallowImmutable(objs[i])) {
            continue;
        }
        // FIXME(immutable): It is not quite clear how `Explicit` should work
        // for nested freeze calls. One could argue that they should be frozen
        // if they're the root of at least one freeze call. Even if this is an
        // enclosing `freeze` call. For now we only allow `freeze` to explicitly
        // freeze root objects of its own freeze call and ignore enclosing ones.
        if (_Py_hashtable_set(state.roots, objs[i], objs[i]) < 0) {
            PyErr_NoMemory();
            goto error;
        }
        SUCCEEDS(push(state.pending, objs[i]));
    }

    // Late-init: mark importlib mutable state as not freezable.
    if (!imm_state->late_init_done) {
        late_init(imm_state);
    }

#ifdef Py_DEBUG
    // In debug mode, we can set a freeze location for debugging purposes.
    // Get a traceback object to use as the freeze location.
    if (imm_state->traceback_func != NULL) {
        PyObject *stack = PyObject_CallFunctionObjArgs(imm_state->traceback_func, NULL);
        if (stack != NULL) {
            // Add the type name to the top of the stack, can be useful.
            PyObject* typename = PyObject_GetAttrString(_PyObject_CAST(Py_TYPE(objs[0])), "__name__");
            push(stack, typename);
            state.freeze_location = stack;
        }
    }
#endif

    // Walk the tree and mark all as shallow immutable
    while (PyList_Size(state.pending) != 0) {
        PyObject* item = pop(state.pending);

        // This object and all reachable ones are deeply immutable, ignore them
        if (_Py_IsDeepImmutable(item)) {
            Py_CLEAR(item);
            continue;
        }

        if (_Py_hashtable_get(state.visited, (void*)item)) {
            debug_obj("Already visited: %s (%p)\n", item);
            Py_CLEAR(item);
            continue;
        }

        // New object, check if freezable
        SUCCEEDS(check_freezable(imm_state, item, &state));

        // Call the pre-freeze hook if one is present
        SUCCEEDS(check_pre_freeze_hook(imm_state, item));

        // If the pre-freeze hook turned the object immutable, we want to skip it.
        if (_Py_IsDeepImmutable(item)) {
            Py_CLEAR(item);
            continue;
        }

        // Mark the object
        if (_Py_hashtable_set(state.visited, (void*)item, (void*)1)) {
            PyErr_NoMemory();
            goto error;
        }
        _Py_SetShallowImmutable(item);

        // Traverse the object
        SUCCEEDS(traverse_freeze(item, &state));

        Py_CLEAR(item);
    }

    SUCCEEDS(mark_deep_immutable(state.visited));

    goto finally;
error:
    debug("Error during freeze\n");
    result = -1;
finally:
    Py_CLEAR(item);
    dealloc_shallow_freeze_state(&state);
    TRACE_MERMAID_END();
    return result;
}

// Main entry point to freeze an object and everything it can reach.
int _PyImmutability_Freeze(PyObject* obj)
{
    if(_Py_IsDeepImmutable(obj)){
        return 0;
    }
    return freeze_impl(&obj, 1);
}

// Freeze multiple root objects and their reachable graphs together.
// All provided objects are treated as roots for EXPLICIT freezable checks.
int _PyImmutability_FreezeMany(PyObject *const *objs, Py_ssize_t nobjs)
{
    return freeze_impl(objs, nobjs);
}
