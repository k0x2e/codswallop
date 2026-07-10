#include "obj.h"

#include <stdlib.h>
#include <string.h>

/* realloc() that aborts on OOM instead of returning NULL (mirrors gc.c's
 * helper of the same name -- including the collect-and-retry behavior, for
 * the same reason: kept local rather than shared across the two small
 * translation units). Every call site below is allocating an RPL object or
 * a payload buffer owned by one, which is exactly what the root-stack
 * contract (gc.h) exists to protect across an allocation like this. */
static void *xrealloc(rpl_gc *gc, void *p, size_t n) {
    void *r = realloc(p, n);
    if (n && !r) {
        gc_collect(gc);
        r = realloc(p, n);
        if (!r) {
            fprintf(stderr, "rpl: out of memory\n");
            abort();
        }
    }
    return r;
}

/* Copy n bytes of s into a fresh nul-terminated buffer. Used because RPL
 * string/symbol data isn't nul-terminated at the source (it's length +
 * pointer), but we still want ordinary C strings for owned storage. */
static char *xstrndup(rpl_gc *gc, const char *s, size_t n) {
    char *r = xrealloc(gc, NULL, n + 1);
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

static char *xstrdup(rpl_gc *gc, const char *s) {
    return xstrndup(gc, s, strlen(s));
}

rpl_obj *rpl_new_int(rpl_gc *gc, int64_t v) {
    rpl_obj *o = gc_alloc(gc, RPL_INTEGER);
    o->integer = v;
    return o;
}

rpl_obj *rpl_new_float(rpl_gc *gc, double v) {
    rpl_obj *o = gc_alloc(gc, RPL_FLOAT);
    o->floating = v;
    return o;
}

/* Shared body for rpl_new_string/rpl_new_comment: they differ only in the
 * type tag, both storing an owned copy of [data, data+len). */
static rpl_obj *new_stringlike(rpl_gc *gc, uint16_t type, const char *data, size_t len) {
    rpl_obj *o = gc_alloc(gc, type);
    o->string.data = xstrndup(gc, data, len);
    o->string.len = len;
    return o;
}

rpl_obj *rpl_new_string(rpl_gc *gc, const char *data, size_t len) {
    return new_stringlike(gc, RPL_STRING, data, len);
}

rpl_obj *rpl_new_comment(rpl_gc *gc, const char *data, size_t len) {
    return new_stringlike(gc, RPL_COMMENT, data, len);
}

/* Deep-copies the parts array itself, into owned storage; each component
 * string is interned (names.h) rather than strdup'd, so identical dotted-
 * path components across many symbols/tags share one allocation. The
 * caller's parts/strings can still be freed or stack-allocated immediately
 * after this returns. */
rpl_obj *rpl_new_symbol(rpl_gc *gc, char **parts, int nparts) {
    rpl_obj *o = gc_alloc(gc, RPL_SYMBOL);
    o->symbol.parts = xrealloc(gc, NULL, sizeof(char *) * (size_t)nparts);
    for (int i = 0; i < nparts; i++)
        o->symbol.parts[i] = rpl_intern(&gc->names, parts[i]);
    o->symbol.nparts = nparts;
    return o;
}

rpl_obj *rpl_new_quote(rpl_gc *gc, rpl_obj *inner) {
    rpl_obj *o = gc_alloc(gc, RPL_QUOTE);
    o->quote.inner = inner;
    return o;
}

rpl_obj *rpl_new_handle(rpl_gc *gc, FILE *f) {
    rpl_obj *o = gc_alloc(gc, RPL_HANDLE);
    o->handle = f;
    return o;
}

/* type selects List vs Code (they share the same payload layout). Starts
 * with no backing array; rpl_list_push grows it on demand. */
rpl_obj *rpl_new_list(rpl_gc *gc, uint16_t type) {
    rpl_obj *o = gc_alloc(gc, type);
    o->list.data = NULL;
    o->list.len = 0;
    o->list.cap = 0;
    return o;
}

/* Append item to list, growing the backing array geometrically (doubling,
 * starting at 4) when full. */
void rpl_list_push(rpl_gc *gc, rpl_obj *list, rpl_obj *item) {
    if (list->list.len == list->list.cap) {
        list->list.cap = list->list.cap ? list->list.cap * 2 : 4;
        list->list.data = xrealloc(gc, list->list.data, sizeof(rpl_obj *) * (size_t)list->list.cap);
    }
    list->list.data[list->list.len++] = item;
}

rpl_obj *rpl_new_tag(rpl_gc *gc, const char *name, rpl_obj *obj) {
    rpl_obj *o = gc_alloc(gc, RPL_TAG);
    o->tag.name = rpl_intern(&gc->names, name);
    o->tag.obj = obj;
    return o;
}

/* next == NULL makes the new directory self-referential -- the lastobj
 * sentinel pattern that terminates every named-store chain. */
rpl_obj *rpl_new_dir(rpl_gc *gc, rpl_obj *tag, rpl_obj *next) {
    rpl_obj *o = gc_alloc(gc, RPL_DIRECTORY);
    o->dir.tag = tag;
    o->dir.next = next ? next : o;
    return o;
}

/* next == NULL makes the new context its own next (the bottom of the call
 * stack) with depth reset to RPL_CALLDEPTH; otherwise depth counts down
 * from next's depth so callers can detect stack overflow by depth == 0. */
rpl_obj *rpl_new_context(rpl_gc *gc, rpl_obj *code, rpl_obj *names, rpl_obj *next) {
    rpl_obj *o = gc_alloc(gc, RPL_CONTEXT);
    o->context.code = code;
    o->context.names = names;
    if (next) {
        o->context.next = next;
        o->context.depth = next->context.depth - 1;
    } else {
        o->context.next = o;
        o->context.depth = RPL_CALLDEPTH;
    }
    o->context.ip = 0;
    return o;
}

rpl_obj *rpl_new_binproc(rpl_gc *gc, const char *name, rpl_eval_fn fn) {
    rpl_obj *o = gc_alloc(gc, RPL_INTERNAL);
    o->binproc.name = xstrdup(gc, name);
    o->binproc.fn = fn;
    return o;
}

/* Creates an empty builtin (no dispatch entries yet); use
 * rpl_builtin_add_dispatch to register each typed handler. */
rpl_obj *rpl_new_builtin(rpl_gc *gc, const char *name, const char *hint, int argct) {
    rpl_obj *o = gc_alloc(gc, RPL_BUILTIN);
    o->builtin.name = xstrdup(gc, name);
    o->builtin.hint = hint ? xstrdup(gc, hint) : NULL;
    o->builtin.argct = argct;
    o->builtin.argck = NULL;
    o->builtin.dispatches = NULL;
    o->builtin.ndispatches = 0;
    return o;
}

/* Appends one dispatch entry: argct type numbers (copied from types) paired
 * with handler, growing bin's argck/dispatches arrays by one slot each.
 * Order matters -- dispatch is first-match linear scan, so earlier calls
 * take priority over later ones with overlapping argck patterns. */
void rpl_builtin_add_dispatch(rpl_gc *gc, rpl_obj *bin, const int *types, rpl_obj *handler) {
    int n = bin->builtin.ndispatches;
    bin->builtin.argck = xrealloc(gc, bin->builtin.argck,
                                   sizeof(int) * (size_t)(n + 1) * (size_t)bin->builtin.argct);
    memcpy(bin->builtin.argck + (size_t)n * (size_t)bin->builtin.argct, types,
           sizeof(int) * (size_t)bin->builtin.argct);
    bin->builtin.dispatches = xrealloc(gc, bin->builtin.dispatches, sizeof(rpl_obj *) * (size_t)(n + 1));
    bin->builtin.dispatches[n] = handler;
    bin->builtin.ndispatches = n + 1;
}

/* Translation of typedir.cp() from pysys/rtypes.py. `self` is the directory
 * node cp() was originally called on: its type number is what subdirectory
 * entries are compared against to decide whether to recurse. */
static rpl_obj *dir_cp(rpl_gc *gc, rpl_obj *self, int depth) {
    if (!depth)
        return self;

    rpl_obj *ourcopy = rpl_new_dir(gc, rpl_cp(gc, self->dir.tag), self->dir.next);
    rpl_obj *rest = ourcopy;
    rpl_obj *current = self;

    /* lastobj-style sentinels point to themselves; stop when we reach one. */
    while (current->dir.next != current->dir.next->dir.next) {
        current = current->dir.next;
        rest->dir.next = rpl_new_dir(gc, rpl_cp(gc, rest->dir.next->dir.tag), current->dir.next);
        rest = rest->dir.next;
        if (rest->dir.tag->tag.obj->type == RPL_DIRECTORY)
            rest->dir.tag->tag.obj = dir_cp(gc, rest->dir.tag->tag.obj, depth - 1);
    }
    return ourcopy;
}

/* Dispatches to the per-type copy semantics described in obj.h: immutable
 * types (int, float, string, symbol, quote, handle, internal, context) fall
 * through to the default case and return themselves unchanged. */
rpl_obj *rpl_cp(rpl_gc *gc, rpl_obj *o) {
    switch (o->type) {
        /* Shallow copy: new backing array, same element pointers. */
        case RPL_LIST:
        case RPL_CODE: {
            rpl_obj *n = gc_alloc(gc, o->type);
            n->list.len = o->list.len;
            n->list.cap = o->list.len;
            n->list.data = n->list.len
                ? xrealloc(gc, NULL, sizeof(rpl_obj *) * (size_t)n->list.len)
                : NULL;
            memcpy(n->list.data, o->list.data, sizeof(rpl_obj *) * (size_t)o->list.len);
            return n;
        }
        /* New tag, same name text, same contained object (not deep-copied).
         * The name is already an interned pointer -- just bump its refcount
         * (O(1)) and reuse it, rather than re-interning the text. */
        case RPL_TAG: {
            rpl_obj *n = gc_alloc(gc, RPL_TAG);
            rpl_intern_ref(o->tag.name);
            n->tag.name = o->tag.name;
            n->tag.obj = o->tag.obj;
            return n;
        }
        /* New builtin with its own copies of the argck/dispatches arrays,
         * so patching one builtin's dispatch table (binhook) can't affect
         * a snapshot taken via cp(). */
        case RPL_BUILTIN: {
            rpl_obj *n = gc_alloc(gc, RPL_BUILTIN);
            n->builtin.name = xstrdup(gc, o->builtin.name);
            n->builtin.hint = o->builtin.hint ? xstrdup(gc, o->builtin.hint) : NULL;
            n->builtin.argct = o->builtin.argct;
            n->builtin.ndispatches = o->builtin.ndispatches;
            size_t argck_n = (size_t)o->builtin.ndispatches * (size_t)o->builtin.argct;
            n->builtin.argck = argck_n ? xrealloc(gc, NULL, sizeof(int) * argck_n) : NULL;
            if (argck_n)
                memcpy(n->builtin.argck, o->builtin.argck, sizeof(int) * argck_n);
            n->builtin.dispatches = o->builtin.ndispatches
                ? xrealloc(gc, NULL, sizeof(rpl_obj *) * (size_t)o->builtin.ndispatches)
                : NULL;
            if (o->builtin.ndispatches)
                memcpy(n->builtin.dispatches, o->builtin.dispatches,
                       sizeof(rpl_obj *) * (size_t)o->builtin.ndispatches);
            return n;
        }
        case RPL_DIRECTORY:
            return dir_cp(gc, o, RPL_CPDEPTH);
        default:
            return o;
    }
}
