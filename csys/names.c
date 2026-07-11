#include "names.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Header sits immediately before the NUL-terminated text handed out to
 * callers, so a bare `char *` returned by rpl_intern is enough to get back
 * to its refcount/id/chain link in O(1) -- no table lookup needed for
 * rpl_intern_ref/rpl_intern_id, and only a bucket recompute (not a walk)
 * for rpl_intern_unref. */
struct rpl_iname {
    rpl_iname *hnext;
    int        refcount;
    int        id;
    char       data[];
};

static void *xrealloc(void *p, size_t n) {
    void *r = realloc(p, n);
    if (n && !r) {
        fprintf(stderr, "rpl: out of memory\n");
        abort();
    }
    return r;
}

static unsigned long hash_str(const char *s) {
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*s++))
        h = ((h << 5) + h) + (unsigned long)c; /* djb2 */
    return h;
}

static inline rpl_iname *hdr(char *interned) {
    return (rpl_iname *)(interned - offsetof(rpl_iname, data));
}

enum { INITIAL_BUCKETS = 64 };

void intern_init(rpl_intern_table *t) {
    memset(t, 0, sizeof(*t));
    t->nbuckets = INITIAL_BUCKETS;
    t->buckets = xrealloc(NULL, sizeof(rpl_iname *) * t->nbuckets);
    memset(t->buckets, 0, sizeof(rpl_iname *) * t->nbuckets);
}

void intern_destroy(rpl_intern_table *t) {
    for (size_t i = 0; i < t->nbuckets; i++) {
        rpl_iname *e = t->buckets[i];
        while (e) {
            rpl_iname *next = e->hnext;
            free(e);
            e = next;
        }
    }
    free(t->buckets);
    free(t->freeids);
    memset(t, 0, sizeof(*t));
}

/* Doubles bucket count and rehashes every live entry in place -- entries
 * are relinked, not copied, so no id or pointer handed out to a caller
 * ever changes. */
static void rehash(rpl_intern_table *t) {
    size_t newn = t->nbuckets * 2;
    rpl_iname **nb = xrealloc(NULL, sizeof(rpl_iname *) * newn);
    memset(nb, 0, sizeof(rpl_iname *) * newn);
    for (size_t i = 0; i < t->nbuckets; i++) {
        rpl_iname *e = t->buckets[i];
        while (e) {
            rpl_iname *next = e->hnext;
            size_t b = hash_str(e->data) % newn;
            e->hnext = nb[b];
            nb[b] = e;
            e = next;
        }
    }
    free(t->buckets);
    t->buckets = nb;
    t->nbuckets = newn;
}

char *rpl_intern(rpl_intern_table *t, const char *name) {
    unsigned long h = hash_str(name);
    size_t b = h % t->nbuckets;
    for (rpl_iname *e = t->buckets[b]; e; e = e->hnext) {
        if (strcmp(e->data, name) == 0) {
            e->refcount++;
            return e->data;
        }
    }

    size_t len = strlen(name);
    rpl_iname *e = xrealloc(NULL, sizeof(rpl_iname) + len + 1);
    memcpy(e->data, name, len + 1);
    e->refcount = 1;
    e->id = t->freelen ? t->freeids[--t->freelen] : t->next_id++;

    if (t->live + 1 > t->nbuckets)
        rehash(t);
    b = h % t->nbuckets;
    e->hnext = t->buckets[b];
    t->buckets[b] = e;
    t->live++;
    return e->data;
}

void rpl_intern_ref(char *interned) {
    hdr(interned)->refcount++;
}

void rpl_intern_unref(rpl_intern_table *t, char *interned) {
    rpl_iname *e = hdr(interned);
    if (--e->refcount > 0)
        return;

    size_t b = hash_str(e->data) % t->nbuckets;
    rpl_iname **link = &t->buckets[b];
    while (*link && *link != e)
        link = &(*link)->hnext;
    if (*link == e)
        *link = e->hnext;
    t->live--;

    if (t->freelen == t->freecap) {
        t->freecap = t->freecap ? t->freecap * 2 : 16;
        t->freeids = xrealloc(t->freeids, sizeof(int) * t->freecap);
    }
    t->freeids[t->freelen++] = e->id;

    free(e);
}

int rpl_intern_id(const char *interned) {
    return hdr((char *)interned)->id;
}
