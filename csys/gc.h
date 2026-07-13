#ifndef RPL_GC_H
#define RPL_GC_H

#include "names.h"
#include "rpl.h"

/* Stop-the-world mark-and-sweep collector.
 *
 * Heap: an intrusive singly-linked list of every live allocation, threaded
 * through rpl_obj.alloc_next. Allocation is malloc + prepend; collection is
 * mark from roots, then sweep the list freeing anything unmarked.
 *
 * Roots come from three places:
 *   - perm_roots: slots that live for the runtime's whole lifetime (the
 *     data stack, the context chain, lastobj, etc. -- wired up in Phase 2).
 *   - root_stack: a scratch stack that C internals push onto before calling
 *     anything which might allocate, and pop before returning. This is the
 *     only way to keep a freshly-created, not-yet-stored local pointer safe
 *     across an allocation.
 *   - held: a by-value scratch stack (see gc_hold below) that every
 *     stack_pop() automatically pushes its result onto, released in a
 *     batch once per trampoline step (rpl_rs). Popping removes a value
 *     from the data stack -- a perm root -- and builtins routinely keep
 *     using the raw pointer afterward across further allocating calls;
 *     gc_hold makes that safe without every call site having to remember
 *     its own root_push/pop. Freshly gc_alloc()'d objects are NOT
 *     automatically held (unlike a popped stack value, nothing yet
 *     depends on them being reachable, and auto-holding every allocation
 *     would make plain "allocate and abandon" garbage immune to
 *     collection for the rest of the step) -- constructors in obj.c that
 *     need their own in-progress object protected across further
 *     allocating calls do it explicitly via root_push/pop, same as any
 *     other internal.
 */
typedef struct rpl_gc {
    rpl_obj *all;
    size_t   count;

    rpl_obj ***root_stack;
    size_t     root_len;
    size_t     root_cap;

    rpl_obj ***perm_roots;
    size_t     perm_len;
    size_t     perm_cap;

    rpl_obj  **held;
    size_t     held_len;
    size_t     held_cap;

    /* Interning table for Tag names / Symbol path components -- see
     * names.h. Owned by the gc because gc_free_payload (below) is where
     * Symbol/Tag objects release their references as they're reaped. */
    rpl_intern_table names;
} rpl_gc;

void gc_init(rpl_gc *gc);
void gc_destroy(rpl_gc *gc);

/* Allocate a zeroed object of the given type and thread it onto the heap.
 * Not held or rooted -- see the note on `held` above and gc_root_push
 * below for who's responsible for protecting it if it needs to survive a
 * further allocating call before being stored somewhere durable. */
rpl_obj *gc_alloc(rpl_gc *gc, uint16_t type);

/* Root stack: push the address of a local rpl_obj* before it might be the
 * only reference to an object across an allocating call; pop it after. Use
 * this when a slot needs to stay protected across a call boundary wider
 * than "until the current trampoline step ends" (e.g. a loop that spans
 * more than one gc_hold-eligible operation) or when byref live-tracking
 * matters (the slot gets reassigned and each new value needs protecting). */
void gc_root_push(rpl_gc *gc, rpl_obj **slot);
void gc_root_pop(rpl_gc *gc, int n);

/* Held stack: push an object *by value* (no address needed, unlike
 * root_stack) so it survives until gc_release() truncates the held stack
 * back to an earlier mark. stack_pop() calls this automatically; rpl_rs()
 * takes a mark before each trampoline step and releases back to it after,
 * so anything a single builtin call pops off the data stack stays
 * protected for that whole call regardless of how many further
 * allocations happen with it still only reachable from a C local, and
 * goes back to being ordinary collectible garbage the instant the step
 * that needed it is done. */
void gc_hold(rpl_gc *gc, rpl_obj *o);
size_t gc_hold_mark(rpl_gc *gc);
void gc_release(rpl_gc *gc, size_t mark);

/* Permanent roots: registered once, live for the runtime's lifetime. */
void gc_add_perm_root(rpl_gc *gc, rpl_obj **slot);
void gc_remove_perm_root(rpl_gc *gc, rpl_obj **slot);

void gc_collect(rpl_gc *gc);

#endif /* RPL_GC_H */
