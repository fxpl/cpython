#ifndef Py_CPYTHON_IMMUTABLE_H
#  error "this header file must not be included directly"
#endif

typedef enum {
    _Py_FREEZABLE_YES = 0,
    _Py_FREEZABLE_NO = 1,
    _Py_FREEZABLE_EXPLICIT = 2,
    _Py_FREEZABLE_PROXY = 3,
} _Py_freezable_status;

/* Immutability comes in two depths:
 *
 *   shallow: the object's own state cannot be mutated, but the objects it
 *            references may still be mutable.
 *   deep:    the object and everything reachable from it is immutable. Only
 *            deeply immutable objects can be shared between interpreters.
 *
 * A deeply immutable object is always shallow immutable too.
 *
 * Freezing is not atomic and cannot be rolled back. A deep freeze marks each
 * object shallow immutable as it walks the graph, so when it fails part-way
 * any subset of the reachable objects may already be shallow immutable, and
 * they stay that way. Nothing becomes deeply immutable unless the whole
 * freeze succeeds.
 *
 * Retrying after removing the blocker completes the freeze: objects that are
 * already shallow immutable are not re-checked for freezability and their
 * pre-freeze hook does not run a second time.
 */

/* Make `obj` shallow immutable. Referenced objects are left alone.
 *
 * `obj` is a root of this freeze, so a FREEZABLE_EXPLICIT object is frozen.
 * Returns 0 on success, -1 with an exception set otherwise.
 */
PyAPI_FUNC(int) _PyImmutability_ShallowFreeze(PyObject*);

/* Shallow freeze several objects. Each is a root of this freeze. */
PyAPI_FUNC(int) _PyImmutability_ShallowFreezeMany(PyObject *const *, Py_ssize_t);

/* Make `obj` and everything reachable from it deeply immutable.
 *
 * `obj` is a root of this freeze, so a FREEZABLE_EXPLICIT object is frozen.
 * Returns 0 on success, -1 with an exception set otherwise. On failure part
 * of the graph is left shallow immutable; see the note above. The object
 * that could not be frozen is attached to the exception as `obj`.
 */
PyAPI_FUNC(int) _PyImmutability_DeepFreeze(PyObject*, int atomic);

/* Deep freeze several object graphs together. All objects are treated as
 * roots, so FREEZABLE_EXPLICIT applies to each of them.
 */
PyAPI_FUNC(int) _PyImmutability_DeepFreezeMany(PyObject *const *, Py_ssize_t, int atomic);

PyAPI_FUNC(int) _PyImmutability_RegisterImmutableByConstruction(PyTypeObject*);

/* Check whether `obj` itself is immutable by construction, i.e. whether it can
 * be viewed as shallow immutable without freezing anything.
 *
 * On success the object is marked shallow immutable and 1 is returned.
 * Returns 0 if it cannot be viewed as shallow immutable, -1 on error.
 */
PyAPI_FUNC(int) _PyImmutability_CanViewAsShallowImmutable(PyObject*);

/* Check whether the whole graph reachable from `obj` is immutable by
 * construction.
 *
 * On success the graph is deeply frozen (to set up the shared refcount
 * management) and 1 is returned.
 * Returns 0 if it cannot be viewed as deeply immutable, -1 on error.
 */
PyAPI_FUNC(int) _PyImmutability_CanViewAsDeepImmutable(PyObject*);

PyAPI_FUNC(int) _PyImmutability_SetFreezable(PyObject *, _Py_freezable_status);
PyAPI_FUNC(int) _PyImmutability_GetFreezable(PyObject *);
PyAPI_FUNC(int) _PyImmutability_UnsetFreezable(PyObject *);
