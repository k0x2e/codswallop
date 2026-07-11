#include "dirindex.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct dirindex_entry {
    const char             *key;   /* interned pointer -- identity is the key */
    rpl_obj                *node;
    struct dirindex_entry  *next;
} dirindex_entry;

struct rpl_dir_index {
    dirindex_entry **buckets;
    size_t           nbuckets; /* always a power of two */
    size_t           count;
    rpl_obj         *tail;     /* last real node before lastobj */
};

static void *xrealloc(void *p, size_t n) {
    void *r = realloc(p, n);
    if (n && !r) {
        fprintf(stderr, "rpl: out of memory\n");
        abort();
    }
    return r;
}

/* Fibonacci/splitmix-style pointer hash -- keys are interned pointers, so
 * hashing the address itself is exact; no string content is ever touched. */
static size_t ptrhash(const void *p, size_t nbuckets) {
    uintptr_t x = (uintptr_t)p;
    x ^= x >> 15;
    x *= (uintptr_t)0x2545F4914F6CDD1DULL;
    x ^= x >> 13;
    return (size_t)x & (nbuckets - 1);
}

static void rehash(rpl_dir_index *idx) {
    size_t newn = idx->nbuckets * 2;
    dirindex_entry **nb = xrealloc(NULL, sizeof(dirindex_entry *) * newn);
    for (size_t i = 0; i < newn; i++)
        nb[i] = NULL;

    for (size_t i = 0; i < idx->nbuckets; i++) {
        dirindex_entry *e = idx->buckets[i];
        while (e) {
            dirindex_entry *next = e->next;
            size_t b = ptrhash(e->key, newn);
            e->next = nb[b];
            nb[b] = e;
            e = next;
        }
    }
    free(idx->buckets);
    idx->buckets = nb;
    idx->nbuckets = newn;
}

static void insert(rpl_dir_index *idx, const char *key, rpl_obj *node) {
    if (idx->count + 1 > idx->nbuckets)
        rehash(idx);
    size_t b = ptrhash(key, idx->nbuckets);
    dirindex_entry *e = xrealloc(NULL, sizeof(dirindex_entry));
    e->key = key;
    e->node = node;
    e->next = idx->buckets[b];
    idx->buckets[b] = e;
    idx->count++;
}

enum { DIRINDEX_INITIAL_BUCKETS = 256 };

rpl_dir_index *dirindex_build(rpl_obj *head, rpl_obj *lastobj) {
    if (head->dir.index)
        return NULL;

    rpl_dir_index *idx = xrealloc(NULL, sizeof(rpl_dir_index));
    idx->nbuckets = DIRINDEX_INITIAL_BUCKETS;
    idx->buckets = xrealloc(NULL, sizeof(dirindex_entry *) * idx->nbuckets);
    for (size_t i = 0; i < idx->nbuckets; i++)
        idx->buckets[i] = NULL;
    idx->count = 0;
    idx->tail = head;

    rpl_obj *node = head;
    while (node != lastobj) {
        insert(idx, node->dir.tag->tag.name, node);
        if (node->dir.next == lastobj)
            idx->tail = node;
        node = node->dir.next;
    }

    head->dir.index = idx;
    return idx;
}

void dirindex_free(rpl_dir_index *idx) {
    if (!idx)
        return;
    for (size_t i = 0; i < idx->nbuckets; i++) {
        dirindex_entry *e = idx->buckets[i];
        while (e) {
            dirindex_entry *next = e->next;
            free(e);
            e = next;
        }
    }
    free(idx->buckets);
    free(idx);
}

rpl_obj *dirindex_lookup(rpl_dir_index *idx, const char *key) {
    size_t b = ptrhash(key, idx->nbuckets);
    for (dirindex_entry *e = idx->buckets[b]; e; e = e->next)
        if (e->key == key)
            return e->node;
    return NULL;
}

void dirindex_append(rpl_dir_index *idx, char *key, rpl_obj *node) {
    insert(idx, key, node);
    idx->tail = node;
}

void dirindex_remove(rpl_dir_index *idx, const char *key) {
    size_t b = ptrhash(key, idx->nbuckets);
    dirindex_entry **link = &idx->buckets[b];
    while (*link && (*link)->key != key)
        link = &(*link)->next;
    if (*link) {
        dirindex_entry *dead = *link;
        *link = dead->next;
        free(dead);
        idx->count--;
    }
}

rpl_obj *dirindex_tail(rpl_dir_index *idx) {
    return idx->tail;
}

void dirindex_set_tail(rpl_dir_index *idx, rpl_obj *tail) {
    idx->tail = tail;
}
