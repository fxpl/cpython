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
#include "pycore_hashtable.h"

typedef struct _PyOwnershipList _PyOwnershipList;
PyAPI_FUNC(_PyOwnershipList*) _PyOwnershipList_new(void);
PyAPI_FUNC(void) _PyOwnershipList_free(_PyOwnershipList* self);
PyAPI_FUNC(int) _PyOwnershipList_push(_PyOwnershipList* self, PyObject *item);
PyAPI_FUNC(PyObject *) _PyOwnershipList_pop(_PyOwnershipList* self);

typedef struct _Py_ownership_state {
    /* The global ownership tick used to mark open regions as dirty, if their
    * invariant might broken. This can happen if untrusted C code is called
    * which doesn't have write barriers. This C code might create references
    * between objects which could violate the invariant. Marking a region as
    * dirty means that it has to be cleaned, before the region can be closed.
    *
    * The tick has two kinds of values:
    * - Even => A region was opened
    * - Odd  => Untrusted code was called and all currently open regions
    *           should be marked as dirty.
    *
    * Transitions by increment:
    * - From even to odd => Unknown C code was called
    * - From odd to even => A new region was opened
    *
    * This mechanism allows marking all regions as dirty with a single tick
    * change.
    *
    * Invariant: The tick counter should always be greater or equal to two
    * as the values 0 and 1 are reserved values by `_Py_region_data.open_tick`.
    * */
    Py_ssize_t tick;

    _Py_hashtable_t *warned_types;
#ifdef Py_DEBUG
    /* The name of the last type that marked all open regions as dirty.
    *
    * This is only intended for debugging
    */
    PyObject* last_dirty_reason;
#endif
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
    PyObject *location_key;
#endif
} _Py_ownership_state;

/* This retrives the current ownership tick or 0 if the tick retrival failed.
* See `_Py_ownership_state.tick`
*/
PyAPI_FUNC(Py_ssize_t) _PyOwnership_get_current_tick(void);

/* Returns the tick which should be used for `region.open_tick` or 0 if the
* ownerstate is currently unavailble.
*/
PyAPI_FUNC(Py_ssize_t) _PyOwnership_get_open_region_tick(void);

/* This function should be called when, untrusted code is executed. It will
* mark all currently open regions as dirty.
*
* It can fail, if the ownership state is currently unavailable
*/
PyAPI_FUNC(int) _PyOwnership_notify_untrusted_code(const char* reason);
PyAPI_FUNC(PyObject*) _PyOwnership_get_last_dirty_reason(void);

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
#ifdef Py_DEBUG
    int freeze_location,
#endif
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
PyAPI_FUNC(int) _PyOwnership_invariant_disable(void);

typedef struct _Py_ownership_invariant_region_data {
    Py_region_t next;
    Py_ssize_t lrc;
    Py_ssize_t osc;
} _Py_ownership_invariant_region_data;

#else
static inline int _PyOwnership_invariant_enable(void) { return 0; }
static inline int _PyOwnership_invariant_pause(void) { return 0; }
static inline int _PyOwnership_invariant_resume(void) { return 0; }
static inline int _PyOwnership_invariant_disable(void) { return 0; }
#endif

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_OWNERSHIP_H */
