#include "gc.h"

#include <stdlib.h>
#include <string.h>

/* realloc() that aborts on OOM instead of returning NULL, so callers never
 * have to check. realloc(p, 0) legitimately returns NULL, hence the n check. */
static void *xrealloc(void *p, size_t n) {
    void *r = realloc(p, n);
    if (n && !r) {
        fprintf(stderr, "rpl: out of memory\n");
        abort();
    }
    return r;
}

void gc_init(rpl_gc *gc) {
    memset(gc, 0, sizeof(*gc));
}

/* Free the heap-allocated payload data hanging off an object (the arrays
 * and buffers rpl_obj itself doesn't own inline) before the rpl_obj struct
 * is freed. Called from both gc_destroy (full teardown) and gc_collect
 * (per-object sweep), so it must not touch alloc_next or other objects. */
static void gc_free_payload(rpl_obj *o) {
    switch (o->type) {
        case RPL_STRING:
        case RPL_COMMENT:
            free(o->string.data);
            break;
        case RPL_SYMBOL:
            for (int i = 0; i < o->symbol.nparts; i++)
                free(o->symbol.parts[i]);
            free(o->symbol.parts);
            break;
        case RPL_LIST:
        case RPL_CODE:
            free(o->list.data);
            break;
        case RPL_TAG:
            free(o->tag.name);
            break;
        case RPL_BUILTIN:
            free(o->builtin.name);
            free(o->builtin.hint);
            free(o->builtin.argck);
            free(o->builtin.dispatches);
            break;
        case RPL_INTERNAL:
            free(o->binproc.name);
            break;
        default:
            break;
    }
}

/* Tear down the whole heap unconditionally -- every allocation ever made
 * through this gc, live or not -- then reset the gc struct to zero. Used at
 * runtime shutdown; not a substitute for gc_collect(). */
void gc_destroy(rpl_gc *gc) {
    rpl_obj *o = gc->all;
    while (o) {
        rpl_obj *next = o->alloc_next;
        gc_free_payload(o);
        free(o);
        o = next;
    }
    free(gc->root_stack);
    free(gc->perm_roots);
    memset(gc, 0, sizeof(*gc));
}

/* Allocate a zeroed object of the given type and prepend it to gc->all so
 * gc_collect() and gc_destroy() can find it. Payload fields are left zeroed;
 * callers (rpl_new_* in obj.c) fill them in immediately after. */
rpl_obj *gc_alloc(rpl_gc *gc, uint16_t type) {
    rpl_obj *o = xrealloc(NULL, sizeof(rpl_obj));
    memset(o, 0, sizeof(*o));
    o->type = type;
    o->alloc_next = gc->all;
    gc->all = o;
    gc->count++;
    return o;
}

/* Push the address of a local rpl_obj* onto the scratch root stack. Grows
 * the backing array geometrically like the other dynamic arrays here. */
void gc_root_push(rpl_gc *gc, rpl_obj **slot) {
    if (gc->root_len == gc->root_cap) {
        gc->root_cap = gc->root_cap ? gc->root_cap * 2 : 16;
        gc->root_stack = xrealloc(gc->root_stack, gc->root_cap * sizeof(*gc->root_stack));
    }
    gc->root_stack[gc->root_len++] = slot;
}

/* Pop n entries off the scratch root stack (LIFO, matching gc_root_push).
 * Clamped so popping more than what's on the stack is a harmless no-op. */
void gc_root_pop(rpl_gc *gc, int n) {
    while (n-- > 0 && gc->root_len)
        gc->root_len--;
}

/* Register a slot as a permanent root: everything reachable from *slot is
 * kept alive by every future gc_collect() until gc_remove_perm_root undoes
 * this. Used for runtime-lifetime roots (data stack, context chain, etc.). */
void gc_add_perm_root(rpl_gc *gc, rpl_obj **slot) {
    if (gc->perm_len == gc->perm_cap) {
        gc->perm_cap = gc->perm_cap ? gc->perm_cap * 2 : 16;
        gc->perm_roots = xrealloc(gc->perm_roots, gc->perm_cap * sizeof(*gc->perm_roots));
    }
    gc->perm_roots[gc->perm_len++] = slot;
}

/* Undo gc_add_perm_root: find slot by pointer identity and remove it,
 * swapping in the last entry to keep the array dense. No-op if not found. */
void gc_remove_perm_root(rpl_gc *gc, rpl_obj **slot) {
    for (size_t i = 0; i < gc->perm_len; i++) {
        if (gc->perm_roots[i] == slot) {
            gc->perm_roots[i] = gc->perm_roots[gc->perm_len - 1];
            gc->perm_len--;
            return;
        }
    }
}

/* Recursively mark o and everything reachable from it, following exactly
 * the pointer fields each type's union actually uses. Self-referential
 * sentinels (lastobj, base_context) terminate naturally via the
 * already-marked check at the top rather than needing special-casing. */
static void gc_mark(rpl_obj *o) {
    if (!o || rpl_marked(o))
        return;
    rpl_set_mark(o);

    switch (o->type) {
        case RPL_LIST:
        case RPL_CODE:
            for (int i = 0; i < o->list.len; i++)
                gc_mark(o->list.data[i]);
            break;
        case RPL_DIRECTORY:
            gc_mark(o->dir.tag);
            gc_mark(o->dir.next);
            break;
        case RPL_TAG:
            gc_mark(o->tag.obj);
            break;
        case RPL_QUOTE:
            gc_mark(o->quote.inner);
            break;
        case RPL_CONTEXT:
            gc_mark(o->context.code);
            gc_mark(o->context.names);
            gc_mark(o->context.next);
            break;
        case RPL_BUILTIN:
            for (int i = 0; i < o->builtin.ndispatches; i++)
                gc_mark(o->builtin.dispatches[i]);
            break;
        default:
            break;
    }
}

/* Full stop-the-world collection: mark from every permanent root and every
 * entry on the scratch root stack, then sweep gc->all, freeing (via
 * gc_free_payload + free) anything left unmarked and clearing the mark bit
 * on survivors so the next collection starts clean. */
void gc_collect(rpl_gc *gc) {
    for (size_t i = 0; i < gc->perm_len; i++)
        gc_mark(*gc->perm_roots[i]);
    for (size_t i = 0; i < gc->root_len; i++)
        gc_mark(*gc->root_stack[i]);

    rpl_obj **link = &gc->all;
    while (*link) {
        rpl_obj *o = *link;
        if (rpl_marked(o)) {
            rpl_clear_mark(o);
            link = &o->alloc_next;
        } else {
            *link = o->alloc_next;
            gc_free_payload(o);
            free(o);
            gc->count--;
        }
    }
}
