#include "runtime.h"
#include "types.h"
#include "dirindex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* realloc() that aborts on OOM instead of returning NULL (same pattern as
 * gc.c/obj.c's helpers; kept local since this translation unit doesn't
 * share a header with them). */
static void *xrealloc(void *p, size_t n) {
    void *r = realloc(p, n);
    if (n && !r) {
        fprintf(stderr, "rpl: out of memory\n");
        abort();
    }
    return r;
}

void rpl_runtime_init(rpl_runtime *rt) {
    memset(rt, 0, sizeof(*rt));
    gc_init(&rt->gc);

    rt->stack = rpl_new_list(&rt->gc, RPL_LIST);

    /* nulltag/lastobj: the sentinel pair every directory chain terminates
     * at. lastobj is deliberately self-referential (rpl_new_dir(next=NULL)). */
    rt->nulltag = rpl_new_tag(&rt->gc, "", rpl_new_int(&rt->gc, 0));
    rt->lastobj = rpl_new_dir(&rt->gc, rt->nulltag, NULL);

    /* ret_internal/nullcode: the shared "drop this context" instruction and
     * a code object containing only it, used as ded()'s throwaway frame body. */
    rt->ret_internal = rpl_new_binproc(&rt->gc, "semicolon", rpl_ret);
    rt->nullcode = rpl_new_list(&rt->gc, RPL_CODE);
    rpl_list_push(&rt->gc, rt->nullcode, rt->ret_internal);

    /* The symbol ded() evaluates to hand off to the (currently unimplemented,
     * Phase 3) user-replaceable error handler. */
    char *parts[1] = { "EXCEPT" };
    rt->dedsym = rpl_new_symbol(&rt->gc, parts, 1);

    rt->nullcaller = rpl_new_string(&rt->gc, "", 0);
    rt->rtcaller = rpl_new_string(&rt->gc, "a higher power", strlen("a higher power"));
    rt->caller = rt->nullcaller;

    rt->reason = NULL;
    rt->running = 1;
    rt->brk = 0;
    rt->interrupt = 0;
    rt->dieanyway = 0;

    /* First Context: runs nullcode (i.e. immediately returns/halts) with a
     * fresh top-level namespace. Real programs replace this via newcall
     * once boot code is loaded (Phase 6/7). */
    rt->context = rpl_new_context(&rt->gc, rt->nullcode, rpl_firstdir(rt, NULL), NULL);

    gc_add_perm_root(&rt->gc, &rt->stack);
    gc_add_perm_root(&rt->gc, &rt->context);
    gc_add_perm_root(&rt->gc, &rt->nulltag);
    gc_add_perm_root(&rt->gc, &rt->lastobj);
    gc_add_perm_root(&rt->gc, &rt->nullcode);
    gc_add_perm_root(&rt->gc, &rt->ret_internal);
    gc_add_perm_root(&rt->gc, &rt->dedsym);
    gc_add_perm_root(&rt->gc, &rt->nullcaller);
    gc_add_perm_root(&rt->gc, &rt->rtcaller);
    gc_add_perm_root(&rt->gc, &rt->caller);
}

void rpl_runtime_destroy(rpl_runtime *rt) {
    free(rt->reason);
    gc_destroy(&rt->gc);
}

rpl_thunk rpl_default_eval(rpl_runtime *rt, rpl_obj *self) {
    rpl_list_push(&rt->gc, rt->stack, self);
    return (rpl_thunk){ rpl_context_eval, rt->context };
}

rpl_thunk rpl_obj_eval(rpl_runtime *rt, rpl_obj *o) {
    (void)rt;   /* not needed to build the thunk; the fn it names uses rt when invoked */
    switch (o->type) {
        case RPL_CONTEXT:
            return (rpl_thunk){ rpl_context_eval, o };
        /* typebinproc carries its own eval function per-instance (set at
         * construction, e.g. rt->ret_internal -> rpl_ret) rather than
         * sharing one like every other type. */
        case RPL_INTERNAL:
            return (rpl_thunk){ o->binproc.fn, o };
        case RPL_QUOTE:
            return (rpl_thunk){ rpl_quote_eval, o };
        case RPL_COMMENT:
            return (rpl_thunk){ rpl_comment_eval, o };
        case RPL_TAG:
            return (rpl_thunk){ rpl_tag_eval, o };
        case RPL_CODE:
            return (rpl_thunk){ rpl_code_eval, o };
        case RPL_SYMBOL:
            return (rpl_thunk){ rpl_symbol_eval, o };
        case RPL_BUILTIN:
            return (rpl_thunk){ rpl_builtin_eval, o };
        /* Integer, Float, String, Directory, Handle: none of these
         * override eval, so they fall through to the shared default
         * (push self, resume the current context). */
        default:
            return (rpl_thunk){ rpl_default_eval, o };
    }
}

rpl_thunk rpl_context_eval(rpl_runtime *rt, rpl_obj *self) {
    if (rt->brk) {
        rt->brk = 0;
        rt->interrupt = 1;
        return rpl_ded(rt, "Break");
    }

    rpl_obj *item = self->context.code->list.data[self->context.ip];
    self->context.ip++;
    return rpl_obj_eval(rt, item);
}

rpl_thunk rpl_ret(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    if (rt->context == rt->context->context.next) {
        /* Bottom of the call stack: erase it so a future newcall can reuse
         * this slot, and stop the trampoline. */
        rt->context->context.code = rt->nullcode;
        rt->context->context.ip = 0;
        rt->running = 0;
    } else {
        rt->context = rt->context->context.next;
    }
    return (rpl_thunk){ rpl_context_eval, rt->context };
}

rpl_thunk rpl_ded(rpl_runtime *rt, const char *reason) {
    free(rt->reason);
    rt->reason = xrealloc(NULL, strlen(reason) + 1);
    memcpy(rt->reason, reason, strlen(reason) + 1);

    /* Force a new context wrapping the current one (same names) so the
     * handler is exempt from the ordinary call-depth limit and doesn't
     * land on top of code we might want to trace back through. */
    rt->context = rpl_new_context(&rt->gc, rt->nullcode, rt->context->context.names, rt->context);

    if (rt->context->context.depth < -1) {
        fprintf(stderr, "...panik!  Excess recursion while already trying to handle an error\n");
        rt->running = 0;
    }

    return rpl_obj_eval(rt, rt->dedsym);
}

/* Each trampoline step is exactly one thunk call -- one builtin/internal
 * invocation, one symbol resolution, one instruction fetch, etc. -- and by
 * construction (see gc.h's file header) never recurses back into rpl_rs
 * itself mid-step. That makes "one step" the natural unit of safety: mark
 * the held stack before the call and release back to that mark after, so
 * anything the step gc_hold()'d (every fresh gc_alloc(), every stack_pop())
 * stays protected for the step's whole duration no matter how many further
 * allocations happen with it still only reachable from a C local, and goes
 * back to being ordinary collectible garbage the instant the step that
 * needed it is done.
 *
 * One subtlety that isn't covered by "release after the step that needed
 * it": the *next* thunk's target -- next.self, e.g. the Code object IFTE
 * just chose that rpl_code_eval is about to call -- is very often a value
 * the current step just stack_pop()'d and is only still alive via this
 * step's held batch. Releasing that batch and *then* handing next.self to
 * the following iteration would leave it referenced by nothing but a C
 * local for however long it takes that iteration's own allocations to run,
 * which is exactly the gap a gc_collect() triggered in there could sweep it
 * in. `carry` closes that gap: its address is registered once via
 * gc_root_push (not re-pushed every iteration, so this costs one root slot
 * for rpl_rs's whole run, not one per step) and reassigned to next.self
 * after every step, so whatever the upcoming step is about to operate on
 * stays reachable across the boundary (gc_mark is transitive, so this
 * protects everything next.self reaches too, not just the pointer itself). */
void rpl_rs(rpl_runtime *rt, rpl_thunk next) {
    /* Everything allocated before this call (ROM loading, boot) has been
     * gc_hold()'d but never released, since that only happens per-step
     * below. Anything from that phase actually worth keeping is already
     * reachable through a perm root (rt->stack, rt->context, the named
     * store) by now, so it's safe to drop the whole backlog in one go --
     * otherwise it would sit in `held` forever, silently exempting it from
     * every future collection. */
    gc_release(&rt->gc, 0);
    rpl_obj *carry = next.self;
    gc_root_push(&rt->gc, &carry);
    while (rt->running) {
        size_t mark = gc_hold_mark(&rt->gc);
        next = next.fn(rt, next.self);
        gc_release(&rt->gc, mark);
        carry = next.self;
    }
    gc_root_pop(&rt->gc, 1);
}

rpl_thunk rpl_newcall(rpl_runtime *rt, rpl_obj *obj) {
    rpl_obj *ctx = rt->context;
    if (ctx->context.code->list.data[ctx->context.ip] == rt->ret_internal) {
        /* Tail call: the very next instruction would just drop this frame
         * anyway, so reuse the slot instead of growing the call stack. */
        ctx->context.ip = 0;
        ctx->context.code = obj;
    } else if (ctx->context.depth) {
        rt->context = rpl_new_context(&rt->gc, obj, ctx->context.names, ctx);
    } else {
        rt->caller = rt->rtcaller;
        char msg[80];
        snprintf(msg, sizeof(msg), "You asked for %d recursions and not a penny more", RPL_CALLDEPTH);
        return rpl_ded(rt, msg);
    }
    return (rpl_thunk){ rpl_context_eval, rt->context };
}

rpl_thunk rpl_newlocall(rpl_runtime *rt, rpl_obj *obj, rpl_obj *names) {
    rpl_obj *ctx = rt->context;
    if (ctx->context.code->list.data[ctx->context.ip] == rt->ret_internal) {
        ctx->context.ip = 0;
        ctx->context.code = obj;
        ctx->context.names = rpl_firstdir(rt, names);
    } else if (ctx->context.depth) {
        rt->context = rpl_new_context(&rt->gc, obj, rpl_firstdir(rt, names), ctx);
    } else {
        rt->caller = rt->rtcaller;
        char msg[80];
        snprintf(msg, sizeof(msg), "You asked for %d recursions and not a penny more", RPL_CALLDEPTH);
        return rpl_ded(rt, msg);
    }
    return (rpl_thunk){ rpl_context_eval, rt->context };
}

rpl_obj *rpl_firstdir(rpl_runtime *rt, rpl_obj *obj) {
    return rpl_new_dir(&rt->gc, rt->nulltag, obj ? obj : rt->lastobj);
}

/* rpl_rcl/rpl_deref/rpl_sto/rpl_rm all walk a directory chain matching each
 * path component against dir.tag->tag.name. Every Tag's name is interned
 * (rpl_new_tag, obj.c), so the match target is always a canonical pointer;
 * interning the incoming query component here too -- rpl_intern() is a
 * hash lookup, not a walk -- turns the per-hop comparison from a strcmp
 * into a pointer compare, independent of how long the name is or how deep
 * the chain gets. The query intern is transient: it's released the moment
 * this component's match is resolved (found or not), whether or not a new
 * tag ends up being created (rpl_new_tag interns its own permanent
 * reference separately, so releasing the transient one here is always
 * correct, never a premature free of the name the new tag holds).
 *
 * On top of that, a chain can carry a dirindex_build()'d index (dir.index
 * -- see dirindex.h) on whichever node MKIDX was called against (only the
 * persistent root context's names head, in practice, and only once MKIDX
 * has run). That's very often *not* the node these functions start
 * scanning from: a query starts at the current context's names, which for
 * anything but the outermost call frame is some ephemeral per-call local
 * scope that simply chains onward (via nulltag boundary nodes -- see
 * dirindex.h's file header) into whatever enclosing scope it was pushed in
 * front of, all the way down to the real root. So the index check has to
 * happen at *every* node the walk visits, not just the one it started at
 * -- otherwise a query that starts in some indexed-nothing local scope and
 * falls through several frames before ever reaching the indexed root would
 * silently never benefit from it (this was a real bug caught by profiling
 * ws.rpl with MKIDX called first and seeing zero difference in rpl_rcl's
 * call count). Once the walk does step onto an indexed node, the rest of
 * that node's span is resolved in O(1) and the walk stops -- the index is
 * exhaustive over everything from where it was built to lastobj, so
 * there's nothing further out there to keep looking for. */

rpl_obj *rpl_rcl(rpl_runtime *rt, char *const *parts, int n) {
    rpl_obj *current = rt->context->context.names;
    for (int i = 0; i < n; i++) {
        if (current->type != RPL_DIRECTORY)
            return NULL;
        char *key = rpl_intern(&rt->gc.names, parts[i]);
        rpl_obj *node = current;
        rpl_obj *found = NULL;
        for (;;) {
            if (node->dir.index) {
                found = dirindex_lookup((rpl_dir_index *)node->dir.index, key);
                break;
            }
            if (node->dir.tag->tag.name == key) {
                found = node;
                break;
            }
            node = node->dir.next;
            if (node == rt->lastobj)
                break;
        }
        rpl_intern_unref(&rt->gc.names, key);
        if (!found)
            return NULL;
        current = found->dir.tag->tag.obj;
    }
    return current;
}

rpl_obj *rpl_deref(rpl_runtime *rt, char *const *parts, int n) {
    rpl_obj *current = rt->context->context.names;
    for (int i = 0; i < n; i++) {
        if (current->type != RPL_DIRECTORY)
            return NULL;
        char *key = rpl_intern(&rt->gc.names, parts[i]);
        rpl_obj *node = current;
        rpl_obj *found = NULL;
        for (;;) {
            if (node->dir.index) {
                found = dirindex_lookup((rpl_dir_index *)node->dir.index, key);
                break;
            }
            if (node->dir.tag->tag.name == key) {
                found = node;
                break;
            }
            node = node->dir.next;
            if (node == rt->lastobj)
                break;
        }
        rpl_intern_unref(&rt->gc.names, key);
        if (!found)
            return NULL;
        if (i + 1 == n)
            return found->dir.tag;
        current = found->dir.tag->tag.obj;
    }
    return NULL;
}

int rpl_sto(rpl_runtime *rt, char *const *parts, int n, rpl_obj *value) {
    int counter = n - 1;
    rpl_obj *current = rt->context->context.names;
    for (int i = 0; i < n; i++) {
        const char *name = parts[i];
        if (current->type != RPL_DIRECTORY)
            return 0;
        char *key = rpl_intern(&rt->gc.names, name);

        /* Walk to either a match, an indexed node (whose index then
         * settles the rest in O(1)), or the true tail (node->dir.next ==
         * lastobj, nothing indexed anywhere in this span). */
        rpl_obj *node = current;
        rpl_dir_index *idx = NULL;
        rpl_obj *found = NULL;
        for (;;) {
            if (node->dir.index) {
                idx = (rpl_dir_index *)node->dir.index;
                found = dirindex_lookup(idx, key);
                break;
            }
            if (node->dir.tag->tag.name == key) {
                found = node;
                break;
            }
            if (node->dir.next == rt->lastobj)
                break; /* node is the tail; found/idx stay NULL */
            node = node->dir.next;
        }

        if (!found) {
            /* Missing: only allowed to append a new leaf entry, and only
             * for the final component -- missing intermediate directories
             * are an error, never auto-created. */
            if (counter) {
                rpl_intern_unref(&rt->gc.names, key);
                return 0;
            }
            rpl_obj *newnode = rpl_new_dir(&rt->gc, rpl_new_tag(&rt->gc, name, value), rt->lastobj);
            if (idx) {
                dirindex_tail(idx)->dir.next = newnode;
                dirindex_append(idx, key, newnode);
            } else {
                node->dir.next = newnode; /* node is the true tail */
            }
            rpl_intern_unref(&rt->gc.names, key);
            return 1;
        }
        rpl_intern_unref(&rt->gc.names, key);
        if (counter) {
            counter--;
            current = found->dir.tag->tag.obj;
        } else {
            /* Final component, already exists -- leave `current` pointing
             * at its directory node so the tag can be overwritten below,
             * rather than descending into its value. */
            current = found;
        }
    }
    current->dir.tag->tag.obj = value;
    return 1;
}

int rpl_rm(rpl_runtime *rt, char *const *parts, int n) {
    rpl_obj *current = rt->context->context.names;
    rpl_obj *last = current;
    rpl_dir_index *idx = NULL; /* index (if any) covering the final match */
    for (int i = 0; i < n; i++) {
        const char *name = parts[i];
        if (current->type != RPL_DIRECTORY || current->dir.next == rt->lastobj)
            return 0;
        char *key = rpl_intern(&rt->gc.names, name);
        /* Removal's own splice-point search stays a plain walk regardless
         * (rare enough that it isn't worth tracking predecessors in the
         * index too -- see dirindex.h); but it still needs to notice
         * whether it ever steps onto an indexed node, purely so the right
         * index can be kept in sync after the splice below. */
        idx = current->dir.index ? (rpl_dir_index *)current->dir.index : NULL;
        while (current->dir.next->dir.tag->tag.name != key) {
            current = current->dir.next;
            if (current->dir.index)
                idx = (rpl_dir_index *)current->dir.index;
            if (current->dir.next == rt->lastobj) {
                rpl_intern_unref(&rt->gc.names, key);
                return 0;
            }
        }
        rpl_intern_unref(&rt->gc.names, key);
        last = current;
        current = current->dir.next->dir.tag->tag.obj;
    }
    /* `last` is the node just before the match; splice the match out. The
     * removed tag's own name is still a live reference (the tag object
     * hasn't been freed, just unlinked), so it's safe to read here for the
     * index update even though our own transient `key` was released above. */
    rpl_obj *removed = last->dir.next;
    last->dir.next = removed->dir.next;

    if (idx) {
        dirindex_remove(idx, removed->dir.tag->tag.name);
        if (dirindex_tail(idx) == removed)
            dirindex_set_tail(idx, last);
    }
    return 1;
}

/* A dotted path plus its length, used only internally by circsym/circdir
 * to track "namelists seen so far" without repeatedly re-deriving lengths. */
typedef struct { char *const *parts; int n; } rpl_path;

/* Every path fed through here comes from a Symbol's .parts (rpl_circsym's
 * own callers) or is built by copying such pointers verbatim (circdir_recurse
 * below, prefixed with dir.tag->tag.name -- also interned); pointer equality
 * is exact, no strcmp needed. */
static int path_eq(rpl_path a, rpl_path b) {
    if (a.n != b.n)
        return 0;
    for (int i = 0; i < a.n; i++)
        if (a.parts[i] != b.parts[i])
            return 0;
    return 1;
}

int rpl_circsym(rpl_runtime *rt, char *const *parts, int n) {
    size_t cap = 8, len = 0;
    rpl_path *seen = xrealloc(NULL, sizeof(rpl_path) * cap);
    seen[len++] = (rpl_path){ parts, n };

    rpl_path cur = { parts, n };
    int circulates = 0;
    for (;;) {
        rpl_obj *symbol = rpl_rcl(rt, cur.parts, cur.n);
        if (!symbol || symbol->type != RPL_SYMBOL)
            break;
        rpl_path next = { symbol->symbol.parts, symbol->symbol.nparts };

        int already_seen = 0;
        for (size_t i = 0; i < len; i++) {
            if (path_eq(seen[i], next)) {
                already_seen = 1;
                break;
            }
        }
        if (already_seen) {
            circulates = 1;
            break;
        }

        if (len == cap) {
            cap *= 2;
            seen = xrealloc(seen, sizeof(rpl_path) * cap);
        }
        seen[len++] = next;
        cur = next;
    }

    free(seen);
    return circulates;
}

static int circdir_recurse(rpl_runtime *rt, char *const *prefix, int prefix_n, rpl_obj *dirtop, int depth) {
    if (!depth)
        return 0;

    dirtop = dirtop->dir.next;
    while (dirtop != rt->lastobj) {
        rpl_obj *val = dirtop->dir.tag->tag.obj;

        if (val->type == RPL_SYMBOL) {
            int combined_n = prefix_n + val->symbol.nparts;
            char **combined = xrealloc(NULL, sizeof(char *) * (size_t)combined_n);
            if (prefix_n)
                memcpy(combined, prefix, sizeof(char *) * (size_t)prefix_n);
            memcpy(combined + prefix_n, val->symbol.parts, sizeof(char *) * (size_t)val->symbol.nparts);
            int hit = rpl_circsym(rt, combined, combined_n);
            free(combined);
            if (hit)
                return 1;
        } else if (val->type == RPL_DIRECTORY) {
            int newprefix_n = prefix_n + 1;
            char **newprefix = xrealloc(NULL, sizeof(char *) * (size_t)newprefix_n);
            if (prefix_n)
                memcpy(newprefix, prefix, sizeof(char *) * (size_t)prefix_n);
            newprefix[prefix_n] = dirtop->dir.tag->tag.name;
            int hit = circdir_recurse(rt, newprefix, newprefix_n, val, depth - 1);
            free(newprefix);
            if (hit)
                return 1;
        }

        dirtop = dirtop->dir.next;
    }
    return 0;
}

int rpl_circdir(rpl_runtime *rt, rpl_obj *dirtop) {
    return circdir_recurse(rt, NULL, 0, dirtop, RPL_CPDEPTH);
}
