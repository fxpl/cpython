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
    /* Temporary value until the state always has a field to indicate this.
    */
    int is_initilized;
#ifdef Py_OWNERSHIP_INVARIANT
    /* This value indicates the state of the ownership invariant. The invariant
    * has to support operations which might reenter into Python and then
    * call other ownership functions. These are the states:
    * -1 => The invariant is disabled.
    * 0 => The invariant is enabled and running.
    * N => The invariant is enabled but waiting on N operations as these might
    *      temporarly violate the invariant.
    */
    int invariant_state;
#endif
} _Py_ownership_state;

/* This function returns true for C wrappers around functions, types and
* all kinds of wrappers around C with immutable state. For ownership these
* can be seen as immutable, meaning they can be referenced from immutable
* objects and from inside regions.
**/
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
