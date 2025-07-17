#ifndef Py_INTERNAL_OWNERSHIP_H
#define Py_INTERNAL_OWNERSHIP_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

#include "exports.h"

typedef struct _Py_ownership_state {
    /* Temporary value until the state always has a field to indicate this. */
    int is_initialized;
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
} _Py_ownership_state;

/* This function returns true for C wrappers around functions, types and
 * all kinds of wrappers around C with immutable state. For ownership these
 * can be seen as immutable, meaning they can be referenced from immutable
 * objects and from inside regions.
 */
PyAPI_FUNC(int) _PyOwnership_is_c_wrapper(PyObject *obj);

/* This function calls the `visit` function for the fields of the `obj`
 * which should be effected by ownership. The `data` pointer will be
 * passed along as the second argument to `visit`.
 */
PyAPI_FUNC(int) _PyOwnership_traverse_obj(PyObject *obj, visitproc visit, void *data);

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
