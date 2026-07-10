#include "types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* realloc() that aborts on OOM instead of returning NULL (same pattern as
 * the other translation units in this port). */
static void *xrealloc(void *p, size_t n) {
    void *r = realloc(p, n);
    if (n && !r) {
        fprintf(stderr, "rpl: out of memory\n");
        abort();
    }
    return r;
}

/* Mirrors pysys/rtypes.py's symtostr(): joins a dotted path's parts with
 * '.' for use in human-readable error messages. Caller frees the result. */
static char *symtostr(char *const *parts, int n) {
    size_t total = 1;
    for (int i = 0; i < n; i++)
        total += strlen(parts[i]) + 1;
    char *out = xrealloc(NULL, total);
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (i)
            strcat(out, ".");
        strcat(out, parts[i]);
    }
    return out;
}

rpl_thunk rpl_quote_eval(rpl_runtime *rt, rpl_obj *self) {
    rpl_list_push(&rt->gc, rt->stack, self->quote.inner);
    return (rpl_thunk){ rpl_context_eval, rt->context };
}

rpl_thunk rpl_comment_eval(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    return (rpl_thunk){ rpl_context_eval, rt->context };
}

rpl_thunk rpl_tag_usreval(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    return (rpl_thunk){ rpl_context_eval, rt->context };
}

rpl_thunk rpl_tag_eval(rpl_runtime *rt, rpl_obj *self) {
    rpl_list_push(&rt->gc, rt->stack, self);
    return (rpl_thunk){ rpl_tag_usreval, self };
}

rpl_thunk rpl_code_eval(rpl_runtime *rt, rpl_obj *self) {
    return rpl_newcall(rt, self);
}

rpl_thunk rpl_symbol_eval(rpl_runtime *rt, rpl_obj *self) {
    rpl_obj *x = rpl_rcl(rt, self->symbol.parts, self->symbol.nparts);

    if (!x) {
        rt->caller = rt->rtcaller;
        char *name = symtostr(self->symbol.parts, self->symbol.nparts);
        char *msg = xrealloc(NULL, strlen(name) * 2 + 64);
        sprintf(msg, "We seek %s but we cannot always find %s", name, name);
        rpl_thunk t = rpl_ded(rt, msg);
        free(msg);
        free(name);
        return t;
    }

    if (rt->brk) {
        /* Most circular references are caught at store time (rpl_circdir/
         * rpl_circsym), but a ^C arriving mid-lookup lands here too. */
        rt->interrupt = 1;
        rt->brk = 0;
        return rpl_ded(rt, "Break");
    }

    /* Delegate: return the resolved object's own eval thunk, unresolved,
     * exactly like Python's `return x.eval`. */
    return rpl_obj_eval(rt, x);
}

rpl_thunk rpl_builtin_eval(rpl_runtime *rt, rpl_obj *self) {
    /* Preemptively claim responsibility for any error this raises. */
    rt->caller = self;

    int argct = self->builtin.argct;
    int len = rt->stack->list.len;
    if (len < argct) {
        char msg[64];
        snprintf(msg, sizeof(msg), "How about %d arguments instead of %d?", argct, len);
        return rpl_ded(rt, msg);
    }

    int base = len - argct;
    for (int i = 0; i < self->builtin.ndispatches; i++) {
        int match = 1;
        for (int j = 0; j < argct; j++) {
            int want = self->builtin.argck[i * argct + j];
            /* 0 (Any) matches anything; otherwise the type must agree. */
            if (want && want != rt->stack->list.data[base + j]->type) {
                match = 0;
                break;
            }
        }
        if (match)
            return rpl_obj_eval(rt, self->builtin.dispatches[i]);
    }

    char msg[96];
    snprintf(msg, sizeof(msg), "There are %d ways to call and you tried #%d",
             self->builtin.ndispatches, self->builtin.ndispatches + 1);
    return rpl_ded(rt, msg);
}

/* Name table in the exact order pysys/rtypes.py's baseregistry() +
 * updatestore() produce for Types.n -- which is also, by construction, the
 * order the RPL_* enum in rpl.h already uses (see the comment there). */
static const char *const type_names[RPL_NTYPES] = {
    "Any", "Context", "Internal", "Symbol", "Float", "String", "Comment",
    "Builtin", "Directory", "Tag", "List", "Code", "Integer", "Handle", "Quote"
};

void rpl_register_types(rpl_runtime *rt) {
    char *types_path[1] = { "Types" };
    rpl_sto(rt, types_path, 1, rpl_firstdir(rt, NULL));

    rpl_obj *nlist = rpl_new_list(&rt->gc, RPL_LIST);
    for (int i = 0; i < RPL_NTYPES; i++) {
        char *path[2] = { "Types", (char *)type_names[i] };
        rpl_sto(rt, path, 2, rpl_new_int(&rt->gc, i));
        rpl_list_push(&rt->gc, nlist, rpl_new_string(&rt->gc, type_names[i], strlen(type_names[i])));
    }

    char *npath[2] = { "Types", "n" };
    rpl_sto(rt, npath, 2, nlist);
}
