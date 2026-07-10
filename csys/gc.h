#ifndef RPL_GC_H
#define RPL_GC_H

#include "rpl.h"

/* Stop-the-world mark-and-sweep collector.
 *
 * Heap: an intrusive singly-linked list of every live allocation, threaded
 * through rpl_obj.alloc_next. Allocation is malloc + prepend; collection is
 * mark from roots, then sweep the list freeing anything unmarked.
 *
 * Roots come from two places:
 *   - perm_roots: slots that live for the runtime's whole lifetime (the
 *     data stack, the context chain, lastobj, etc. -- wired up in Phase 2).
 *   - root_stack: a scratch stack that C internals push onto before calling
 *     anything which might allocate, and pop before returning. This is the
 *     only way to keep a freshly-created, not-yet-stored local pointer safe
 *     across an allocation.
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
} rpl_gc;

void gc_init(rpl_gc *gc);
void gc_destroy(rpl_gc *gc);

/* Allocate a zeroed object of the given type and thread it onto the heap. */
rpl_obj *gc_alloc(rpl_gc *gc, uint16_t type);

/* Root stack: push the address of a local rpl_obj* before it might be the
 * only reference to an object across an allocating call; pop it after. */
void gc_root_push(rpl_gc *gc, rpl_obj **slot);
void gc_root_pop(rpl_gc *gc, int n);

/* Permanent roots: registered once, live for the runtime's lifetime. */
void gc_add_perm_root(rpl_gc *gc, rpl_obj **slot);
void gc_remove_perm_root(rpl_gc *gc, rpl_obj **slot);

void gc_collect(rpl_gc *gc);

#endif /* RPL_GC_H */
