// Implementation of biased reference counting inter-thread queue.
//
// Biased reference counting maintains two refcount fields in each object:
// ob_ref_local and ob_ref_shared. The true refcount is the sum of these two
// fields. In some cases, when refcounting operations are split across threads,
// the ob_ref_shared field can be negative (although the total refcount must
// be at least zero). In this case, the thread that decremented the refcount
// requests that the owning thread give up ownership and merge the refcount
// fields. This file implements the mechanism for doing so.
//
// Each thread state maintains a queue of objects whose refcounts it should
// merge. The thread states are stored in a per-interpreter hash table by
// thread id. The hash table has a fixed size and uses a linked list to store
// thread states within each bucket.
//
// The queueing thread uses the eval breaker mechanism to notify the owning
// thread that it has objects to merge. Additionally, all queued objects are
// merged during GC.
#include "Python.h"
#include "pycore_object.h"      // _Py_ExplicitMergeRefcount
#include "pycore_brc.h"         // struct _brc_thread_state
#include "pycore_ceval.h"       // _Py_set_eval_breaker_bit
#include "pycore_llist.h"       // struct llist_node
#include "pycore_pystate.h"     // _PyThreadStateImpl

#ifdef Py_GIL_DISABLED

// Get the hashtable bucket for a given thread id.
static struct _brc_bucket *
get_bucket(PyInterpreterState *interp, uintptr_t tid)
{
    return &interp->brc.table[tid % _Py_BRC_NUM_BUCKETS];
}

// Find the thread state in a hash table bucket by thread id.
static _PyThreadStateImpl *
find_thread_state(struct _brc_bucket *bucket, uintptr_t thread_id)
{
    struct llist_node *node;
    llist_for_each(node, &bucket->root) {
        // Get the containing _PyThreadStateImpl from the linked-list node.
        _PyThreadStateImpl *ts = llist_data(node, _PyThreadStateImpl,
                                            brc.bucket_node);
        if (ts->brc.tid == thread_id) {
            return ts;
        }
    }
    return NULL;
}

// Merge the refcounts of all objects in `stack` that are still owned by
// `tid`, keeping the queue's reference.
static void
merge_queued_refcounts(_PyObjectStack *stack, uintptr_t tid)
{
    for (_PyObjectStackChunk *buf = stack->head; buf != NULL; buf = buf->prev) {
        for (Py_ssize_t i = 0; i < buf->n; i++) {
            PyObject *ob = buf->objs[i];
            // The owner may have handed the object off after it was queued.
            if (_Py_atomic_load_uintptr(&ob->ob_tid) == tid) {
                _Py_ExplicitMergeRefcount(ob, 0);
            }
        }
    }
}

// Release the queue's reference to each object. This may run destructors,
// so the bucket mutex must not be held.
static void
decref_merged_objects(_PyObjectStack *stack)
{
    PyObject *ob;
    while ((ob = _PyObjectStack_Pop(stack)) != NULL) {
        // Merged objects have `ob_tid == 0`, which makes this a Py_DECREF.
        // Skipped objects are forwarded to their new owner.
        _Py_brc_queue_object(ob);
    }
}

// Enqueue an object to be merged by the owning thread. This steals a
// reference to the object.
void
_Py_brc_queue_object(PyObject *ob)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();

    uintptr_t ob_tid = _Py_atomic_load_uintptr(&ob->ob_tid);
    while (ob_tid == _Py_FREE_TID || ob_tid == _Py_ThreadId()) {
        if (_Py_IsOwnedByCurrentThreadDecRef(ob)) {
            // A plain Py_DECREF would leave `_Py_REF_QUEUED` set, which stops
            // any future owner from being asked to merge. Merging clears it.
            Py_ssize_t refcount = _Py_ExplicitMergeRefcount(ob, -1);
            if (refcount == 0) {
                _Py_Dealloc(ob);
            }
            return;
        }
        // Another thread claimed it first.
        ob_tid = _Py_atomic_load_uintptr(&ob->ob_tid);
    }
    if (ob_tid == 0) {
        // The owning thread may have concurrently decided to merge the
        // refcount fields.
        Py_DECREF(ob);
        return;
    }

    struct _brc_bucket *bucket = get_bucket(interp, ob_tid);
    PyMutex_Lock(&bucket->mutex);
    _PyThreadStateImpl *tstate = find_thread_state(bucket, ob_tid);
    if (tstate == NULL) {
        // If we didn't find the owning thread then it must have already exited.
        // It's safe (and necessary) to merge the refcount. Subtract one when
        // merging because we've stolen a reference.
        Py_ssize_t refcount = _Py_ExplicitMergeRefcount(ob, -1);
        PyMutex_Unlock(&bucket->mutex);
        if (refcount == 0) {
            _Py_Dealloc(ob);
        }
        return;
    }

    if (_PyObjectStack_Push(&tstate->brc.objects_to_merge, ob) < 0) {
        PyMutex_Unlock(&bucket->mutex);

        // Fall back to stopping all threads and manually merging the refcount
        // if we can't enqueue the object to be merged.
        _PyEval_StopTheWorld(interp);
        Py_ssize_t refcount = _Py_ExplicitMergeRefcount(ob, -1);
        _PyEval_StartTheWorld(interp);

        if (refcount == 0) {
            _Py_Dealloc(ob);
        }
        return;
    }

    if (_PyThreadState_TrySuspendDetached(&tstate->base)) {
        // The owning thread is detached (e.g. blocked on a lock or in a
        // system call) and may not run Python code again for a long time,
        // so merge its queue on its behalf instead of waiting for it. While
        // it is held in the "suspended" state it cannot attach and therefore
        // cannot touch ob_ref_local or ob_tid.
        _PyObjectStack merged = {0};
        _PyObjectStack_Merge(&merged, &tstate->brc.objects_to_merge);
        merge_queued_refcounts(&merged, ob_tid);
        _PyThreadState_ResumeDetached(&tstate->base);
        PyMutex_Unlock(&bucket->mutex);

        decref_merged_objects(&merged);
        return;
    }

    // Notify owning thread
    _Py_set_eval_breaker_bit(&tstate->base, _PY_EVAL_EXPLICIT_MERGE_BIT);

    PyMutex_Unlock(&bucket->mutex);
}

// `tid` owns the queue. It is not the current thread when a thread state is
// removed on behalf of a thread that is gone (e.g. after fork).
static void
merge_queued_objects(_PyObjectStack *to_merge, uintptr_t tid)
{
    PyObject *ob;
    while ((ob = _PyObjectStack_Pop(to_merge)) != NULL) {
        if (_Py_atomic_load_uintptr(&ob->ob_tid) != tid) {
            // Ownership was handed off after the object was queued, so its
            // `ob_ref_local` is no longer ours to merge.
            _Py_brc_queue_object(ob);
            continue;
        }
        // Subtract one when merging because the queue had a reference.
        Py_ssize_t refcount = _Py_ExplicitMergeRefcount(ob, -1);
        if (refcount == 0) {
            _Py_Dealloc(ob);
        }
    }
}

// Process this thread's queue of objects to merge.
void
_Py_brc_merge_refcounts(PyThreadState *tstate)
{
    struct _brc_thread_state *brc = &((_PyThreadStateImpl *)tstate)->brc;
    struct _brc_bucket *bucket = get_bucket(tstate->interp, brc->tid);

    assert(brc->tid == _Py_ThreadId());

    // Append all objects into a local stack. We don't want to hold the lock
    // while calling destructors.
    PyMutex_Lock(&bucket->mutex);
    _PyObjectStack_Merge(&brc->local_objects_to_merge, &brc->objects_to_merge);
    PyMutex_Unlock(&bucket->mutex);

    // Process the local stack until it's empty
    merge_queued_objects(&brc->local_objects_to_merge, brc->tid);
}

// Merge the queue of the thread that owns `tid` on its behalf, if that
// thread is detached. An owner that was attached when objects were queued
// may detach (e.g. block on a lock) before its eval breaker drains them.
void
_Py_brc_merge_for_detached_owner(uintptr_t tid)
{
    assert(tid != _Py_ThreadId());
    PyInterpreterState *interp = _PyInterpreterState_GET();
    struct _brc_bucket *bucket = get_bucket(interp, tid);
    _PyObjectStack merged = {0};

    PyMutex_Lock(&bucket->mutex);
    _PyThreadStateImpl *tstate = find_thread_state(bucket, tid);
    if (tstate != NULL && tstate->brc.objects_to_merge.head != NULL
        && _PyThreadState_TrySuspendDetached(&tstate->base))
    {
        _PyObjectStack_Merge(&merged, &tstate->brc.objects_to_merge);
        merge_queued_refcounts(&merged, tid);
        _PyThreadState_ResumeDetached(&tstate->base);
    }
    PyMutex_Unlock(&bucket->mutex);

    decref_merged_objects(&merged);
}

void
_Py_brc_init_state(PyInterpreterState *interp)
{
    struct _brc_state *brc = &interp->brc;
    for (Py_ssize_t i = 0; i < _Py_BRC_NUM_BUCKETS; i++) {
        llist_init(&brc->table[i].root);
    }
}

void
_Py_brc_init_thread(PyThreadState *tstate)
{
    struct _brc_thread_state *brc = &((_PyThreadStateImpl *)tstate)->brc;
    uintptr_t tid = _Py_ThreadId();

    // Add ourself to the hashtable
    struct _brc_bucket *bucket = get_bucket(tstate->interp, tid);
    PyMutex_Lock(&bucket->mutex);
    brc->tid = tid;
    llist_insert_tail(&bucket->root, &brc->bucket_node);
    PyMutex_Unlock(&bucket->mutex);
}

void
_Py_brc_remove_thread(PyThreadState *tstate)
{
    struct _brc_thread_state *brc = &((_PyThreadStateImpl *)tstate)->brc;
    if (brc->tid == 0) {
        // The thread state may have been created, but never bound to a native
        // thread and therefore never added to the hashtable.
        assert(tstate->_status.bound == 0);
        return;
    }

    struct _brc_bucket *bucket = get_bucket(tstate->interp, brc->tid);

    // We need to fully process any objects to merge before removing ourself
    // from the hashtable. It is not safe to perform any refcount operations
    // after we are removed. After that point, other threads treat our objects
    // as abandoned and may merge the objects' refcounts directly.
    bool empty = false;
    while (!empty) {
        // Process the local stack until it's empty
        merge_queued_objects(&brc->local_objects_to_merge, brc->tid);

        PyMutex_Lock(&bucket->mutex);
        empty = (brc->objects_to_merge.head == NULL);
        if (empty) {
            llist_remove(&brc->bucket_node);
        }
        else {
            _PyObjectStack_Merge(&brc->local_objects_to_merge,
                                 &brc->objects_to_merge);
        }
        PyMutex_Unlock(&bucket->mutex);
    }

    assert(brc->local_objects_to_merge.head == NULL);
    assert(brc->objects_to_merge.head == NULL);
}

void
_Py_brc_after_fork(PyInterpreterState *interp)
{
    // Unlock all bucket mutexes. Some of the buckets may be locked because
    // locks can be handed off to a parked thread (see lock.c). We don't have
    // to worry about consistency here, because no thread can be actively
    // modifying a bucket, but it might be paused (not yet woken up) on a
    // PyMutex_Lock while holding that lock.
    for (Py_ssize_t i = 0; i < _Py_BRC_NUM_BUCKETS; i++) {
        _PyMutex_at_fork_reinit(&interp->brc.table[i].mutex);
    }
}

#endif  /* Py_GIL_DISABLED */
