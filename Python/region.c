#include "Python.h"
#include "refcount.h"
#include "pyerrors.h"

#include "pycore_interp.h"      // PyThreadState_Get
#include "pycore_ownership.h"
#include "pycore_pyerrors.h"
#include "pycore_region.h"
#include "pycore_runtime.h"    // _Py_ID

#include <stdbool.h>

typedef struct regiondata regiondata;

/* Macro that jumps to error, if the expression `x` does not succeed. */
#define SUCCEEDS(x) { do { int r = (x); if (r != 0) goto error; } while (0); }

/* Macros for readability */
#define NULL_REGION 0

/* Checks for predefined static regions without data */
#define IS_LOCAL_REGION(r)        ((Py_region_t)(r) == _Py_LOCAL_REGION)
#define IS_IMMUTABLE_REGION(r)    ((Py_region_t)(r) == _Py_IMMUTABLE_REGION)
#define IS_COWN_REGION(r)         ((Py_region_t)(r) == _Py_COWN_REGION)
#define HAS_DATA(r)               (!IS_LOCAL_REGION(r) && !IS_IMMUTABLE_REGION(r) && !IS_COWN_REGION(r))

/* Magic values for `regiondata.open_tick` */
#define OPEN_TICK_CLOSED 0
#define OPEM_TICK_DIRTY 1

/* Macros to access the owner and check for tags */
#define OWNER_TAG_COWN              ((Py_uintptr_t)0x1)
#define OWNER_TAG_MERGED            ((Py_uintptr_t)0x2)
#define OWNER_PTR_MASK              (~(OWNER_TAG_COWN | OWNER_TAG_MERGED))
#define GET_OWNER_WITH_TAG(data)    (((regiondata*)(data))->owner)
#define GET_OWNER_PTR(data)         (GET_OWNER_WITH_TAG(data) & OWNER_PTR_MASK)
#define HAS_OWNER_TAG(data, tag)    (GET_OWNER_WITH_TAG(data) & tag)

/* Helper macros */
#define ASSERT_IS_UNION_ROOT(region) assert(!HAS_DATA(region) || !HAS_OWNER_TAG(region, OWNER_TAG_MERGED))
#define ASSERT_REGION_HAS_NO_TAG(region) assert((region & OWNER_PTR_MASK) == region)

struct regiondata {
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

    /* The bridge object belonging to this regiondata. This pointer can be
     * NULL, when the bridge was already deallocated but some objects retain
     * a reference to the `regiondata` object.
     *
     * This is a weak reference to the brige, meaning the RC is not updated
     * by writes to this field.
     */
    PyObject* bridge;
    // TODO: Probably not safe rn, since name could be removed by the GC
    PyObject *name;
};

// Prototyes
static int regiondata_inc_osc(Py_region_t region);
static int regiondata_dec_osc(Py_region_t region);
static int regiondata_is_open(Py_region_t data);
static Py_region_t regiondata_get_parent(Py_region_t region);
static int regiondata_set_parent(Py_region_t region, Py_region_t new_parent);

// This uses the given arguments to create and throw a `RegionError`
static void throw_region_error(
    const char *format_str, PyObject *format_args,
    PyObject* src, PyObject* tgt)
{
    // Don't stomp existing exception
    PyThreadState *tstate = PyThreadState_Get();
    if (_PyErr_Occurred(tstate)) {
        return;
    }

    PyErr_Format(PyExc_RuntimeError, format_str, format_args);

    // Set source and target fields
    // Get the current exception (should be a RuntimeError)
    PyObject *exc = PyErr_GetRaisedException();
    assert(exc && PyObject_TypeCheck(exc, (PyTypeObject *)PyExc_RuntimeError));

    // Add 'source' and 'target' attributes to the exception
    PyObject_SetAttr(exc, &_Py_ID(source), src ? src : Py_None);
    PyObject_SetAttr(exc, &_Py_ID(target), tgt ? tgt : Py_None);

    PyErr_SetRaisedException((PyObject*)exc);
}

static Py_region_t regiondata_new() {
    regiondata* data = (regiondata*)calloc(1, sizeof(regiondata));
    if (data == NULL) {
        return NULL_REGION;
    }

    data->rc = 1;
    return (Py_region_t)data;
}

static void regiondata_inc_rc(Py_region_t region) {
    if (!HAS_DATA(region)) {
        return;
    }

    // Change RC
    regiondata *data = (regiondata*)region;
    data->rc += 1;
}

static void regiondata_dec_rc(Py_region_t region) {
    if (!HAS_DATA(region)) {
        return;
    }

    // Change RC
    regiondata *data = (regiondata*)region;
    data->rc -= 1;

    // Dealloc if needed
    if (data->rc == 0) {
        // The RC should never hit zero with a cown as the parent
        assert(HAS_OWNER_TAG(data, OWNER_TAG_COWN) == 0);

        // The region has to be closed, when the RC hits zero
        assert(data->open_tick == OPEN_TICK_CLOSED);

        // Decrement the owner RC, the owner will always be a region at
        // this point. This accesses the owner directly since we want
        // to decrement the RC of this specific owning region not the
        // root of the union find.
        regiondata_dec_rc(GET_OWNER_PTR(region));

        // Free the data belonging to this region
        free(data);
    }
}

/* Returns the root of the union-find tree that the given region is a part of
 */
static Py_region_t regiondata_union_root(Py_region_t region) {
    // Regions without data are always roots of the union-find forest
    if (!HAS_DATA(region)) {
        return region;
    }

    // Return if this if the root of the union-find
    if (!HAS_OWNER_TAG(region, OWNER_TAG_MERGED)) {
        return region;
    }

    // Increase the RC of `region` to avoid special casing in the following code
    regiondata_inc_rc(region);

    // Keep the child pointer to reassign the owner and correct the RC
    regiondata *child = (regiondata*)region;
    region = GET_OWNER_PTR(region);

    // Walk the union-find until the root is reached
    while (HAS_DATA(region) && HAS_OWNER_TAG(region, OWNER_TAG_MERGED)) {
        // Assign the owner of the child. This halves the tree everytime the
        // root is search for. This results in an amortized time of O(1).
        child->owner = GET_OWNER_WITH_TAG(region);

        // The RC of the `regiondata` which was previously the owner of
        // `child` has to be decremented. However, this might deallocate
        // the object. This code therefore wait until the next iteration
        // when the `region` is stored in `child` to decrement the RC.
        regiondata_dec_rc((Py_region_t)child);

        // Prepare `child` and `region` values for the next iteration.
        child = (regiondata*)region;
        region = GET_OWNER_PTR(region);
    }

    // Cleanup RC count
    regiondata_dec_rc((Py_region_t)child);

    // The `region` value now holds the root of the union-find tree.
    return region;
}

// FIXME: xFrednet: If performance of this becomes a problem, we could write a
// specialized version for merging into static regions as this makes several
// operations easier. The compiler could figure several of these out, but it
// would require several layers of inlining.
static int regiondata_union_merge(
    Py_region_t source, Py_region_t target
) {
    // Invariant:
    assert(HAS_DATA(source));
    ASSERT_IS_UNION_ROOT(source);
    ASSERT_IS_UNION_ROOT(target);
    ASSERT_REGION_HAS_NO_TAG(target);

    int result = 0;

    // Increase the RC of `target` to make sure none of the following
    // operations deallocates it by accident.
    regiondata_inc_rc(target);

    // If the target was open, we increment the OSC by one to keep it
    // open until this merge is done. This makes sure that a region
    // doesn't get closed and reopened.
    bool cleanup_inc_osc = false;
    if (regiondata_is_open(target)) {
        // Inc OSC can't fail here, since `target` is already open
        regiondata_inc_osc(target);
        cleanup_inc_osc = true;
    }

    // If `target` is the parent of `source` it can be merged. This unsets
    // the parent of `source` to correctly update the OSC and RC.
    Py_region_t source_parent = regiondata_get_parent(source);
    if (source_parent == target) {
        // Set parent can't fail here, since this function has increased the
        // OSC, thereby keeping the region open if it was previously open.
        regiondata_set_parent(source, NULL_REGION);
        source_parent = NULL_REGION;
    }

    // `source` can't be merged if it has any other parent than `target`
    // as the link from `source_parent` to the bridge of `source` would
    // break isolation after the merge. However, a merge of source into
    // target is always allowed.
    if (source_parent != NULL_REGION && !IS_IMMUTABLE_REGION(target)) {
        // TODO: xFrednet: Better error message with explaination and conditional
        // based on if X is static
        throw_region_error(
            "unable to merge X into Y since X still has a parent", Py_None,
            Py_None, Py_None);
        goto error;
    }

    // Set the owner to the target with the merged tag
    regiondata *source_data = (regiondata*) source;
    regiondata_inc_rc(target);
    source_data->owner = target | OWNER_TAG_MERGED;

    // Merge stats into the `target`
    if (HAS_DATA(target)) {
        regiondata *target_data = (regiondata*) target;
        target_data->lrc += source_data->lrc;
        target_data->osc += source_data->osc;

        // Check how the `open_tick` should be updated
        if (target_data->open_tick == OPEN_TICK_CLOSED) {
            // The target was previously closed, merging the new data
            // might have opened it. Taking the `open_tick` from `source`
            // puts target into the right state.
            target_data->open_tick = source_data->open_tick;
        } else if (target_data->open_tick != source_data->open_tick) {
            // At least one of the regions was dirty since the `open_tick`
            // is mismatching.
            target_data->open_tick = OPEM_TICK_DIRTY;
        } else {
            // The open ticks are equal, nothing needs to be done
        }
    }

    // Remove information from `source`
    source_data->bridge = NULL;
    source_data->lrc = 0;
    source_data->osc = 0;
    source_data->open_tick = OPEN_TICK_CLOSED;

    // Skip the error label and run the normal cleanup code
    goto cleanup;

error:
    result = 1;

cleanup:
    // This returns the OSC which was aquired ealier to keep it open during
    // this merge.
    if (cleanup_inc_osc) {
        result |= regiondata_dec_osc(target);
    }

    // Decrement the `target` RC again
    regiondata_dec_rc(target);

    return result;
}

/* This opens the region and marks it as clean.
 *
 * This operation may fail if:
 * - The `_Py_ownership_state` is currently unavailable
 * - Opening a parent region failed
 * - TODO: xFrednet: If the owing cown is released.
 */
static int regiondata_open(Py_region_t region) {
    // Invariant:
    ASSERT_IS_UNION_ROOT(region);

    // Regions without metadata are always open
    if (!HAS_DATA(region)) {
        return 0;
    }

    // Don't reopen a open region, as that would mark it as clean again
    if (regiondata_is_open(region)) {
        return 0;
    }

    // Mark the region as open.
    regiondata *data = (regiondata*)region;
    data->open_tick = _PyOwnership_get_open_region_tick();

    // Check if opening the region was successful
    if (data->open_tick == OPEN_TICK_CLOSED) {
        return 1;
    }

    // The open tick should always be even, see invariant
    assert((data->open_tick % 2) == 0);

    // Notify the owner
    if (HAS_OWNER_TAG(region, OWNER_TAG_COWN)) {
        // TODO: xFrednet: Implement this branch, probably just an assert
        assert(false);
    } else if (regiondata_get_parent(region) != 0) {
        SUCCEEDS(regiondata_open(regiondata_get_parent(region)));
    }

    // Check for failure, which would leave the region closed
    return 0;

error:
    // Mark the region as closed on failure.
    data->open_tick = OPEN_TICK_CLOSED;
    return 1;
}

static int regiondata_is_open(Py_region_t region) {
    // Invariant:
    ASSERT_IS_UNION_ROOT(region);

    // Regions without metadata are always open
    if (!HAS_DATA(region)) {
        return true;
    }

    return ((regiondata*)region)->open_tick != OPEN_TICK_CLOSED;
}

static void regiondata_mark_as_dirty(Py_region_t region) {
    // Invariant:
    ASSERT_IS_UNION_ROOT(region);

    // Regions without metadata are never dirty
    if (!HAS_DATA(region)) {
        return;
    }

    // Only open regions can be marked as dirty
    assert(regiondata_is_open(region));

    // Mark region as dirty
    regiondata* data = (regiondata*)region;
    data->open_tick = OPEM_TICK_DIRTY;
}

static int regiondata_is_dirty(Py_region_t region) {
    // Invariant:
    ASSERT_IS_UNION_ROOT(region);

    // Regions without metadata are never dirty
    if (!HAS_DATA(region)) {
        return false;
    }

    // Closed regions are always clean
    if (!regiondata_is_open(region)) {
        return false;
    }

    // Check if the region is open and already marked as dirty
    regiondata* data = (regiondata*)region;
    if (data->open_tick == OPEM_TICK_DIRTY) {
        return true;
    }

    // Check if untrusted code was called since this region was opened
    Py_ssize_t current_tick = _PyOwnership_get_current_tick();
    if (data->open_tick == current_tick) {
        return false;
    }

    // Set to dirty constant for quicker lookup
    data->open_tick = OPEM_TICK_DIRTY;

    return true;
}

/* This closes the region and propagates the status to the owner.
 *
 * This operation may fail if:
 * - The region is dirty (potentially caused by `_Py_ownership_state` being unavailable)
 * - Closing a parent region failed
 * - TODO: xFrednet: If the owing cown is released.
 *
 * The region might still be closed, if the error came from an owner.
 */
static int regiondata_close(Py_region_t region) {
    // Invariant:
    ASSERT_IS_UNION_ROOT(region);
    assert(regiondata_is_open(region));

    // Regions without metadata can't be closed
    if (!HAS_DATA(region)) {
        return 0;
    }

    // Dirty regions can't be closed
    if (regiondata_is_dirty(region)) {
        return 1;
    }

    // Mark the region as closed.
    regiondata *data = (regiondata*)region;
    data->open_tick = OPEN_TICK_CLOSED;

    // Notify the owner
    if (HAS_OWNER_TAG(region, OWNER_TAG_COWN)) {
        // TODO: xFrednet: Implement this branch
        assert(false);
    } else if (regiondata_get_parent(region) != 0) {
        return regiondata_close(regiondata_get_parent(region));
    }

    // Check for failure, which would leave the region closed
    return 0;
}

/* This uses the inner state of the region and closes it if possible.
 *
 * This can fail if the region gets closed, see `regiondata_close`.
 */
static int regiondata_check_close(Py_region_t region) {
    // Invariant:
    ASSERT_IS_UNION_ROOT(region);

    // Static regions can't be closed
    if (!HAS_DATA(region)) {
        return 0;
    }

    // Check if the region can currently be closed
    regiondata *data = (regiondata*)region;
    if (data->lrc == 0 && data->osc == 0 && !regiondata_is_dirty(region)) {
        // Propagate the result
        return regiondata_close(region);
    }

    // Nothing needs to be done, and everything is fine
    return 0;
}

/* This increases the local reference count.
 *
 * This might open this and parent regions, which can fail. See
 * `regiondata_open` for possible failures.
 * */
static int regiondata_inc_lrc(Py_region_t region) {
    // Invariant:
    ASSERT_IS_UNION_ROOT(region);

    // Static regions don't need to be updated
    if (!HAS_DATA(region)) {
        return 0;
    }

    // Attempt to mark the region as open
    if (regiondata_open(region)) {
        return 1;
    }

    // Update the LRC, once the region is open
    regiondata *data = (regiondata*)region;
    data->lrc += 1;

    return 0;
}

/* This decreases the local reference count.
 *
 * This might close this and parent regions, which can fail. See
 * `regiondata_close` for possible failures.
 * */
static int regiondata_dec_lrc(Py_region_t region) {
    // Invariant:
    ASSERT_IS_UNION_ROOT(region);

    // Static regions don't need to be updated
    if (!HAS_DATA(region)) {
        return 0;
    }

    // Update the OSC
    regiondata *data = (regiondata*)region;
    data->lrc -= 1;

    // Check the region state to determine if it should be closed.
    SUCCEEDS(regiondata_check_close(region));

    // Return 0 on success
    return 0;

error:
    // Undo the LRC decrement
    data->lrc += 1;

    // Propagate the failure information
    return 1;
}

/* This increases the open-subregion count. (This does not update RC)
 *
 * This might open this and parent regions, which can fail. See
 * `regiondata_open` for possible failures.
 * */
static int regiondata_inc_osc(Py_region_t region) {
    // Invariant:
    ASSERT_IS_UNION_ROOT(region);

    // Static regions don't need to be updated
    if (!HAS_DATA(region)) {
        return 0;
    }

    // Attempt to mark the region as open
    if (regiondata_open(region)) {
        return 1;
    }

    // Update the OSC, once the region is open
    regiondata *data = (regiondata*)region;
    data->osc += 1;

    return 0;
}

/* This decreases the open-subregion count. (This does not update RC)
 *
 * This might close this and parent regions, which can fail. See
 * `regiondata_close` for possible failures.
 * */
static int regiondata_dec_osc(Py_region_t region) {
    // Invariant:
    ASSERT_IS_UNION_ROOT(region);

    // Static regions don't need to be updated
    if (!HAS_DATA(region)) {
        return 0;
    }

    // Update the OSC
    regiondata *data = (regiondata*)region;
    data->osc -= 1;

    // Check the region state to determine if it should be closed.
    SUCCEEDS(regiondata_check_close(region));

    // Return 0 on success
    return 0;

error:
    // Undo the OSC decrement
    data->osc += 1;

    // Propagate the failure information
    return 1;
}

/* Setting the parent of an open region, might open the new parent region
 * and close the old parent region.
 *
 * This can fail, see `regiondata_open` and `regiondata_close` for possible
 * failures.
 * */
static int regiondata_set_parent(Py_region_t region, Py_region_t new_parent) {
    // Check invariant:
    assert(HAS_DATA(region));
    ASSERT_REGION_HAS_NO_TAG(new_parent);
    ASSERT_IS_UNION_ROOT(region);
    ASSERT_IS_UNION_ROOT(new_parent);
    assert(region != new_parent);
    ASSERT_REGION_HAS_NO_TAG(GET_OWNER_WITH_TAG(region));

    // Get the old parent
    regiondata* data = (regiondata*) region;
    Py_region_t old_parent = GET_OWNER_PTR(data);

    // Notify the parents, if this region is open.
    if (regiondata_is_open(region)) {
        if (regiondata_inc_osc(new_parent)) {
            return 1;
        }

        if (regiondata_dec_osc(old_parent)) {
            // Undo the inc_osc from above.
            regiondata_dec_osc(new_parent);
            return 1;
        }
    }

    // Only set the parent here, once all the failable operations are done
    data->owner = new_parent;
    regiondata_inc_rc(new_parent);
    regiondata_dec_rc(old_parent);

    return 0;
}

/* Returns the pointer to the parent region or 0 if the region doesn't have a
 * parent.
 */
static Py_region_t regiondata_get_parent(Py_region_t region) {
    // Invariant:
    ASSERT_IS_UNION_ROOT(region);

    // Static regions never have a parent
    if (HAS_DATA(region)) {
        return 0;
    }

    // Don't return the owner, if it's a cown
    if (HAS_OWNER_TAG(region, OWNER_TAG_COWN)) {
        return 0;
    }

    // Get the parent
    Py_region_t parent_field = GET_OWNER_PTR(region);
    Py_region_t parent_root = regiondata_union_root(parent_field);

    // If the parent was merged with another region we want to update the
    // owner to point at the root.
    if (parent_field != parent_root) {
        regiondata* data = (regiondata*) region;
        data->owner = parent_root;
        regiondata_inc_rc(parent_root);
        regiondata_dec_rc(parent_field);
    }

    // Get the root of the parent
    return parent_root;
}

/* Returns `true` if the given region has a parent
 */
static bool regiondata_has_parent(Py_region_t region) {
    return regiondata_get_parent(region) != 0;
}

/* Returns true, if `other` is an ancestor of `region`.
 */
static bool regiondata_is_ancestor(Py_region_t region, Py_region_t ancestor) {
    // Invariant:
    ASSERT_IS_UNION_ROOT(region);
    ASSERT_IS_UNION_ROOT(ancestor);
    ASSERT_REGION_HAS_NO_TAG(ancestor);

    // Static regions never have parents
    if (!HAS_DATA(region)) {
        return false;
    }

    // Static regions are never parents
    if (!HAS_DATA(ancestor)) {
        return false;
    }

    // Walk the ancestor tree until the root
    while (region) {
        if (region == ancestor) {
            return true;
        }

        region = regiondata_get_parent(region);
    }

    return false;
}

/* Returns true if the given object is the bridge of the given region
 */
static bool regiondata_is_bridge(Py_region_t region, PyObject *obj) {
    // Invariant:
    ASSERT_IS_UNION_ROOT(region);
    assert(obj != NULL);

    // Static regions have no brigde objects
    if (!HAS_DATA(region)) {
        return false;
    }

    regiondata *data = (regiondata*)region;

    return data->bridge == obj;
}

/* Sets the region of the object to the newly given region.
 *
 * This will just update the RC of the old and new region, all other state,
 * like the LRC, has to be updated separatly.
 */
static void PyObject_SetRegion(PyObject* obj, Py_region_t new_region) {
    // Invariant:
    assert(obj);
    ASSERT_IS_UNION_ROOT(new_region);
    ASSERT_REGION_HAS_NO_TAG(new_region);

    // Update the region and region rc
    Py_region_t old_region = obj->ob_region;
    obj->ob_region = new_region;
    regiondata_inc_rc(new_region);
    regiondata_dec_rc(old_region);
}

// Add the transitive closure of objects in the local region reachable from obj to region
// static PyObject *add_to_region(PyObject *obj, Py_region_ptr_t region) {}
typedef struct AddRegionState {
    Py_region_t merge_region;
    Py_region_t subject_region;
} AddRegionState;

static
int _add_to_region_check_obj(PyObject *obj, void *state_void) {
    // AddRegionState *state = (AddRegionState*)state_void;

    // Py_region_t obj_region = _PyRegion_Get(obj);

    // // Skip the object, if it's already part of the merge region
    // if (obj_region == state->merge_region) {
    //     return Py_OWNERSHIP_TRAVERSE_SKIP;
    // }

    // // Add the object to the merge region, this will also prevent it
    // // from being traversed again.
    // PyObject_SetRegion(obj, state->merge_region);

    // Sanity Check, all objects given to this function should be in the
    // merge region
    assert(_PyRegion_Get(obj) == ((AddRegionState*)state_void)->merge_region);

    // `_add_to_region_visit` already does the filtering and ensures that only
    // new objects are traversed. This is therefore a no-op indicateing that
    // the object should be traversed.
    return Py_OWNERSHIP_TRAVERSE_VISIT;
}

static
int _add_to_region_visit(PyObject *src, PyObject *tgt, void *state_void) {
    AddRegionState *state = (AddRegionState*)state_void;

    Py_region_t tgt_region = _PyRegion_Get(tgt);

    // These regerences are allowed and should not be followed
    if (IS_IMMUTABLE_REGION(tgt_region) || IS_COWN_REGION(tgt_region)) {
        return Py_OWNERSHIP_TRAVERSE_SKIP;
    }

    regiondata *merge_data = (regiondata*)state->merge_region;

    // Take ownership of local objects
    if (IS_LOCAL_REGION(tgt_region)) {
        // Add incoming references to the LRC
        // -1 for the reference this call came from
        //
        // FIXME(regions): xFrednet: Handle weak references
        merge_data->lrc += Py_REFCNT(tgt) - 1;

        // Add the object to the merge region, this will also prevent it
        // from being traversed again.
        PyObject_SetRegion(tgt, state->merge_region);

        // FIXME(regions): xFrednet: Handle RC of immortal objects
        assert(!_Py_IsImmortal(tgt));

        // Return and notify that `tgt` should also be traversed
        return Py_OWNERSHIP_TRAVERSE_VISIT;
    }

    // The target was previously in the local region but has already been
    // added to the merge region by a previous iteration. This therefore only
    // adjusts the LRC
    if (tgt_region == state->merge_region || tgt_region == state->subject_region) {
        // The LRC of the merge region can go negative by this operation as
        // this also includes references which should be subtract from the
        // LRC of the subject region.
        merge_data->lrc -= 1;

        // The object should not be traversed.
        return Py_OWNERSHIP_TRAVERSE_SKIP;
    }

    // At this point, we know that target is in another region.
    // If target is in a different region, it has to be a bridge object.
    // References to contained objects are forbidden.
    if (!regiondata_is_bridge(tgt_region, tgt)) {
        // TODO: Better error message
        throw_region_error("References to objects in other regions are forbidden", Py_None, src, tgt);

        return Py_OWNERSHIP_TRAVERSE_ERR;
    }

    // The target is a bridge object from another region. This is allowed, if
    // the region doesn't have a parent
    if (regiondata_has_parent(tgt_region)) {
        // TODO: Better error message
        throw_region_error("Regions are not allowed to have multiple parents", Py_None, src, tgt);

        return Py_OWNERSHIP_TRAVERSE_ERR;
    }

    if (regiondata_is_ancestor(state->subject_region, tgt_region)) {
        // TODO: Better error message
        throw_region_error("Regions are not allowed to create cycles in the ancestor tree", Py_None, src, tgt);

        return Py_OWNERSHIP_TRAVERSE_ERR;
    }

    // From the previous checks it is know that `tgt` is the bridge object
    // of a free region. Thus we can make it a sub region and allow the
    // reference.
    //
    // `regiondata_set_parent` will also ensure that the `osc` is updated.
    regiondata_set_parent(tgt_region, state->merge_region);

    // The object reference was accepted, but the target should not be traversed
    return Py_OWNERSHIP_TRAVERSE_SKIP;
}

// Main entry point to freeze an object and everything it can reach.
int _add_to_region(PyObject* obj, Py_region_t subject_region)
{
    // Invariant:
    ASSERT_IS_UNION_ROOT(subject_region);

    // Trivial Accept
    if (_PyRegion_Get(obj) == subject_region) {
        return 0;
    }

    int result = 0;

    // Initialize the state
    AddRegionState add_state;
    add_state.subject_region = subject_region;
    add_state.merge_region = regiondata_new();
    if (add_state.merge_region) {
        PyErr_NoMemory();
        goto error;
    }

    // Manually call visit with `obj` as the target to ensure that it is
    // correctly added to the merge region or throws an error
    result = _add_to_region_visit(NULL, obj, (void*)&add_state);

    switch (result)
    {
    case Py_OWNERSHIP_TRAVERSE_VISIT:
        // Traverse the object graph
        SUCCEEDS(_PyOwnership_traverse_object_graph(obj, _add_to_region_check_obj, _add_to_region_visit, (void*)&add_state));
    case Py_OWNERSHIP_TRAVERSE_SKIP:
        // Indicate success
        result = 0;
        break;
    default:
        goto error;
    }

    // Merge the region into the subject region since all objects could be added
    SUCCEEDS(regiondata_union_merge(add_state.merge_region, subject_region));
    goto finally;

error:
    // Merge the region into local, to undo any ownership changes
    regiondata_union_merge(add_state.merge_region, _Py_LOCAL_REGION);
    result = -1;

finally:
    regiondata_dec_rc(add_state.merge_region);
    return result;
}

/* ====================================
 * Exported functions
 * ====================================
 */


/* Returns the region of the given object. This is the slow path of `_PyRegion_`.
 *
 * This function can't be inlined as it requires additional metadata to check
 * if the region of the object was merged with another one.
 */
Py_region_t _PyRegion_GetSlow(PyObject *obj) {
    Py_region_t region = regiondata_union_root(obj->ob_region);

    // Check if the region should be updated, this can happen if the object
    // region was merged into another region.
    if (obj->ob_region != region) {
        PyObject_SetRegion(obj, region);
    }

    return region;
}

/* Returns true, if the given region is marked as dirty
 */
int _PyRegion_IsDirty(Py_region_t region) {
    return regiondata_is_dirty(region);
}

int _PyRegion_IsParent(Py_region_t child, Py_region_t parent) {
    return regiondata_get_parent(child) == parent;
}

/* Returns the bridge object belonging to the region of the given object.
 */
PyObject* _PyRegion_GetBridge(PyObject *obj) {
    Py_region_t region = _PyRegion_Get(obj);

    // Regions without data don't have a bridge
    if (!HAS_DATA(region)) {
        // Return None, since NULL would indicate an exception
        Py_RETURN_NONE;
    }

    regiondata *data = (regiondata*)region;
    return data->bridge;
}

/* Notifys the contianing region that the given object is now immutable.
 * This will mark the previously owning region as dirty as the LRC or OSC
 * might be invalidated by this move.
 *
 * This function can fail, if the move closes a parent region. See
 * `regiondata_close` for possible failures.
 */
int _PyRegion_SignalImmutable(PyObject *obj) {
    Py_region_t region = _PyRegion_Get(obj);

    // Moving an object from a static region is trivial
    if (!HAS_DATA(region)) {
        PyObject_SetRegion(obj, _Py_IMMUTABLE_REGION);
        return 0;
    }

    if (regiondata_is_bridge(region, obj)) {
        // Freezing the brigde object might invalidate the OSC of the parent.
        // Ideally, we could just unparent the region to prevent the dirty
        // mark, but freezing might fail. And if it fails, we would want to
        // reconstruct the region and keep the parent relationship.
        regiondata_mark_as_dirty(regiondata_get_parent(region));
    }

    // The moved object might have been referenced from the local region
    // or reference the bridge of another region. This region change
    // therefore invalidates the LRC and OSC of the region. It's marked
    // as dirty, and these counts are only reestablished when needed.
    regiondata_mark_as_dirty(region);

    return 0;
}

/* Checks if a reference from `src` to `tgt` is allowed and updates the
 * internal region state accordingly.
 *
 * Returns 0 on success.
 */
int _PyRegion_AddRef(PyObject *src, PyObject *tgt) {
    // FIXME(regions): xFrednet: It might be worth to put the fast path into
    // the header and allow inlining

    Py_region_t src_region = _PyRegion_Get(src);
    Py_region_t tgt_region = _PyRegion_Get(tgt);

    if (src_region == tgt_region) {
        // Intra-region references are always permitted and not tracket
        return 0;
    }

    if (IS_IMMUTABLE_REGION(tgt_region) || IS_COWN_REGION(tgt_region)) {
        // References to immutable objects or cowns are always permitted
        return 0;
    }

    if (IS_LOCAL_REGION(src_region)) {
        // References from the local region are allowed, but need to be registered
        return regiondata_inc_lrc(tgt_region);
    }

    // Attempt to slurp the target object into the source region
    return _add_to_region(tgt, src_region);
}

/* Removes the reference from `src` to `tgt` and updates the internal state of
 * the regions.
 *
 * Returns 0 on success.
 */
int _PyRegion_RemoveRef(PyObject *src, PyObject *tgt) {
    Py_region_t src_region = _PyRegion_Get(src);
    Py_region_t tgt_region = _PyRegion_Get(tgt);

    if (src_region == tgt_region) {
        // Intra-region references are always permitted and not tracket
        return 0;
    }

    if (IS_IMMUTABLE_REGION(tgt_region) || IS_COWN_REGION(tgt_region)) {
        // References to immutable objects or cowns are always permitted
        return 0;
    }

    if (IS_LOCAL_REGION(src_region)) {
        // Decrease the target region LRC since this reference came from
        // the local region
        return regiondata_dec_lrc(tgt_region);
    }

    if (regiondata_is_bridge(tgt_region, tgt)
        && regiondata_get_parent(tgt_region) == src_region
    ) {
        // The removed reference was the owning references. The target region
        // gets unparented and is now free.
        return regiondata_set_parent(tgt_region, NULL_REGION);
    } else {
        // The reference came from `src` to `tgt` while the target region
        // already had a parent. This is not allowed but can happend in
        // unaware code. The two regions therefore have to be marked as dirty
        assert(regiondata_is_dirty(src_region));
        assert(regiondata_is_dirty(tgt_region));

        // The two regions are marked as dirty. This is an additional safety net
        // for builds without asserts.
        regiondata_mark_as_dirty(src_region);
        regiondata_mark_as_dirty(tgt_region);

        // Still return 0, since the reference could be should be removed.
        return 0;
    }
}

int _PyRegion_AddLocalRef(PyObject *tgt) {
    return regiondata_inc_lrc(_PyRegion_Get(tgt));
}

int _PyRegion_RemoveLocalRef(PyObject *tgt) {
    return regiondata_dec_lrc(_PyRegion_Get(tgt));
}

// TODO(regions): xFrednet: PyRegionObject
// TODO(regions): xFrednet: Write Barrier in: Bytecode
// TODO(regions): xFrednet: Write Barrier in: Dictionary
// TODO(regions): xFrednet: Dirty on C code
// TODO(regions): xFrednet: Cowns
// TODO(regions): xFrednet: Weak Region Reference
