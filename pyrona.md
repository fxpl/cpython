# Lungfish Region Model — Conceptual Reference

This document is written to be supplied as context to an LLM (such as Claude
or Copilot) that is assisting with work on Pyrona, the CPython implementation
of Lungfish. It describes what the region model is and why it exists. Write
barrier implementation details — the concrete C API and how to insert it —
are covered in a separate document.

This document is sufficient for reasoning about the ownership model,
reference legality, region topology, closedness, and high-level safety
guarantees. It is not the implementation reference for write barriers,
runtime APIs, exact exception types, object headers, or C-level integration.
Where those details matter, defer to the implementation rather than inferring
them from this document.

---

## The Problem

Python has historically run as a single thread under a global interpreter
lock (GIL), which serialises all bytecode execution. To achieve real
parallelism, Python (via PEP 734) now supports multiple sub-interpreters,
each with its own GIL, running in the same process. But sub-interpreters
are fully isolated: to share data between them, objects must currently be
serialised (pickled) and copied, which is slow.

The underlying obstacle is shared mutable state. If two sub-interpreters
could directly reference the same mutable object, concurrent access would
cause data races — and because each sub-interpreter manages its own memory
with non-atomic reference counting, such races could corrupt the runtime
itself, not just user data.

Lungfish removes this obstacle. It introduces an ownership discipline that
lets objects be moved or shared across sub-interpreters safely — directly,
without serialisation — by guaranteeing that mutable objects are never
reachable from more than one sub-interpreter at a time. Data races become
structurally impossible rather than something programmers must prevent by
adding synchronisation correctly.

---

## Core Idea

Lungfish partitions all objects into groups called **regions**. At any point
in time, a region is owned by at most one sub-interpreter. Because only the
owning sub-interpreter can access the region's objects, those objects can be
read and written without synchronisation — they permit sequential reasoning.

To share data between sub-interpreters, Lungfish provides exactly two safe
mechanisms:

1. **Share** an immutable (frozen) object freely across sub-interpreters
2. **Coordinate** access to a mutable region through a cown

---

## Object Categories

Every object in Lungfish belongs to exactly one of five categories:

### Local objects
All newly allocated objects start here. The local region is an implicit
region associated with the current sub-interpreter. It contains the
sub-interpreter's stack and local frame, and all newly created heap objects.

Local region ownership is **ephemeral**: as soon as a persistent reference
to one of its objects is stored into another (non-local) region, ownership
of that object transfers automatically into the referencing region. (Merely
passing an object as an argument or observing it does not transfer it — only
storing a reference into another region does.) Any reference still held from
the local region to the now-moved object becomes a **borrowed reference**
(see Reference Types below) — the local-scope variable remains valid but is
now a borrow rather than an owning reference.

Because all newly allocated objects begin in the local region, existing
*sequential* (single interpreter) Python code works without modification — it
operates entirely within the local region and never triggers a transfer.
Only code that deliberately shares mutable state across sub-interpreters is
constrained by the ownership model, and that code may need changes.

### Contained objects
Mutable objects that have been moved into an explicitly created region (one
created via `Region()`, as opposed to the implicit local region). They are
encapsulated inside the region and can only be accessed by the
sub-interpreter that currently owns the region.

### Bridge objects
Like contained objects, but they additionally permit a single incoming
reference from outside their enclosing region. The bridge object is the
**handle** to a region — it is the runtime object that represents the region
and acts as an entry point to its contents.

Bridge objects are **externally unique**: at most one *owning* reference to
a bridge object may originate from outside its own region. Borrowed
references from the local region do not count toward this limit — a bridge
object may simultaneously have its single external owning reference and any
number of borrows from the local region. External uniqueness counts only
owning references; local borrowed references do not create additional owners
and do not violate uniqueness. This uniqueness property (over owning
references only) is what allows safe ownership transfer: moving the one
owning reference moves the entire region.

### Immutable objects
Objects that have been frozen. Freezing is deep: the entire transitive
closure of mutable state reachable from the frozen object becomes immutable.
Immutable objects can be freely shared across sub-interpreters and regions
without synchronisation, because they cannot change.

Because freezing is deep, an immutable object can only reference other
immutable objects (or cowns, which mediate their own access). It can never
reference a mutable bridge or contained object — such a reference would let
the immutable object observe mutation, violating immutability. This is why
the constraint table forbids immutable → bridge and immutable → contained.

### Cowns (concurrent owners)
Cells that hold a reference to a bridge object (or another cown or immutable
object). A cown exposes an `acquire` and a `release` operation. Its contents
cannot be accessed until the cown is acquired; acquisition grants exclusive
access to the contents for the duration of the acquisition block, and
`release` ends that access.

Release requires the **held region** (the region whose bridge object the
cown stores — not to be confused with a "contained object") to be closed.
Attempting to release while the held region is still open raises an
exception. This is what guarantees no borrows escape the acquisition block.

(A cown may hold another cown. Reaching the inner cown means acquiring the
outer cown, accessing the inner one, and acquiring it in turn. The detailed
semantics of nested cowns are out of scope for this conceptual document.)

Cowns are the second sharing mechanism (alongside immutable objects): they
allow mutable state to be shared between sub-interpreters, but accessed by
only one at a time.

---

## Reference Types

Lungfish distinguishes three kinds of references:

### Borrowed references
References from the local region into another region. Because the local
region has ephemeral ownership, these references are temporary — they track
that a sub-interpreter is currently "looking into" a region but does not own
it persistently. Borrowed references are tracked by the **Local Reference
Count (LRC)** of the target region.

Local variables live in the local region (they are part of the
sub-interpreter's stack and local frame). So when a local variable points
*into another region*, that reference is a borrow. (A local variable
pointing to another local object is an ordinary intra-local-region
reference, not a borrow — borrows are specifically local-to-other-region
references.)

### External references
References from one non-local region into another, targeting a bridge
object. These must satisfy external uniqueness — there can be at most one
such reference per bridge object from outside its region. This is what
makes the region topology a forest (a collection of trees, not an arbitrary
graph).

### Shared references
References to immutable objects or cowns. These are always permitted from
anywhere, because immutable objects cannot change and cowns mediate their
own access.

---

## Region Topology

Regions form a **forest of trees**:

- Each region has at most one parent region (the region that holds the
  external owning reference — the "owning pointer" — to its bridge object)
- A region with no parent is a **root region** (or **free region**)
- A region whose bridge object is stored in a cown is also a root: the cown
  holds the owning pointer, so the cown becomes the root of that region. The
  cown is not itself a region and does not participate in the region tree as
  a parent — it simply holds the owning reference.

This tree structure is what makes ownership transfer efficient. To move
a closed region to another sub-interpreter, only the single reference to its
bridge object needs to move. The entire transitive closure of state under it
moves implicitly.

Cycles in the region tree are prevented at the point a region is nested: a
region that already has a parent cannot be captured again (attempting to do
so raises an exception), and the nesting operation checks that it would not
introduce a cycle in the region topology.

---

## Closedness

**Closed is a computed status, not an operation.** A region is **open** if
it is not closed. A region is closed when all three of the following hold:
- Its LRC (Local Reference Count) is 0 — no borrowed references from the
  local region point into it,
- Its OSC (Open Sub-Region Count) is 0 — none of its direct child regions
  are open, and
- It is not **dirty** (see below).

A dirty region is open by definition — dirtiness is one of the ways a region
can be open, alongside having live borrows or open descendants.

There is no `close()` call a programmer makes. Closedness is recomputed as
references come and go. It is also not directly observable: to look at a
region at all you must hold a reference to it on the stack, and that
reference is itself a borrow that opens the region. What *is* observable is
indirect — a parent region's OSC dropping, or a cown becoming releasable.

A region with live borrows cannot become closed, and therefore cannot be
transferred or released from a cown. This is intentional: the borrow itself
keeps the region open, which is the safety guarantee.

### LRC, OSC, and propagation

Each region carries two counters. The LRC counts borrows into the region
itself. The OSC counts how many of its direct child regions are currently
open.

When a child region transitions between open and closed, its parent's OSC is
updated by one — the update happens on the transition, not on every
individual borrow. A child becomes open when its LRC goes 0→1, when its own
OSC goes 0→1, or when it becomes dirty; it becomes closed again only when all
three conditions clear. Because each open/closed transition propagates to the
parent's OSC, openness propagates all the way up the tree: a parent cannot be
closed while any descendant is open or dirty.

### Dirty regions

A region can have LRC = 0 and OSC = 0 and still be open, because it is
**dirty**. A dirty region's bookkeeping can no longer be trusted to reflect
its true borrow state. It must be cleaned — via the `clean()` method on its
bridge object, which reestablishes the invariant — before it can become
closed. Because a dirty region counts as open, marking a region dirty also
increments its parent's OSC, exactly as a borrow would.

A region becomes dirty in two cases:
1. **Freezing an object inside it.** Freezing moves the frozen object and its
   transitive closure out of the region. The region that owned those objects
   is marked dirty, because the runtime cannot cheaply determine how to
   adjust the LRC for the references that left.
2. **Calling non-migrated C code** (C extensions not instrumented with
   Pyrona's write barriers). When execution enters such code, all currently
   open regions are marked dirty, because that code may have created or
   destroyed references without going through the write barriers.

Closedness is the precondition for safe transfer and safe cown release.
A closed region is dominated by the single owning reference to its bridge
object, so the holder of that reference has exclusive access to everything
in the region and its closed children.

---

## Key Operations

### Region creation
```python
x = Region()   # creates a new bridge object for a fresh region
```
New regions start as root regions (no parent).

### Object transfer (implicit)
When a local object is assigned as a field of a contained or bridge object,
it transfers into that region automatically. The transfer is transitive —
all local objects reachable from the transferred object move with it.
The transfer fails (raising an exception) if, while following reachable
objects, it encounters a reference to a non-bridge object that already
belongs to some other region (any region that is neither the local region
nor the target region the objects are moving into). Such a reference would
break region isolation.

If the referenced object is a bridge object of a region that has no parent
(a root/free region), the transfer succeeds and that region becomes nested
as a child of the target region. If the referenced bridge object already has
a parent, capturing it again would violate external uniqueness, so the
transfer fails with an exception rather than silently re-parenting it.

The reverse is automatic: when the owning reference to a child region's
bridge object is dropped (for example by assigning `None` over it), the child
region's parent pointer is cleared and it becomes a root (free) region again.
Un-nesting requires no explicit operation — removing the owning reference is
what frees the region.

### Borrowing
References from the local region to a region's objects are borrows. They
are permitted freely but increase the LRC of the target region, preventing
it from being closed while the borrow exists. When a borrow opens a region
(its LRC goes 0→1), its parent region's OSC is incremented, so the open state
propagates up the region tree — a parent cannot be closed while any
descendant is open (see Closedness).

### Freezing

```python
freeze(x)   # makes x and all transitively reachable mutable state immutable
```

Freezing is independent of the region topology — any object can be frozen,
whether it is local or inside a region. Freezing makes the target and
everything reachable from it deeply immutable. Frozen objects no longer need
a region, because immutable objects are safe to share across sub-interpreters
without exclusive access. (As noted under Closedness, freezing an object that
lived inside a region marks that region dirty, because references leaving the
region during the freeze require the region's bookkeeping to be
reestablished.)

Freezing follows references through bridge objects too. If the frozen
closure reaches a bridge object, that bridge and all objects contained in
its region are frozen as well, recursively through any further nested
regions. A frozen bridge is no longer a bridge — it becomes an ordinary
immutable object, and its region ceases to exist.

### Coordinating access through a cown
```python
r = Region()
c = Cown(r)        # c holds r; r becomes the held region, c is its root

with c:            # acquire: exclusive access to r for the block
    ...            # read/write objects in r
                   # release on block exit; requires r to be closed,
                   # otherwise an exception is raised
```
Acquiring a cown grants exclusive access to the held region for the duration
of the block. Releasing requires the held region to be closed.

### Cleaning a dirty region
```python
b.clean()          # b is a bridge object; reestablishes the region invariant
```
`clean()` is a method on a bridge object. It recomputes the region's borrow
bookkeeping so a dirty region can become closed again. It is needed after the
region has been marked dirty (see Dirty regions).


## The Reference Constraint Table

This table is checked for **every** reference as it is created. Rows are the
source object category; columns are the target object category.

| Source → Target | local | immutable | bridge | contained | cown |
|-----------------|-------|-----------|--------|-----------|------|
| local           | ✓     | ✓         | ✓ (borrowed) | ✓ (borrowed) | ✓ |
| immutable       | ✗     | ✓         | ✗      | ✗         | ✓ |
| bridge          | → (transfer) | ✓  | ✓ (E∨U) | ✓ (E)   | ✓ |
| contained       | → (transfer) | ✓  | ✓ (E∨U) | ✓ (E)   | ✓ |
| cown            | ✗     | ✓         | ✓ (U)  | ✗         | ✓ |

Annotations:
- **→** triggers implicit transfer of ownership
- **E** requires source and target to be in the same region
- **U** requires the reference to be the only external (owning) reference to
  the target bridge object
- **E∨U** means **E or U** — the reference is permitted if *either* condition
  holds. For bridge → bridge, the **E** case is a self-reference (a region
  has exactly one bridge object, so "same region" means the bridge
  referencing itself, e.g. `r.x = r`). This is a real, allowed operation, not
  merely a degenerate case. The **U** case is region nesting (one region's
  bridge holding the unique external reference to a child region's bridge).
- **borrowed** the reference is a borrow (increases the target region's LRC,
  does not transfer ownership)

---

## Canonical Examples

These examples are intentionally high-level. They show the shape of the
operations, not exact runtime APIs or exception types.

### Moving a local object into a region
```python
r = Region()
x = []
r.item = x          # x transfers into r; the local variable x is now a borrow
```

### Nesting one region inside another
```python
parent = Region()
child = Region()
parent.child = child            # child becomes nested under parent
assert child.parent == parent

parent.child = None             # the owning reference is removed
assert child.parent == None     # child is a root region again
```

### Self-reference inside a region
```python
r = Region()
r.x = r             # permitted: a bridge may reference itself (the E case)
```

### Freezing an object
```python
x = [1, 2, 3]
freeze(x)           # x and everything reachable from it become deeply immutable
```

### Sharing mutable state through a cown
```python
r = Region()
c = Cown(r)         # c mediates exclusive access to r
with c:             # acquire
    ...             # exclusive access to r here
                    # released on exit; r must be closed at that point
```

---

## What Lungfish Does NOT Do

- It does not prevent data races in code that bypasses the ownership model
  (e.g. non-Pyrona-aware C extensions)
- It does not provide a static type system — all enforcement is dynamic
- It does not prevent logical bugs — only structural data races
- It does not manage memory — reference counting and GC continue as normal

---

## Relationship to Python Sub-Interpreters (PEP 734)

PEP 734 allows multiple isolated Python sub-interpreters to run in the same
process, each with its own GIL. This enables true parallelism without
removing the GIL. However, sub-interpreters are fully isolated — objects
cannot be shared between them directly. Communication currently requires
costly serialisation (pickling).

Lungfish targets this model. Its goal is to enable direct, zero-copy sharing
of objects across sub-interpreters by making the shared objects either
immutable (frozen) or exclusively owned (transferred via regions). Each
sub-interpreter keeps its own GIL for its mutable objects. The region model
provides the discipline needed to safely move or share objects across that
boundary.

Free-threaded Python (PEP 703) is a different approach — it removes the GIL
entirely and makes all reference count manipulations atomic. It ensures the
interpreter does not crash but does not prevent data races in user code.
The two are complementary in a concrete sense: free-threaded Python provides
parallelism within a single interpreter but leaves user-level data races
possible, while Lungfish provides the ownership discipline that rules those
races out. Lungfish's immutable-object machinery (atomic reference counting
on frozen objects only) could also be used by free-threaded Python to make
shared immutable data cheaper. They solve different halves of the problem:
free-threading removes the serialisation point, Lungfish supplies the safety
guarantee.

---

## Terminology Quick Reference

| Term | Meaning |
|------|---------|
| Region | An isolated group of mutable objects owned by one sub-interpreter at a time |
| Bridge object | The handle/entry point to a region; externally unique over owning references |
| Local region | The implicit region for a sub-interpreter's stack, local frame, and new allocations |
| LRC | Local Reference Count — counts borrowed references pointing into a region |
| OSC | Open Sub-Region Count — counts how many of a region's direct children are open |
| Dirty | A region whose borrow bookkeeping can no longer be trusted; must be cleaned before it can be closed |
| Closed region | A region with LRC = 0, OSC = 0, and not dirty; safe to transfer or release |
| Cown | Concurrent owner — a cell mediating exclusive access to a region |
| Freeze | Making an object and all reachable state deeply immutable |
| Borrow | A reference from the local region into another region |
| Transfer | Moving ownership of a local object into a region |
| External reference | A cross-region owning reference targeting a bridge object |
| Ephemeral ownership | The local region's property of giving up ownership of one of its objects when that object becomes referenced from another region |
