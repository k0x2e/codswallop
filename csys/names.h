#ifndef RPL_NAMES_H
#define RPL_NAMES_H

#include <stddef.h>

/* Reference-counted string interning table for named-store keys: RPL_TAG
 * names and RPL_SYMBOL dotted-path components. Every dir chain in the
 * named store is walked component-by-component comparing tag names against
 * symbol parts (see rpl_rcl/rpl_deref/rpl_sto/rpl_rm/rpl_circsym in
 * runtime.c); before this, every one of those strings was its own private
 * strdup'd copy, so "I*", "EXCEPT", or any name mentioned across many tags
 * (which is most of them -- CASE/FOREACH/etc. bind the same handful of
 * names over and over) had one allocation per mention.
 *
 * Interning collapses identical text to one shared allocation with a
 * refcount and a dense id, handed out by rpl_intern() and released via
 * rpl_intern_unref() when the owning Symbol/Tag is garbage-collected (see
 * gc_free_payload in gc.c). Once a name's refcount hits zero its table
 * entry is dropped and its id is recycled by the next new name, so ids
 * stay dense without needing to renumber/compact anything already live --
 * that's what makes this safe to reference by raw pointer (tag.name,
 * symbol.parts[i]) instead of by index, unlike an array that gets shifted
 * on removal. */

typedef struct rpl_iname rpl_iname;

typedef struct {
    rpl_iname **buckets;
    size_t      nbuckets;
    size_t      live;      /* distinct interned names right now */

    int    *freeids;
    size_t  freelen;
    size_t  freecap;
    int     next_id;
} rpl_intern_table;

void intern_init(rpl_intern_table *t);
void intern_destroy(rpl_intern_table *t);

/* Look up name; if present, bump its refcount and return the shared
 * interned pointer. Otherwise create a new entry (refcount 1, a freshly
 * assigned or recycled id) and return its pointer. The result is a
 * NUL-terminated string owned by the table: callers store the pointer
 * (tag.name, symbol.parts[i]) but must release it via rpl_intern_unref,
 * never free(), when they're done with it. */
char *rpl_intern(rpl_intern_table *t, const char *name);

/* Bump the refcount of an already-interned pointer. O(1): refcount and id
 * live in a small header immediately behind the returned pointer, so this
 * needs no table lookup. Used when copying an object that already holds
 * an interned name (rpl_cp on a Tag) instead of re-interning its text. */
void rpl_intern_ref(char *interned);

/* Drop one reference to an interned pointer. At zero the entry is removed
 * from the table and its id is pushed onto a freelist for reuse. */
void rpl_intern_unref(rpl_intern_table *t, char *interned);

/* The id assigned to this interned name (stable for its lifetime, reused
 * once released). Exposed for introspection/debugging/future ROM dedup. */
int rpl_intern_id(const char *interned);

#endif /* RPL_NAMES_H */
