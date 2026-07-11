#ifndef RPL_DIRINDEX_H
#define RPL_DIRINDEX_H

#include "rpl.h"

/* O(1) accelerator for a directory chain's tail segment.
 *
 * rpl_rcl/rpl_deref/rpl_sto/rpl_rm (runtime.c) normally resolve a path
 * component by walking node-by-node from some head, comparing
 * dir.tag->tag.name against the (interned, names.h) query key. For most
 * directories -- every per-call local scope, created fresh and thrown away
 * a few instructions later -- that's fine: a handful of entries, walked a
 * handful of times. But the one directory that's genuinely permanent (the
 * root context's names chain, alive and growing for the whole run) gets
 * walked millions of times against hundreds of entries, and that walk
 * dominates runtime (see PLAN.md's profiling note).
 *
 * Rather than promote chains to indexed status adaptively, indexing here
 * is deliberate and explicit: the MKIDX builtin (I*.mkidx, internals.c)
 * calls dirindex_build() once, by hand, from RPL source (boot.rpl), on
 * whatever the root context's names head currently is -- after the
 * standard library has finished populating it, before the REPL/any real
 * program starts hammering on it. Everything already stored by that point
 * gets O(1) lookup from then on; anything stored afterward stays correctly
 * indexed too, via the incremental dirindex_append/dirindex_remove hooks
 * runtime.c's rpl_sto/rpl_rm already call. Every other directory in the
 * system (every local scope, every user MKDIR result nobody ever indexes)
 * is untouched by any of this and keeps walking exactly as before.
 *
 * Keyed by the *interned* name pointer itself (names.h): interning
 * guarantees one allocation per distinct name, so hashing the pointer
 * value is exact -- no string content ever needs comparing here. */

typedef struct rpl_dir_index rpl_dir_index;

/* Walks head's chain to lastobj once, indexing every tag found (including
 * any nulltag scope-boundary node's empty name -- harmless, never queried,
 * simpler than special-casing it out). Attaches the result to head->dir.index.
 * A no-op (returns NULL, doesn't touch head) if head is already indexed. */
rpl_dir_index *dirindex_build(rpl_obj *head, rpl_obj *lastobj);

void dirindex_free(rpl_dir_index *idx);

/* O(1): NULL if key isn't present anywhere in the indexed span. */
rpl_obj *dirindex_lookup(rpl_dir_index *idx, const char *key);

/* Record a node newly appended at the tail under key, and advance the
 * cached tail pointer to it (rpl_sto's append path, once it knows head is
 * indexed, uses dirindex_tail() below to splice in O(1) instead of
 * re-walking to find the end). */
void dirindex_append(rpl_dir_index *idx, char *key, rpl_obj *node);

/* Drop key's entry (rpl_rm, after its usual O(n) splice -- removal is rare
 * enough that finding the splice point doesn't need to be O(1), only
 * keeping the index honest afterward does). */
void dirindex_remove(rpl_dir_index *idx, const char *key);

rpl_obj *dirindex_tail(rpl_dir_index *idx);
void     dirindex_set_tail(rpl_dir_index *idx, rpl_obj *tail);

#endif /* RPL_DIRINDEX_H */
