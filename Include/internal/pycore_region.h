#ifndef Py_INTERNAL_REGION_H
#define Py_INTERNAL_REGION_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

#include "object.h"
#include "pycore_ownership.h"

/* Macros for readability */
#define NULL_REGION 0

typedef struct _Py_region_data {
    /* The number of references coming in from the local region.
     *
     * This value should always be >= 0 with the exception of
     * the `add_to_region` process. This can create a temporary
     * region, which will be merged into the target region. The
     * LRC can be negative, if the merge should decrease the LRC
     * of the target region.
     */
    Py_ssize_t lrc;

    /* The number of open subregions. */
    Py_ssize_t osc;

    /* Snapshot of the ownership tick, when the region was opened. This
     * is used to track if the region is open and if the region is clean.
     *
     * If the region is clean, it means the LRC and OSC can be trusted to
     * securely close the region. However, these values might be incorrect,
     * if the region is dirty. This can happen, when we call untrusted C
     * code. A dirty region first has to be cleaned, before it can be closed.
     *
     * See `_Py_ownership_state.tick` for an explaination of the tick counter.
     *
     * This value indicates the following states:
     * - (0) => The region is closed
     * - (1) => The region is open and dirty
     * - (N) if N == state.tick => The region is open and clean, since the
     *                             ownership and open tick are the same
     * - (N) if N != state.tick => The region is open but dirty, since an
     *                             ownership tick was triggered.
     *
     * Invariant: The open tick should always be 1 or an even number.
     */
    Py_ssize_t open_tick;

    /* The number of references to this object */
    Py_ssize_t rc;

    /* A tagged pointer to the owner of this region. The tag indicates the
     * type of owner and relationship:
     *
     * These are the possible tags:
     * - 0b00 => The pointer points to the parent region (or is null)
     * - 0b01 => The pointer points to the cown owing this region
     * - 0b10 => The pointer points to the parent in the union-find forest
     */
    Py_uintptr_t owner;

    /* The bridge object belonging to this _Py_region_data. This pointer can be
     * NULL, when the bridge was already deallocated but some objects retain
     * a reference to the `_Py_region_data` object.
     *
     * This is a weak reference to the brige, meaning the RC is not updated
     * by writes to this field.
     */
    PyObject* bridge;
    // TODO: Probably not safe rn, since name could be removed by the GC
    PyObject *name;

#ifdef Py_OWNERSHIP_INVARIANT
    _Py_ownership_invariant_region_data invariant_data;
#endif
} _Py_region_data;

PyAPI_FUNC(Py_region_t) _PyRegion_GetSlow(PyObject *obj);

/* Returns the region of the given object.
 */
static inline Py_region_t _PyRegion_Get(PyObject *obj) {
    assert(obj);

    // Immutable objects can be shared across threads, it's not save to access
    // the region information without synchronization.
    if (_Py_IsImmutable(obj)) {
        return _Py_IMMUTABLE_REGION;
    }

    // Fast path, almost every object should be in one of these regions
    if (obj->ob_region == _Py_LOCAL_REGION
        || obj->ob_region == _Py_COWN_REGION
    ) {
        return obj->ob_region;
    }

    return _PyRegion_GetSlow(obj);
}

static inline int _Py_IsLocal(PyObject *obj) {
    return _PyRegion_Get(obj) == _Py_LOCAL_REGION;
}
#define _Py_IsLocal(obj) _Py_IsLocal(_PyObject_CAST(obj))

PyAPI_FUNC(Py_region_t) _PyRegion_New(PyObject *bridge);
PyAPI_FUNC(void) _PyRegion_DecRc(Py_region_t region);

PyAPI_FUNC(int) _PyRegion_IsOpen(Py_region_t region);
PyAPI_FUNC(int) _PyRegion_IsDirty(Py_region_t region);
PyAPI_FUNC(int) _PyRegion_IsParent(Py_region_t child, Py_region_t parent);
PyAPI_FUNC(Py_region_t) _PyRegion_GetParent(Py_region_t child);


PyAPI_FUNC(int) _PyRegion_IsBridge(PyObject *obj);
PyAPI_FUNC(PyObject*) _PyRegion_GetBridge(Py_region_t region);

PyAPI_FUNC(int) _PyRegion_SignalImmutable(PyObject *obj);

// Helper macros to count the number of arguments
#define _PyRegion__COUNT_ARGS(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N, ...) N
#define _PyRegion_COUNT_ARGS(...) _PyRegion__COUNT_ARGS(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1)
#define _PyRegion_MAX_ARG_COUNT 16

PyAPI_FUNC(int) _PyRegion_AddRef(PyObject *src, PyObject *tgt);
PyAPI_FUNC(int) _PyRegion_AddRefs(PyObject *src, int tgt_count, ...);
#define _PyRegion_ADDREF(src, tgt) _PyRegion_AddRef(_PyObject_CAST(src), _PyObject_CAST(tgt))
#define _PyRegion_ADDREFS(src, ...) _PyRegion_AddRefs(_PyObject_CAST(src), _PyRegion_COUNT_ARGS(__VA_ARGS__), __VA_ARGS__)

PyAPI_FUNC(int) _PyRegion_RemoveRef(PyObject *src, PyObject *tgt);
#define _PyRegion_REMOVEREF(src, tgt) _PyRegion_RemoveRef(_PyObject_CAST(src), _PyObject_CAST(tgt))

PyAPI_FUNC(int) _PyRegion_AddLocalRef(PyObject *tgt);
PyAPI_FUNC(int) _PyRegion_AddLocalRefs(int tgt_count, ...);
#define _PyRegion_ADDLOCALREF(tgt) _PyRegion_AddLocalRef(_PyObject_CAST(tgt))
#define _PyRegion_ADDLOCALREFS(tgt) _PyRegion_AddLocalRefs(_PyRegion_COUNT_ARGS(__VA_ARGS__), __VA_ARGS__)

PyAPI_FUNC(int) _PyRegion_RemoveLocalRef(PyObject *tgt);
#define _PyRegion_REMOVELOCALREF(tgt) _PyRegion_RemoveLocalRef(_PyObject_CAST(tgt))

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_REGION_H */
