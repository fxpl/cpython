#ifndef Py_INTERNAL_REGION_H
#define Py_INTERNAL_REGION_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

#include "object.h"
#include "region.h"
#include "pycore_ownership.h"
#include "pycore_gc.h" // PyGC_Head

/* Macros for readability */
#define NULL_REGION 0

/* PyObject_HEAD defines the initial segment of every PyObject used as a region bridge. */
#define PyBridgeObject_HEAD \
    PyObject_HEAD; \
    /* The region value which will be updated and \
     * still filled when the dealloc function of \
     * the object is called. \
     */ \
    Py_region_t region; \
    /** The name of the region or NULL */ \
    PyObject *name;

#define PyBridgeObject_HEAD_INIT(op) \
    op->region = NULL_REGION; \
    op->name = NULL;

/**
 * Objects used as bridges need to have an additional region field, which is
 * still filled in the dealloc function. This should be the inital segment,
 * similar to how `PyObject` is the inital segment for other objects.
*/
typedef struct _PyBridgeObject {
    PyBridgeObject_HEAD;
} _PyBridgeObject;

#define _PyBridgeObject_CAST(op) _Py_CAST(_PyBridgeObject*, op)

// FIXME(regions): xFrednet: Several parts of this state should be atomic to
//      allow weak references from asking if the region is currently accessable.
//      This might also be helpful to reduce the level of corruption which can
//      happen when a region is somehow shared across threads. It would be
//      interesting to see if using more atomics here has a performance impact.
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
     * - 0b11 => The pointer points to the parent in the union-fing forest, but the
     *           merge is not confirmed yet. Meaning references should not updated.
     *
     * Use the macros in `regions.c` to access these
     */
    Py_uintptr_t owner;

    /* The bridge object belonging to this _Py_region_data. This pointer can be
     * NULL, when the bridge was already deallocated but some objects retain
     * a reference to the `_Py_region_data` object.
     *
     * This is a weak reference to the brige, meaning the RC is not updated
     * by writes to this field.
     */
    _PyBridgeObject* bridge;

    /* Objects have to be removed from their local GC cycle, when they're moved
     * into a region. Instead they're moved into this list, to allow GC inside
     * the region.
     * 
     * Bridges can't form cycles with objects outside their regions (Mudolo cowns).
     * It should therefore be safe to take them out of the GC cycle.
     */
    PyGC_Head gc_list;

#ifdef Py_OWNERSHIP_INVARIANT
    _Py_ownership_invariant_region_data invariant_data;
#endif
} _Py_region_data;

typedef struct _PyCownObject _PyCownObject;

/** Callback to the owning cown. The first argument is the bride of of the
 * owned region. The second argument is the concurrent unit id.
 * 
 * Note that these callbacks have to be thread safe, as they may be called
 * from multiple threads simutaniously.
 */
typedef int(*_Py_cown_state_callback)(_PyCownObject *op, _PyBridgeObject* region, uint64_t cuid);
typedef int(*_Py_cown_merge_callback)(_PyCownObject *op, _PyBridgeObject* region, uint64_t cuid, Py_region_t into_region, _PyBridgeObject* into_bridge);

typedef struct _Py_cown_callbacks
{
    _Py_cown_state_callback open;
    _Py_cown_state_callback close;
    _Py_cown_merge_callback merge;
    _Py_cown_state_callback is_accessible;
} _Py_cown_callbacks;

struct _PyCownObject {
    PyObject_HEAD;
    _Py_cown_callbacks *callbacks;
};
#define _PyCownObject_CAST(op) _Py_CAST(_PyCownObject*, op)

PyAPI_FUNC(int) _PyRegion_New(_PyBridgeObject *bridge);
PyAPI_FUNC(int) _PyRegion_Dissolve(Py_region_t region);
PyAPI_FUNC(void) _PyRegion_DecRc(Py_region_t region);

PyAPI_FUNC(Py_ssize_t) _PyRegion_GetLrc(Py_region_t region);
PyAPI_FUNC(Py_ssize_t) _PyRegion_GetOsc(Py_region_t region);
PyAPI_FUNC(int) _PyRegion_IsOpen(Py_region_t region);
PyAPI_FUNC(int) _PyRegion_IsDirty(Py_region_t region);
PyAPI_FUNC(int) _PyRegion_IsParent(Py_region_t child, Py_region_t parent);
PyAPI_FUNC(int) _PyRegion_ClosesWithLrc(Py_region_t region, Py_ssize_t lrc);
PyAPI_FUNC(Py_region_t) _PyRegion_GetParent(Py_region_t child);
PyAPI_FUNC(int) _PyRegion_Clean(Py_region_t region);

PyAPI_FUNC(int) _PyRegion_IsBridge(PyObject *obj);
PyAPI_FUNC(PyObject*) _PyRegion_GetBridge(Py_region_t region);
PyAPI_FUNC(void) _PyRegion_RemoveBridge(Py_region_t region);

PyAPI_FUNC(int) _PyRegion_SignalImmutable(PyObject *obj);
PyAPI_FUNC(void) _PyRegion_SignalDealloc(PyObject *obj);

PyAPI_FUNC(void) _PyRegion_HackDirtyForPrototype(Py_region_t region);

PyAPI_FUNC(int) _PyRegion_SetCownRegion(_PyCownObject *cown);
PyAPI_FUNC(int) _PyRegion_HasOwner(Py_region_t region);
PyAPI_FUNC(int) _PyRegion_SetCown(_PyBridgeObject* bridge, _PyCownObject *cown);
PyAPI_FUNC(int) _PyRegion_RemoveCown(_PyBridgeObject* bridge, _PyCownObject *cown);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_REGION_H */
