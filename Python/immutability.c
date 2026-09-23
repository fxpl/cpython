
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


// Macro that jumps to error, if the expression `x` does not succeed.
#define SUCCEEDS(x) { do { int r = (x); if (r != 0) goto error; } while (0); }

#define SCC_RANK_FLAG _PyGC_PREV_MASK_COLLECTING

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


static int push_weak(PyObject* s, PyObject* item){
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
    return push_weak(s, _Py_NewRef(item));
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

// Returns a borrowed reference to the last item in the list.
static PyObject* peek(PyObject* s){
    PyObject* item;
    Py_ssize_t size = PyList_Size(s);
    if (size == 0) {
        return NULL;
    }

    item = PyList_GetItem(s, size - 1);
    if (item == NULL) {
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

    // Whether the frozen graph should use atomic reference counting.
    // TODO(immutability): not honoured yet, only plumbed through.
    int atomic;

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

#ifdef Py_DEBUG
    Py_CLEAR(state->freeze_location);
#endif

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
    state->atomic = 0;
#ifdef Py_DEBUG
    state->freeze_location = NULL;
#endif

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

// Forwards to the real visitproc, noting whether tp_traverse visited the type.
typedef struct {
    visitproc visit;
    void *state;
    PyObject *type;
    int type_visited;
} type_visit_tracker_t;

static int
track_type_visit(PyObject *op, void *tracker_untyped)
{
    type_visit_tracker_t *tracker = (type_visit_tracker_t *)tracker_untyped;
    if (op == tracker->type) {
        tracker->type_visited = 1;
    }
    return tracker->visit(op, tracker->state);
}

// Wrapper around tp_traverse that also visits the type object.
// tp_traverse does not visit the type for non-heap types, but
// tp_reachable should visit all reachable objects including the type.
static int
traverse_via_tp_traverse(PyObject *obj, visitproc visit, void *state)
{
    PyTypeObject *tp = Py_TYPE(obj);
    type_visit_tracker_t tracker = {visit, state, _PyObject_CAST(tp), 0};

    traverseproc traverse = tp->tp_traverse;
    if (traverse != NULL) {
        int err = traverse(obj, track_type_visit, &tracker);
        if (err) {
            return err;
        }
    }

    // `tp_traverse` of heap types *should* include a `Py_VISIT(Py_TYPE(self))`
    // since around Python 2.7, but plenty of types still don't. As part of the
    // visit we also track if the type was found and manually visit it, if not.
    //
    // This must go through `visit` rather than touching a worklist directly:
    // every caller of get_reachable_proc() passes its own state type, and only
    // its own visitproc knows how to interpret it.
    if (!tracker.type_visited) {
        return visit(_PyObject_CAST(tp), state);
    }
    return 0;
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
        _Py_OB_FLAG_ADD(op, _Py_IMMUTABLE_FLAG);
    }
}
static inline void _Py_SetDeepImmutable(PyObject *op)
{
    _Py_OB_FLAG_ADD(op, _Py_IMMUTABLE_DEPTH_FLAG);
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

#pragma region Freezability

static int check_freezable(
    struct _Py_immutability_state *state,
    PyObject* obj,
    shallow_freeze_state_t *freeze_state
) {
    debug_obj("check_freezable  %s (%p)\n", obj);

    // Check per-object freezable status set via set_freezable().
    int obj_status = _PyImmutability_GetFreezable(obj);
    // -2 means the lookup itself failed and has set an exception, which must
    // not be swallowed by the fallbacks below. -1 only means "not found".
    if (obj_status == -2) {
        return -1;
    }
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
    uint16_t old_flags = _Py_atomic_load_uint16(&obj->ob_flags);
    while (true) {
        uint16_t new_flags = (old_flags & ~(_Py_FREEZABLE_SET_FLAG | _Py_FREEZABLE_STATUS_MASK));
        new_flags |= _Py_FREEZABLE_SET_FLAG |
                 ((status << _Py_FREEZABLE_STATUS_SHIFT) & _Py_FREEZABLE_STATUS_MASK);
        if (_Py_atomic_compare_exchange_uint16(&obj->ob_flags, &old_flags, new_flags)) {
            break;
        }
    };
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
    _Py_OB_FLAG_REMOVE(obj, _Py_FREEZABLE_SET_FLAG | _Py_FREEZABLE_STATUS_MASK);
    return 0;
}


// Read the freezable status from ob_flags.
// Returns the status if set, or -1 if not set.
static inline int
_get_freezable_from_flags(PyObject *obj)
{
    uint16_t flags = _Py_OB_FLAGS_LOAD(obj);
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

#pragma endregion Freezability



#pragma region Weakref Handling
#ifdef _Py_PYRONA_INTERPRETER_SHARING

static int make_weakrefs_interpreter_safe_visit(
    _Py_hashtable_t *tbl, const void *key, const void *value, void *unused)
{
    (void)tbl;
    (void)value;
    (void)unused;
    _PyWeakref_OnObjectFreeze((PyObject*)key);
    return 0;
}

static void make_weakrefs_interpreter_safe(_Py_hashtable_t *immutable) {
    _Py_hashtable_foreach(immutable, make_weakrefs_interpreter_safe_visit, NULL);
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
    {
        callback_progress *progress = pending->progress;
        weakref_decref_weakrefs(pending->head);
        PyMem_Free(pending);
        // Give back the token taken by weakref_distribute_callbacks, or the
        // object's deallocation is never re-triggered.
        weakref_signal_handled(progress);
        return;
    }
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

static PyObject* get_scc_next(PyObject* obj);

/* Clear weakrefs with callbacks for an SCC, and call them.
 * Returns:
 * (true) Deallocation can continue.
 * (false) Callbacks were scheduled, deallocation will be triggered again.
 */
static int weakref_handle_callbacks_scc(PyObject* obj)
{
    // Collect weakrefs with callbacks into a list.
    PyWeakReference* head = NULL;
    PyObject* n = obj;
    do {
        PyObject* c = n;
        n = get_scc_next(c);
        if (_PyType_SUPPORTS_WEAKREFS(Py_TYPE(c))) {
            _PyImmutability_ClearWeakRefsWithCallback(c, &head);
        }
    } while (n != obj);

    return weakref_distribute_callbacks(head, obj);
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

#endif // _Py_PYRONA_INTERPRETER_SHARING
#pragma endregion // Weakref Handling



#pragma region SCCs & Interpreters
#ifdef _Py_PYRONA_INTERPRETER_SHARING

static void scc_set_representative(PyObject* obj, PyObject* parent)
{
    assert(_PyObject_IS_GC(obj));

    // Use GC space for the parent pointer.
    PyGC_Head* gc = _Py_AS_GC(obj);
    assert(((uintptr_t)parent & ~_PyGC_PREV_MASK) == 0);
    uintptr_t finalized_bit = gc->_gc_prev & _PyGC_PREV_MASK_FINALIZED;
    gc->_gc_prev = finalized_bit | _Py_CAST(uintptr_t, parent);
}

static PyObject* scc_get_representative(PyObject* obj)
{
    assert((_Py_AS_GC(obj)->_gc_prev & SCC_RANK_FLAG) == 0);
    // Use GC space for the parent pointer.
    return _Py_CAST(PyObject*, _Py_AS_GC(obj)->_gc_prev & _PyGC_PREV_MASK);
}

static int scc_is_root(PyObject* obj) {
    return (_Py_AS_GC(obj)->_gc_prev & SCC_RANK_FLAG) != 0;
}

static void scc_set_rank(PyObject* obj, size_t rank)
{
    // Use GC space for the rank.
    PyGC_Head* gc = _Py_AS_GC(obj);
    uintptr_t finalized_bit = gc->_gc_prev & _PyGC_PREV_MASK_FINALIZED;
    gc->_gc_prev = finalized_bit | (rank << _PyGC_PREV_SHIFT) | SCC_RANK_FLAG;
}

static size_t scc_get_rank(PyObject* obj)
{
    assert((_Py_AS_GC(obj)->_gc_prev & SCC_RANK_FLAG) == SCC_RANK_FLAG);
    // Use GC space for the rank.
    return _Py_AS_GC(obj)->_gc_prev >> _PyGC_PREV_SHIFT;
}

static void set_scc_next(PyObject* obj, PyObject* next)
{
    debug("   set_scc_next %p -> %p\n", obj, next);
    // Use GC space for the next pointer.
    _Py_AS_GC(obj)->_gc_next = (uintptr_t)next;
}

static PyObject* get_scc_next(PyObject* obj)
{
    // Use GC space for the next pointer.
    return _Py_CAST(PyObject*, _Py_AS_GC(obj)->_gc_next);
}

static void scc_init_non_trivial(PyObject* obj)
{
    // Check if this not been part of an SCC yet.
    if (get_scc_next(obj) == NULL) {
        // Set up a new SCC with a single element.
        scc_set_rank(obj, 0);
        set_scc_next(obj, obj);
    }

    // Mark this object as being part of an SCC
    _Py_OB_FLAG_ADD(obj, _Py_IMMUTABLE_SCC_FLAG);
}

static void scc_return_to_gc(PyObject* op)
{
    _Py_OB_FLAG_REMOVE(op, _Py_IMMUTABLE_SCC_FLAG);
    set_scc_next(op, NULL);
    scc_set_representative(op, NULL);
    _PyObject_GC_TRACK(op);
}

static void scc_init(PyObject* obj)
{
    assert(_PyObject_IS_GC(obj));
    assert(_PyObject_GC_IS_TRACKED(obj));

    // Let the Immutable GC take over tracking the lifetime
    // of this object. This releases the space for the SCC
    // algorithm.
    _PyObject_GC_UNTRACK(obj);

    // The GC uses the collecting flag to identify objects part of the
    // current collection set. This flag remains while the finalizer
    // of unreachable objects is being called.
    //
    // If something calls `freeze(obj)` as part of their finalizer we
    // might receive an object with the flag set. This removes the flag
    // to prevent future GC collections to assume this object is currently
    // being collected.
    _PyGC_CLEAR_COLLECTING(obj);

    scc_set_rank(obj, 0);
}

static PyObject* scc_get_root(PyObject* obj)
{
    if (scc_is_root(obj)) {
        return obj;
    }
    // Grandparent path compression for union find.
    PyObject* grandparent = obj;
    PyObject* rep = scc_get_representative(obj);
    while (!scc_is_root(rep)) {
        PyObject* parent = rep;
        rep = scc_get_representative(rep);
        scc_set_representative(grandparent, rep);
        grandparent = parent;
    }
    return rep;
}

static bool
scc_union(PyObject* a, PyObject* b)
{
    // TODO(immutability): Why is this needed, can't we just yeet it?
    // Initialize SCC information for both objects.
    // If they are already in an SCC, this is a no-op.
    scc_init_non_trivial(a);
    scc_init_non_trivial(b);

    PyObject* rep_a = scc_get_root(a);
    PyObject* rep_b = scc_get_root(b);
    if (rep_a == rep_b)
        return false;

    // Determine rank, and switch so that rep_a has higher rank.
    size_t rank_a = scc_get_rank(rep_a);
    size_t rank_b = scc_get_rank(rep_b);
    if (rank_a < rank_b) {
        PyObject* temp = rep_a;
        rep_a = rep_b;
        rep_b = temp;
    } else if (rank_a == rank_b) {
        // Increase rank of new representative.
        scc_set_rank(rep_a, rank_a + 1);
    }

    scc_set_representative(rep_b, rep_a);

    // Merge the cyclic lists.
    PyObject* next_a = get_scc_next(rep_a);
    PyObject* next_b = get_scc_next(rep_b);
    set_scc_next(rep_a, next_b);
    set_scc_next(rep_b, next_a);
    return true;
}

/**
 * The DFS walk for SCC calculations needs to perform actions on both
 * the pre-order and post-order visits to an object.  To achieve this
 * with a single stack we use a marker object (PostOrderMarker) to
 * indicate that the object being popped is a post-order visit.
 *
 * Effectively we do
 *   obj = pop()
 *   if obj is SccPostOrderMarker:
 *      obj = pop()
 *      post_order_action(obj)
 *   else:
 *      push(obj)
 *      push(SccPostOrderMarker)
 *      pre_order_action(obj)
 *
 * In pre_order_action, the children of obj can be pushed onto the stack,
 * and once all that work is completed, then the SccPostOrderMarker will pop out
 * and the post_order_action can be performed.
 *
 * Using a separate object means it cannot conflict with anything
 * in the actual python object graph.
 */
PyObject SccPostOrderMarkerStruct = _PyObject_HEAD_INIT(&_PyNone_Type);
static PyObject* SccPostOrderMarker = &SccPostOrderMarkerStruct;

#define SCC_VISITED_DONE ((void*)1)
#define SCC_VISITED_PENDING ((void*)2)

typedef struct {
    // Used to track traversal order
    // All references are weak
    PyObject *dfs;
    // Used to track SCC to handle cycles during traversal.
    // All references are weak
    PyObject *pending;
    // During traversal we can't follow weakly referenced objects directly as
    // that could form SCCs and keep objects incorrectly alive. To handle these
    // objects we track them as new roots here.
    // All references are weak
    PyObject *new_roots;
    // All items which have been visited.
    // 1 -> Visited and done
    // 2 -> Visited and pending
    _Py_hashtable_t *visited;
} scc_build_state_t;

static void dealloc_scc_build_state(scc_build_state_t *state) {
    // We can't call the destructor directly since these lists store weakrefs
    if (state->pending != NULL) {
        while(PyList_Size(state->pending) > 0){
            pop(state->pending);
        }
        Py_CLEAR(state->pending);
    }

    if (state->dfs != NULL) {
        while(PyList_Size(state->dfs) > 0){
            pop(state->dfs);
        }
        Py_CLEAR(state->dfs);
    }

    if (state->new_roots != NULL) {
        while(PyList_Size(state->new_roots) > 0){
            pop(state->new_roots);
        }
        Py_CLEAR(state->new_roots);
    }

    if (state->visited != NULL) {
        _Py_hashtable_destroy(state->visited);
        state->visited = NULL;
    }
}

static int init_scc_build_state(scc_build_state_t *state) {
    state->dfs = NULL;
    state->pending = NULL;
    state->visited = NULL;

    state->dfs = PyList_New(0);
    if (state->dfs == NULL) {
        goto error;
    }

    state->pending = PyList_New(0);
    if (state->pending == NULL) {
        goto error;
    }

    state->new_roots = PyList_New(0);
    if (state->new_roots == NULL) {
        goto error;
    }

    state->visited = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if (state->visited == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    return 0;
error:
    dealloc_scc_build_state(state);
    return -1;
}

static void scc_complete(PyObject *obj, scc_build_state_t *state) {
    PyObject* c = get_scc_next(obj);
    // Single object SCCs are tagged for normal atomic reference counting
    if (c == NULL) {
        debug_obj("Completing SCC %s (%p) with single member rc = %zd\n", obj, Py_REFCNT(obj));
        _Py_OB_FLAG_ADD(obj, _Py_ATOMIC_RC_FLAG);
        scc_set_representative(obj, obj);
        return;
    }

    size_t rc = Py_REFCNT(obj);
    size_t count = 1;
    while (c != obj)
    {
        debug("Adding %p to SCC %p\n", c, obj);
        rc += Py_REFCNT(c);
        // Mark this object as being RCed as part of an SCC
        _Py_OB_FLAG_ADD(c, (_Py_IMMUTABLE_SCC_FLAG | _Py_ATOMIC_RC_FLAG));
        scc_set_representative(c, obj);
        c = get_scc_next(c);
        count++;
    }
    // We will have left an RC live for each element in the SCC, so
    // we need to remove that from the SCCs refcount.
    obj->ob_refcnt = rc - (count - 1);
    _Py_OB_FLAG_ADD(c, (_Py_IMMUTABLE_SCC_FLAG | _Py_ATOMIC_RC_FLAG));
    scc_set_representative(obj, obj);

    debug_obj("Completed SCC %s (%p) with %zu members with rc %zu \n", obj, count, rc - (count - 1));
}

static void scc_pop_pending(scc_build_state_t *state) {
    PyObject *pending = pop(state->pending);
    _Py_hashtable_entry_t *entry = _Py_hashtable_get_entry(state->visited, (void*)pending);
    assert(entry != NULL);
    assert(entry->value == SCC_VISITED_PENDING);
    entry->value = SCC_VISITED_DONE;
}

static void scc_finish_at_postorder(PyObject *item, scc_build_state_t *state) {
    PyObject* current_scc = peek(state->pending);
    if (item == current_scc)
    {
        debug("Completed an SCC\n");
        scc_pop_pending(state);
        debug_obj("Representative: %s (%p)\n", item);

        scc_complete(item, state);
    }
}

static void scc_add_internal_reference(PyObject* obj)
{
    obj->ob_refcnt--;
    debug_obj("Decrementing rc of %s (%p) to %zd\n", obj, _Py_REFCNT(obj));
    assert(_Py_REFCNT(obj) > 0);
}

static void scc_add_internal_pending_edge(PyObject *obj, scc_build_state_t *state) {
    PyObject *current_scc = peek(state->pending);
    if (current_scc == NULL) {
        Py_FatalError("freeze: pending object without pending SCC");
    }
    while (scc_union(current_scc, obj)) {
        debug_obj("Representative: %s (%p)\n", current_scc);
        scc_pop_pending(state);
        current_scc = peek(state->pending);
        if (current_scc == NULL) {
            Py_FatalError("freeze: SCC union emptied pending stack");
        }
    }
    scc_add_internal_reference(obj);
}

static int scc_build_visit(PyObject *obj, void *state_untyped) {
    scc_build_state_t *state = (scc_build_state_t*)state_untyped;
    if (obj == NULL) {
        return 0;
    }

    // References to deeply immutable objects are trivially accepted
    if (_Py_IsDeepImmutable(obj)
        && (_Py_hashtable_get(state->visited, (void*)obj) != SCC_VISITED_PENDING)
    ) {
        return 0;
    }

    // Queue the object for exploration
    if (push_weak(state->dfs, obj)) {
        return -1;
    }

    return 0;
}

static int scc_build_traverse(PyObject *obj, scc_build_state_t *state) {
    // Ignore C wrappers
    if (is_c_wrapper(obj)) {
        return 0;
    }

    // Traverse the object
    traverseproc reachable = get_reachable_proc(Py_TYPE(obj));
    int result = reachable(obj, (visitproc)scc_build_visit, state);

    // We can't visit these weakly referenced objects directly, as that may
    // build SCCs across weakreferences and keep objects alive when they
    // should die. Instead we enqueue them as new roots to start traversing later.
    if (PyWeakref_Check(obj)) {
        PyObject* wr;
        int res = PyWeakref_GetRef(obj, &wr);
        if (res < 0) {
            return -1;
        }
        // wr is only set when res == 1; a dead referent leaves it NULL.
        if (res == 1) {
            // The object will stay alive due to the GIL
            Py_DECREF(wr);
            if (push_weak(state->new_roots, wr)) {
                return -1;
            }
        }
    }

    return result;
}

static int scc_build(_Py_hashtable_t *visited_set, PyObject *const *roots, int nroots) {
    int result = 0;
    scc_build_state_t state;
    SUCCEEDS(init_scc_build_state(&state));

    // Init the pending stack
    for (Py_ssize_t i = 0; i < nroots; i++) {
        if (!_Py_IsDeepImmutable(roots[i])) {
            SUCCEEDS(push_weak(state.dfs, roots[i]));
        }
    }

    while (PyList_Size(state.dfs) != 0 || PyList_Size(state.new_roots) != 0) {
        PyObject* item = pop(state.dfs);
        if (item == NULL) {
            item = pop(state.new_roots);
        }

        // Complete SCCs
        if (item == SccPostOrderMarker) {
            item = pop(state.dfs);

            // Have finished traversing graph reachable from item
            scc_finish_at_postorder(item, &state);
            continue;
        }

        // Skip object's we've already visited
        void* visited_state = _Py_hashtable_get(state.visited, (void*)item);
        if (visited_state != 0) {
            debug_obj("Already visited: %s (%p)\n", item);
            // Handle pending edges
            if (visited_state == SCC_VISITED_PENDING) {
                scc_add_internal_pending_edge(item, &state);
            }
            continue;
        }

        // Untrack GC objects and enable atomic RC
        if (_PyObject_IS_GC(item) && _PyObject_GC_IS_TRACKED(item)) {
            // Add postorder step to dfs.
            SUCCEEDS(push_weak(state.dfs, item));
            SUCCEEDS(push_weak(state.dfs, SccPostOrderMarker));
            // Add to the SCC path
            SUCCEEDS(push_weak(state.pending, item));
            
            scc_init(item);
            visited_state = SCC_VISITED_PENDING;
        } else {
            _Py_OB_FLAG_ADD(item, _Py_ATOMIC_RC_FLAG);
            visited_state = SCC_VISITED_DONE;
        }

        // Mark the object as visited (and maybe pending)
        if (_Py_hashtable_set(state.visited, (void*)item, visited_state)) {
            PyErr_NoMemory();
            goto error;
        }

        // Traverse the object
        SUCCEEDS(scc_build_traverse(item, &state));
    }

    goto finally;
error:
    result = -1;
finally:
    dealloc_scc_build_state(&state);
    return result;
}

typedef struct {
    int has_weakreferences;
    int has_legacy_finalizers;
    int has_finalizers;
} scc_details_t;

static void scc_set_refcounts_to_one(PyObject* obj)
{
    PyObject* n = obj;
    do {
        PyObject* c = n;
        n = get_scc_next(c);
        c->ob_refcnt = 1;
    } while (n != obj);
}

static int _dissolve_scc_reconstruct_rcs_visit(PyObject *obj, void *scc_rep) {
    if (obj == NULL)
        return 0;

    if ((_Py_OB_FLAGS_LOAD(obj) & _Py_IMMUTABLE_SCC_FLAG) == 0)
        return 0;

    PyObject* rep = scc_get_representative(obj);
    if (rep == scc_rep) {
        // Increase the reference count as we found an interior edge for the SCC.
        debug_obj("Reinstate %s (%p) with rc %zu from %p\n", obj, Py_REFCNT(obj), scc_rep);
        obj->ob_refcnt++;
    }

    return 0;
}

static void scc_reconstruct_rcs(PyObject *obj, scc_details_t *details) {
    assert(_Py_OB_FLAGS_LOAD(obj) & _Py_IMMUTABLE_SCC_FLAG);
    PyObject* scc_rep = scc_get_representative(obj);

    details->has_weakreferences = 0;
    details->has_legacy_finalizers = 0;
    details->has_finalizers = 0;

    // Add back the reference counts for the interior edges.
    PyObject* n = obj;
    do {
        PyObject* c = n;
        n = get_scc_next(c);

        traverseproc traverse = get_reachable_proc(Py_TYPE(c));
        traverse(c, (visitproc)_dissolve_scc_reconstruct_rcs_visit, scc_rep);

        if (Py_TYPE(c)->tp_del != NULL)
            details->has_legacy_finalizers++;
        if (Py_TYPE(c)->tp_finalize != NULL && !_PyGC_FINALIZED(c))
            details->has_finalizers++;
        if (_PyType_SUPPORTS_WEAKREFS(Py_TYPE(c)) &&
            *_PyObject_GET_WEAKREFS_LISTPTR_FROM_OFFSET(c) != NULL) {
            details->has_weakreferences++;
        }
    } while (n != obj);
}

// Must run after scc_reconstruct_rcs, which needs the whole SCC still flagged.
static void scc_make_mutable(PyObject *obj)
{
    PyObject* n = obj;
    do {
        debug_obj("Unfreezing %s @ %p\n", n);
        PyObject* c = n;
        n = get_scc_next(c);
        _Py_CLEAR_IMMUTABLE(c);
    } while (n != obj);
}

// Returns all the objects in the SCC to the Python cycle detector.
static void scc_dissolve_to_gc(PyObject* obj)
{
    PyObject* n = obj;
    do {
        PyObject* c = n;
        n = get_scc_next(c);
        scc_return_to_gc(c);
        debug("Returned %p rc = %zu to GC\n", c, Py_REFCNT(c));
        Py_DECREF(c);
    } while (n != obj);
}

static void scc_call_finalizers(PyObject *obj) {
    PyObject* n = obj;
    // Call the finalizers for all objects in the SCC.
    do {
        PyObject* c = n;
        n = get_scc_next(c);
        if (_PyGC_FINALIZED(c))
            continue;
        destructor finalize = Py_TYPE(c)->tp_finalize;
        if (finalize == NULL)
            continue;
        // Call the finalizer for the object.
        finalize(c);
        // Mark so we don't finalize it again.
        _PyGC_SET_FINALIZED(c);
    } while (n != obj);
}

static void scc_clear_weakrefs(PyObject *obj) {
    // Clear the remaining weakrefs without calling callbacks.
    PyObject *n = obj;
    do {
        PyObject* c = n;
        n = get_scc_next(c);
        if (_PyType_SUPPORTS_WEAKREFS(Py_TYPE(c))) {
            _PyWeakref_ClearWeakRefsNoCallbacks(c);
        }
    } while (n != obj);
}

static void scc_clear_objects(PyObject *obj) {
    // Clear the remaining weakrefs without calling callbacks.
    PyObject *n = obj;
    do {
        PyObject* c = n;
        n = get_scc_next(c);
        inquiry clear = Py_TYPE(c)->tp_clear;
        if (clear != NULL) {
            clear(c);
        }
    } while (n != obj);
}

static void scc_unfreeze_and_finalize(PyObject *obj) {
    scc_details_t details;
    scc_set_refcounts_to_one(obj);
    scc_reconstruct_rcs(obj, &details);
    scc_make_mutable(obj);

    // Legacy finalizers are delegated to Python's GC
    if (details.has_legacy_finalizers > 0) {
        debug("There are legacy finalizers in the SCC.  Let cycle detector handle this case.\n");
        debug("Legacy finalizers: %d\n", scc_details.has_legacy_finalizers);
        scc_dissolve_to_gc(obj);
        return;
    }

    if (details.has_finalizers) {
        scc_call_finalizers(obj);
    }

    if (details.has_weakreferences) {
        scc_clear_weakrefs(obj);
    }

    scc_clear_objects(obj);
    scc_dissolve_to_gc(obj);
}

// Perform a decref on an immutable object
int _Py_DecRef_Immutable(PyObject *op)
{
    assert(_Py_IsDeepImmutable(op));
    if (_Py_OB_FLAGS_LOAD(op) & _Py_IMMUTABLE_SCC_FLAG) {
        op = scc_get_representative(op);
    }
    assert(_Py_IsDeepImmutable(op));

    uint32_t old = _Py_atomic_add_uint32(&op->ob_refcnt, -1);
    assert(old > 0);
    if (old != 1) {
        return 0;
    }

    if (_Py_OB_FLAGS_LOAD(op) & _Py_IMMUTABLE_SCC_FLAG) {
        if (!weakref_handle_callbacks_scc(op)) {
            // Callbacks were scheduled, deallocation will be triggered again.
            return 0;
        }
        scc_unfreeze_and_finalize(op);
        return 0;
    }

    if (!weakref_handle_callbacks_single(op)) {
        // Callbacks were scheduled, deallocation will be triggered again.
        return false;
    }

    // If scc_init untracked this object it will set the scc representative to
    // the object itself, without setting the SCC flag. Here we can use this
    // information to make sure we only retrack op, if we untracked it previously.
    if (_PyObject_IS_GC(op) && scc_get_representative(op) == op) {
        scc_return_to_gc(op);
    }

    _Py_CLEAR_IMMUTABLE(op);

    return 1;
}

// _Py_RefcntAdd_Immutable(op, 1);
void _Py_RefcntAdd_Immutable(PyObject *op, Py_ssize_t increment)
{
    assert(_Py_IsDeepImmutable(op));
    if (_Py_OB_FLAGS_LOAD(op) & _Py_IMMUTABLE_SCC_FLAG) {
        op = scc_get_representative(op);
    }
    assert(_Py_IsDeepImmutable(op));

    _Py_atomic_add_uint32(&op->ob_refcnt, 1);
}

/* Tries to incref op and returns 1 if successful or 0 otherwise.
 * Used when creating a strong reference from a weak reference.
 * Needs to hold the weakref list lock (LOCK_WEAKREFS).
 */
int _Py_TryIncref_Immutable(PyObject *op)
{
    assert(_Py_IsDeepImmutable(op));
    if (_Py_OB_FLAGS_LOAD(op) & _Py_IMMUTABLE_SCC_FLAG) {
        op = scc_get_representative(op);
    }
    assert(_Py_IsDeepImmutable(op));

    uint32_t old = _Py_atomic_load_uint32_relaxed(&op->ob_refcnt);
    while (old > 0) {
        if (_Py_atomic_compare_exchange_uint32(&op->ob_refcnt, &old, old + 1)) {
            return 1;
        }
    }

    return 0;
}

/* Returns 1 if there are no references to the object's SCC. */
int _Py_IsDead_Immutable(PyObject *op)
{
    assert(_Py_IsDeepImmutable(op));
    if (_Py_OB_FLAGS_LOAD(op) & _Py_IMMUTABLE_SCC_FLAG) {
        op = scc_get_representative(op);
    }
    assert(_Py_IsDeepImmutable(op));

    return _Py_atomic_load_uint32_relaxed(&op->ob_refcnt) == 0;
}

#endif // _Py_PYRONA_INTERPRETER_SHARING
#pragma endregion

static int
_mark_deep_immutable_cb(_Py_hashtable_t *ht, const void *key, const void *value, void *user_data)
{
    PyObject *item = (PyObject *)key;
    _Py_SetDeepImmutable(item);
    return 0;
}

static int finish_deep_immutable_tree(_Py_hashtable_t *visited_set, PyObject *const *roots, int nroots) {
#ifdef _Py_PYRONA_INTERPRETER_SHARING
    // Has to run before the graph is marked deep immutable: scc_build treats a
    // deep immutable object as the boundary of an earlier freeze and skips it,
    // so marking first would make it walk nothing.
    //
    // The SCC construction will also remove the objects from the local GC list
    SUCCEEDS(scc_build(visited_set, roots, nroots));
#endif

    SUCCEEDS(_Py_hashtable_foreach(visited_set, _mark_deep_immutable_cb, NULL));

#ifdef _Py_PYRONA_INTERPRETER_SHARING
    // _PyWeakref_OnObjectFreeze asserts the deep flag, so this stays after.
    make_weakrefs_interpreter_safe(visited_set);
#endif

    // TODO(immutability): handle weakreferences
    return 0;

error:
    return -1;
}

#pragma region Immutable by construction

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
    _Py_SetShallowImmutable(obj);
    if (push(state->pending, obj) < 0) {
        return -1;
    }

    return 0;
}

int _PyImmutability_CanViewAsShallowImmutable(PyObject *obj)
{
    // Check if obj itself can be viewed as shallow immutable, that is whether
    // its own state is immutable by construction. What it references is not
    // inspected and may well be mutable.
    //
    // If so, the object is marked shallow immutable and 1 is returned.
    // Returns 0 if it cannot be viewed as shallow immutable, -1 on error.

    // Already shallow immutable — trivially yes.
    if (_Py_IsShallowImmutable(obj)) {
        return 1;
    }

    struct _Py_immutability_state *imm_state = get_immutable_state();
    if (imm_state == NULL) {
        return -1;
    }

    if (is_immutable_by_construction(imm_state, obj)) {
        _Py_SetShallowImmutable(obj);
        return 1;
    }

    return 0;
}

int _PyImmutability_CanViewAsDeepImmutable(PyObject *obj)
{
    // Check if the object graph rooted at obj can be viewed as deeply
    // immutable. It can if every reachable object is either already deeply
    // frozen, or is immutable by construction (its own state cannot be
    // mutated, though it may reference other objects).
    //
    // If the graph can be viewed as deeply immutable, it is deeply frozen
    // (to set up proper refcount management) and 1 is returned.
    // Returns 0 if the graph cannot be viewed as deeply immutable, -1 on error.

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
    state.imm_state = imm_state;

    // Mark root visited and seed the worklist. The root never passes through
    // can_view_as_immutable_visit, so it has to be marked here.
    if (_Py_hashtable_set(state.visited, obj, (void *)1) < 0) {
        PyErr_NoMemory();
        goto error;
    }
    _Py_SetShallowImmutable(obj);
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

    SUCCEEDS(finish_deep_immutable_tree(state.visited, &obj, 1));

    result = 1;
    goto finally;
error:
    result = -1;
finally:
    dealloc_implicit_freeze_state(&state);
    return result;
}

#pragma endregion Immutable by construction

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
    if ((_Py_OB_FLAGS_LOAD(obj) & _Py_PREFREEZE_RAN_FLAG) != 0) {
        return 0;
    }

    // Mark pre-freeze hook as completed. This has to be set before calling
    // the pre-freeze hook in case the pre-freeze hook reenters to prevent
    // an infinite loop.
    _Py_OB_FLAG_ADD(obj, _Py_PREFREEZE_RAN_FLAG);

    // Run the pre-freeze hook if it's present.
    return _run_pre_freeze_hook(imm_state, obj);
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

static int traverse_freeze(PyObject *obj, shallow_freeze_state_t *freeze_state)
{
    int result = 0;

#ifdef MERMAID_TRACING
    freeze_state->start = obj;
    TRACE_MERMAID_NODE(obj);
#endif

    debug_obj("Traversing %s (%p) rc=%zd\n", obj, Py_REFCNT(obj));

    if (is_c_wrapper(obj)) {
        return 0;
    }

    traverseproc reachable = get_reachable_proc(Py_TYPE(obj));
    if (PyType_Check(obj)) {
        // tp_mro, tp_bases and tp_base are guarded by the interpreter-wide
        // type lock, not by the type's own mutex (see TYPE_LOCK in
        // typeobject.c). The GC gets away without it by only traversing with
        // the world stopped; we traverse with other threads live, so we need
        // both locks. Holding them across the whole traversal is also what
        // keeps the borrowed references handed to freeze_visit alive.
        Py_BEGIN_CRITICAL_SECTION2_MUTEX(&PyInterpreterState_Get()->types.mutex,
                                         &obj->ob_mutex);
        result = reachable(obj, (visitproc)freeze_visit, freeze_state);
        Py_END_CRITICAL_SECTION2();
    }
    else {
        Py_BEGIN_CRITICAL_SECTION(obj);
        result = reachable(obj, (visitproc)freeze_visit, freeze_state);
        Py_END_CRITICAL_SECTION();
    }
    if (result != 0) {
        goto error;
    }

    // Weak references are not followed by the GC, but should be
    // for immutability.  Otherwise, we could share mutable state
    // using a weak reference.
    if (PyWeakref_Check(obj)) {
        PyObject* wr;
        int res = PyWeakref_GetRef(obj, &wr);
        if (res == -1) {
            goto error;
        }
        // wr is only set when res == 1; a dead referent leaves it NULL.
        if (res == 1) {
            result = freeze_visit(wr, freeze_state);
            Py_DECREF(wr);
            if (result) {
                goto error;
            }
        }
    }

    goto finally;
error:
    result = -1;
finally:
    return result;
}

static int
freeze_impl(PyObject *const *objs, Py_ssize_t nobjs, int atomic)
{
    struct _Py_immutability_state* imm_state = NULL;
    imm_state = get_immutable_state();
    if (imm_state == NULL) {
        return -1;
    }

    int result = 0;
    PyObject *item = NULL;
    TRACE_MERMAID_START();

    // Initialize the freeze state
    shallow_freeze_state_t state;
    SUCCEEDS(init_shallow_freeze_state(&state));
    state.atomic = atomic;

    // Register all roots and push onto the DFS stack
    for (Py_ssize_t i = 0; i < nobjs; i++) {
        if (!_Py_IsShallowImmutable(objs[i])) {
            // FIXME(immutable): It is not quite clear how `Explicit` should work
            // for nested freeze calls. One could argue that they should be frozen
            // if they're the root of at least one freeze call. Even if this is an
            // enclosing `freeze` call. For now we only allow `freeze` to explicitly
            // freeze root objects of its own freeze call and ignore enclosing ones.
            if (_Py_hashtable_set(state.roots, objs[i], objs[i]) < 0) {
                PyErr_NoMemory();
                goto error;
            }
        }
        if (!_Py_IsDeepImmutable(objs[i])) {
            SUCCEEDS(push(state.pending, objs[i]));
        }
    }

    // Late-init: mark importlib mutable state as not freezable.
    if (!imm_state->late_init_done) {
        late_init(imm_state);
    }

#ifdef Py_DEBUG
    // In debug mode, we can set a freeze location for debugging purposes.
    // Get a traceback object to use as the freeze location.
    //
    // This is purely diagnostic, so failures must never change the outcome of
    // the freeze. Any exception raised here is cleared rather than propagated.
    if (imm_state->traceback_func != NULL) {
        PyObject *stack = PyObject_CallFunctionObjArgs(imm_state->traceback_func, NULL);
        if (stack != NULL) {
            // Add the type name to the top of the stack, can be useful.
            PyObject* typename = PyObject_GetAttrString(_PyObject_CAST(Py_TYPE(objs[0])), "__name__");
            if (typename != NULL) {
                push_weak(stack, typename);
            }
            state.freeze_location = stack;
        }
        if (PyErr_Occurred()) {
            PyErr_Clear();
        }
    }
#endif

    // Walk the tree and mark all as shallow immutable
    while (PyList_Size(state.pending) != 0) {
        item = pop(state.pending);

        // This object and all reachable ones are deeply immutable, ignore them
        if (_Py_IsDeepImmutable(item)) {
            Py_CLEAR(item);
            continue;
        }

        // Skip object's e've already visited
        if (_Py_hashtable_get(state.visited, (void*)item)) {
            debug_obj("Already visited: %s (%p)\n", item);
            Py_CLEAR(item);
            continue;
        }

        // We only check the freezability and pre-freeze hook if the object is mutable.
        if (!_Py_IsShallowImmutable(item)) {
            // New object, check if freezable
            SUCCEEDS(check_freezable(imm_state, item, &state));
    
            // Call the pre-freeze hook if one is present
            SUCCEEDS(check_pre_freeze_hook(imm_state, item));
    
            // If the pre-freeze hook turned the object immutable, we want to skip it.
            if (_Py_IsDeepImmutable(item)) {
                Py_CLEAR(item);
                continue;
            }
            _Py_SetShallowImmutable(item);
        }

        // FIXME(immutability): For undoing freezes, we can just store a different value in
        // the hashtable, that way we can tell, if this object was shallow immutable before or not.
        //
        // Mark the object
        if (_Py_hashtable_set(state.visited, (void*)item, (void*)1)) {
            PyErr_NoMemory();
            goto error;
        }

        // Traverse the object
        SUCCEEDS(traverse_freeze(item, &state));

        Py_CLEAR(item);
    }

    SUCCEEDS(finish_deep_immutable_tree(state.visited, objs, nobjs));

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

// Make the given objects shallow immutable, without touching what they
// reference. A shallow immutable object may still reach mutable state, so it
// gets neither the deep flag nor any SCC or atomic refcount setup.
static int
shallow_freeze_impl(PyObject *const *objs, Py_ssize_t nobjs)
{
    struct _Py_immutability_state *imm_state = get_immutable_state();
    if (imm_state == NULL) {
        return -1;
    }

    int result = 0;
    shallow_freeze_state_t state;
    SUCCEEDS(init_shallow_freeze_state(&state));

    // Every object handed to shallow_freeze() is a root of this call, so
    // EXPLICIT objects are freezable here.
    for (Py_ssize_t i = 0; i < nobjs; i++) {
        if (_Py_hashtable_set(state.roots, objs[i], objs[i]) < 0) {
            PyErr_NoMemory();
            goto error;
        }
    }

    // Late-init: mark importlib mutable state as not freezable.
    if (!imm_state->late_init_done) {
        late_init(imm_state);
    }

    for (Py_ssize_t i = 0; i < nobjs; i++) {
        if (_Py_IsShallowImmutable(objs[i])) {
            continue;
        }

        SUCCEEDS(check_freezable(imm_state, objs[i], &state));
        SUCCEEDS(check_pre_freeze_hook(imm_state, objs[i]));

        _Py_SetShallowImmutable(objs[i]);
    }

    goto finally;
error:
    debug("Error during shallow freeze\n");
    result = -1;
finally:
    dealloc_shallow_freeze_state(&state);
    return result;
}

// Main entry point to make a single object shallow immutable.
int _PyImmutability_ShallowFreeze(PyObject* obj)
{
    if (_Py_IsShallowImmutable(obj)) {
        return 0;
    }
    return shallow_freeze_impl(&obj, 1);
}

// Make several objects shallow immutable.
// All provided objects are treated as roots for EXPLICIT freezable checks.
int _PyImmutability_ShallowFreezeMany(PyObject *const *objs, Py_ssize_t nobjs)
{
    return shallow_freeze_impl(objs, nobjs);
}

// Main entry point to deeply freeze an object and everything it can reach.
int _PyImmutability_DeepFreeze(PyObject* obj, int atomic)
{
    if(_Py_IsDeepImmutable(obj)){
        return 0;
    }
    return freeze_impl(&obj, 1, atomic);
}

// Deeply freeze multiple root objects and their reachable graphs together.
// All provided objects are treated as roots for EXPLICIT freezable checks.
int _PyImmutability_DeepFreezeMany(PyObject *const *objs, Py_ssize_t nobjs, int atomic)
{
    return freeze_impl(objs, nobjs, atomic);
}

// TODOs:
// - Run tests on Free-Threaded: Done :D
//    - RWLock on freeze for Rollback support
//    - Most `ob_flags` accesses will probably need to be atomic..
// - Maybe fix mermaid output
// - Review changes