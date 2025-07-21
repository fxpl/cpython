#ifndef Py_INTERNAL_OWNERSHIP_H
#define Py_INTERNAL_OWNERSHIP_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

#include "exports.h"
#include "object.h"

typedef struct _Py_ownership_state {
    /* Temporary value until the state always has a field to indicate this. */
    int is_initialized;
    // FIXME: xFrednet: Can we remove this special casing in favor of
    //     unfreezable fields or thread local wrappers.
    PyObject *module_locks;
    PyObject *blocking_on;
#ifdef Py_OWNERSHIP_INVARIANT
    /* Tracks the state of the ownership invariant. Some ownership-related
     * operations may temporarily violate the invariant. To handle this safely,
     * the invariant must be suspended during such operations and only resumed
     * once all of them complete. This is necessary to support re-entrancy.
     *
     * For example, during freezing, the object graph is traversed and objects
     * are marked as immutable — even while they may still reference mutable
     * objects. If the invariant were enforced mid-way, it would raise a
     * (premature) error, despite the state being corrected as the operation
     * completes. To avoid this, the invariant must be paused during the freeze.
     *
     * States:
     *  -1 => The invariant is disabled.
     *   0 => The invariant is active and enforced.
     *   N => The invariant is temporarily paused. The value indicates the
     *        number of suspensions yet to be resumed (this supports nesting).
     */
    int invariant_state;
#endif
#ifdef Py_DEBUG
    /* Function to create a traceback object in debug builds. This is only used
     * for debugging and can be NULL
     */
    PyObject *traceback_func;
#endif
} _Py_ownership_state;

PyAPI_FUNC(int) _PyOwnership_is_c_wrapper(PyObject *obj);
/* Called for every object, to check what should be done with it. This
 * can be used to implemented a set visited objects and avoid traversing
 * objects multiple times.
 * 
 * The return value indicates success and if the object should be
 * traversed. These are the return values:
 *   -1) Failure
 *    0) Ok, but don't traverse the object
 *    1) Ok, and traverse the object
 */
typedef int (*ownershipcheckproc)(PyObject* obj, void *state);

/* Like `visitproc` for `_PyOwnership_traverse_object_graph`. The first
 * argument is the source of the reference and the second one is the
 * referenced object.
 * 
 * The return value indicates success and if the target object should be
 * traversed. These are the return values:
 *   -1) Failure, stop traversal
 *    0) Ok, but don't traverse the target object
 *    1) Ok, and traverse the target object
 */
typedef int (*ownershipvisitproc)(PyObject* src, PyObject* tgt, void *state);

#define Py_OWNERSHIP_TRAVERSE_ERR   -1
#define Py_OWNERSHIP_TRAVERSE_SKIP   0
#define Py_OWNERSHIP_TRAVERSE_VISIT  1

PyAPI_FUNC(int) _PyOwnership_traverse_object_graph(
    PyObject *obj,
    ownershipcheckproc caller_check,
    ownershipvisitproc caller_visit,
    void *caller_state
);

#ifdef Py_OWNERSHIP_INVARIANT

#include "object.h" // PyObject, visitproc
#include "pytypedefs.h" // PyThreadState

#define Py_OWNERSHIP_INVARIANT_DISABLED -1
#define Py_OWNERSHIP_INVARIANT_ENABLED 0

/* This function validates that the current heap follows the ownership
 * rules. This is a slow operation and should only be done for debugging.
 *
 * 0 indicates a valid heap, -1 will be returned if an error was thrown.
 */
PyAPI_FUNC(int) _PyOwnership_check_invariant(PyThreadState *tstate);

PyAPI_FUNC(int) _PyOwnership_invariant_enable(void);
PyAPI_FUNC(int) _PyOwnership_invariant_pause(void);
PyAPI_FUNC(int) _PyOwnership_invariant_resume(void);

#else
#   define _PyOwnership_invariant_enable() 0 /* success */
#   define _PyOwnership_invariant_pause() 0 /* success */
#   define _PyOwnership_invariant_resume() 0 /* success */
#endif

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_OWNERSHIP_H */
