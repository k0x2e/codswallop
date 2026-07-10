/* Mirrors pysys/internals.py: the raw primitive functions that get wrapped
 * as RPL_INTERNAL ("Internal"/typebinproc) objects and stored into the
 * named store (conventionally under "I*") by rpl_stoprocs. builtins.rpl
 * (RPL source, not ported here) wraps each of these with user-level
 * argument-type checking to build the actual Builtin dispatch tables.
 *
 * Each function below has signature rpl_thunk fn(rpl_runtime *, rpl_obj *)
 * to match rpl_eval_fn / what rpl_new_binproc expects; `self` is unused
 * (internals aren't per-object eval overrides), matching how Python's
 * makebinprocs() closures only take `rt`.
 *
 * ##########################################################################
 * Exceptions / deferrals -- called out explicitly, not silently skipped:
 *
 * - File I/O (fopen/fclose/fread/freadline/fwrite/fwriten/feof): ported
 *   using C stdio (fopen/fclose/fread/fgets/fwrite/feof) against
 *   RPL_HANDLE's `handle` FILE* field. Python tracks its own `eof` flag on
 *   the handle object (set manually when a read comes back short); the C
 *   port has nowhere to store that per-handle (RPL_HANDLE is just a bare
 *   FILE*), so `bi_feof` calls libc's feof() directly instead. This is a
 *   reasonable stand-in: stdio's own EOF flag is set by the same read
 *   calls that would have made Python set its flag, but the two aren't
 *   guaranteed to agree bit-for-bit in every corner case (e.g. Python may
 *   observe a "short but non-empty" read as EOF slightly before stdio's
 *   feof() would report true). Error handling shape (push args back, call
 *   ded with the same message) is preserved even though the exact libc
 *   error surface (errno vs. Python exceptions) differs.
 *
 * - `prompt` (console input): uses fgets from stdin. There is no line-
 *   editing (readline/libedit) wired up in csys/ yet -- a plain blocking
 *   read is what's implemented here. Matching Python's fancier readline
 *   story (and its Windows fallback) is future work, not this phase.
 *
 * - `epoch`: uses gettimeofday() rather than time() for closer fidelity to
 *   Python's time.time() (which has sub-second precision); this is a
 *   POSIX-only call, noted here in case portability to non-POSIX targets
 *   ever matters.
 *
 * - `rnd`: implemented with rand()/RAND_MAX. This is a placeholder --
 *   Python uses random.random() (Mersenne Twister). Exact PRNG choice and
 *   seeding strategy are explicitly out of scope for this phase; whoever
 *   cares about statistical quality or reproducibility later should
 *   revisit this.
 *
 * - `evalrom`: now registered (Phase 6 is done -- see csys/rom.{h,c}).
 *   Python's stoprocs() stores this out of pysys/rom.py rather than
 *   pysys/internals.py; rpl_stoprocs mirrors that by pulling in
 *   rpl_evalrom from rom.h and storing it as one extra entry after the
 *   main table, rather than listing it in rpl_internals_table alongside
 *   everything makebinprocs() actually builds.
 *
 * - `regtype` (dynamic user-type registration): Python's rtypes.py has a
 *   dynamic type registry (rpltypes.registerusr) that hands out fresh type
 *   numbers at runtime. The C port's type numbers are the fixed RPL_* enum
 *   in rpl.h (RPL_NTYPES included) -- there is no slack in the object
 *   model for a *new* runtime type number: rpl_obj's union is keyed by a
 *   closed switch in gc.c (gc_mark), obj.c (rpl_cp), runtime.c
 *   (rpl_obj_eval) and types.c (rpl_register_types), none of which can
 *   dispatch on a type number they don't already know about. Extending
 *   this properly would mean either widening the union to carry an
 *   arbitrary "user type" payload plus a runtime-side vtable (eval/cp/mark
 *   overrides looked up by type number), which is a real design task, not
 *   a one-function port. Given that scope, `bi_regtype` here is a stub:
 *   it pops its three arguments (to keep stack shape consistent with a
 *   real call) and reports a clear ded() error rather than silently
 *   pretending to succeed. Flagged again in the Phase 5 report and in
 *   PLAN.md's Phase 5 entry so Phase 6+ doesn't forget this is unfinished.
 * ##########################################################################
 */

#include "internals.h"
#include "types.h"
#include "rom.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* realloc() that aborts on OOM instead of returning NULL (same pattern used
 * throughout csys/). */
static void *xrealloc(void *p, size_t n) {
    void *r = realloc(p, n);
    if (n && !r) {
        fprintf(stderr, "rpl: out of memory\n");
        abort();
    }
    return r;
}

static char *xstrndup(const char *s, size_t n) {
    char *r = xrealloc(NULL, n + 1);
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

static char *xstrdup(const char *s) {
    return xstrndup(s, strlen(s));
}

/* ########################################################################
 * Data-stack helpers. There's no rpl_stack_push/pop in runtime.c yet
 * (Phase 1-3 tests just poke rt->stack->list directly), so these live here.
 *
 * stack_pop mirrors pysys/rtypes.py's typelst.pop(): returns NULL on an
 * empty stack rather than underflowing, exactly like Python's `.pop()`
 * returning None on an empty list. A handful of internals (rpl_local in
 * particular) rely on that None-not-an-exception behavior; most others
 * just call this and immediately dereference the result, which -- like the
 * Python original crashing with AttributeError on None.data -- is a
 * caller-must-not-do-that invariant, not a checked error path. */
static rpl_obj *stack_pop(rpl_runtime *rt) {
    if (rt->stack->list.len == 0)
        return NULL;
    return rt->stack->list.data[--rt->stack->list.len];
}

static void stack_push(rpl_runtime *rt, rpl_obj *o) {
    rpl_list_push(&rt->gc, rt->stack, o);
}

/* 1-indexed from the top: stack_peek(rt, 1) is the top of stack. */
static rpl_obj *stack_peek(rpl_runtime *rt, int fromtop) {
    return rt->stack->list.data[rt->stack->list.len - fromtop];
}

static int stack_len(rpl_runtime *rt) {
    return rt->stack->list.len;
}

static rpl_thunk cont(rpl_runtime *rt) {
    return (rpl_thunk){ rpl_context_eval, rt->context };
}

/* ########################################################################
 * Small shared helpers. */

/* Mirrors pysys/rtypes.py's symtostr(): joins a dotted path with '.' for
 * human-readable error messages. Caller frees the result. */
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

/* Same order as types.c's type_names -- duplicated locally per this
 * project's existing convention of not sharing small file-local tables
 * across translation units. */
static const char *const type_names[RPL_NTYPES] = {
    "Any", "Context", "Internal", "Symbol", "Float", "String", "Comment",
    "Builtin", "Directory", "Tag", "List", "Code", "Integer", "Handle", "Quote"
};

static const char *typename_of(rpl_obj *o) {
    if (o->type < RPL_NTYPES)
        return type_names[o->type];
    return "Unknown";
}

/* Generic "what string represents this object's Caller/Reason identity"
 * helper. Mirrors the fact that in Python, typestr/typebin/typebinproc all
 * happen to expose a `.data` string attribute (nullcaller/rtcaller are
 * typestr; a builtin or internal that claims Caller is itself, exposing
 * its own name string). Anything else falls back to a placeholder, since
 * Python's duck typing doesn't have a clean C equivalent for "any object
 * with a .data that happens to be a string". */
static const char *caller_string(rpl_obj *o) {
    switch (o->type) {
        case RPL_STRING:
        case RPL_COMMENT:
            return o->string.data;
        case RPL_BUILTIN:
            return o->builtin.name;
        case RPL_INTERNAL:
            return o->binproc.name;
        default:
            return "(object)";
    }
}

/* Generic truthiness, mirroring Python's duck-typed `bool(x.data)` used by
 * ift/ifte/and/or/not. Numeric types compare against zero; string-likes and
 * lists/code are truthy iff nonempty; everything else defaults true (a
 * best-effort stand-in for "truthy unless demonstrably empty/zero"). */
static int truthy(rpl_obj *o) {
    switch (o->type) {
        case RPL_INTEGER: return o->integer != 0;
        case RPL_FLOAT:   return o->floating != 0.0;
        case RPL_STRING:
        case RPL_COMMENT: return o->string.len != 0;
        case RPL_LIST:
        case RPL_CODE:    return o->list.len != 0;
        case RPL_SYMBOL:  return o->symbol.nparts != 0;
        default:          return 1;
    }
}

/* Python integer/float modulo: result takes the sign of the divisor (unlike
 * C's %/fmod, which take the sign of the dividend). Used by modint/modfloat
 * to match pysys/internals.py's `y.data % x.data` exactly. */
static int64_t pymod_int(int64_t a, int64_t b) {
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0)))
        r += b;
    return r;
}

static double pymod_float(double a, double b) {
    double r = fmod(a, b);
    if (r != 0 && ((r < 0) != (b < 0)))
        r += b;
    return r;
}

/* Generic numeric-or-string equality, mirroring Python's `x.data == y.data`
 * duck typing (1 == 1.0 is true in Python, hence the int/float cross-check). */
static int data_equal(rpl_obj *a, rpl_obj *b) {
    int a_num = (a->type == RPL_INTEGER || a->type == RPL_FLOAT);
    int b_num = (b->type == RPL_INTEGER || b->type == RPL_FLOAT);
    if (a_num && b_num) {
        double av = a->type == RPL_INTEGER ? (double)a->integer : a->floating;
        double bv = b->type == RPL_INTEGER ? (double)b->integer : b->floating;
        return av == bv;
    }
    if ((a->type == RPL_STRING || a->type == RPL_COMMENT) &&
        (b->type == RPL_STRING || b->type == RPL_COMMENT)) {
        return a->string.len == b->string.len &&
               memcmp(a->string.data, b->string.data, a->string.len) == 0;
    }
    if (a->type == RPL_SYMBOL && b->type == RPL_SYMBOL) {
        if (a->symbol.nparts != b->symbol.nparts)
            return 0;
        for (int i = 0; i < a->symbol.nparts; i++)
            if (strcmp(a->symbol.parts[i], b->symbol.parts[i]) != 0)
                return 0;
        return 1;
    }
    return a == b;
}

/* Generic ordering: 1 if a>b, -1 if a<b, 0 if equal/incomparable. Numeric
 * cross-type (int vs float) and lexicographic string comparison, matching
 * Python's `.data > .data` duck typing for the small set of types ordering
 * comparisons are actually used on. */
static int data_gt(rpl_obj *a, rpl_obj *b) {
    int a_num = (a->type == RPL_INTEGER || a->type == RPL_FLOAT);
    int b_num = (b->type == RPL_INTEGER || b->type == RPL_FLOAT);
    if (a_num && b_num) {
        double av = a->type == RPL_INTEGER ? (double)a->integer : a->floating;
        double bv = b->type == RPL_INTEGER ? (double)b->integer : b->floating;
        return av > bv;
    }
    if ((a->type == RPL_STRING || a->type == RPL_COMMENT) &&
        (b->type == RPL_STRING || b->type == RPL_COMMENT)) {
        size_t n = a->string.len < b->string.len ? a->string.len : b->string.len;
        int c = n ? memcmp(a->string.data, b->string.data, n) : 0;
        if (c != 0)
            return c > 0;
        return a->string.len > b->string.len;
    }
    return 0;
}

/* validatename(): splits text on '.', rejecting empty segments or segments
 * containing a delimiter/whitespace character. Returns a malloc'd parts
 * array (nparts via out param) on success, NULL on failure -- direct port
 * of pysys/parse.py's validatename(). Caller frees the array (and, unlike
 * rpl_new_symbol, the individual char* aren't owned copies -- they point
 * into a single owned buffer freed alongside the array; see the
 * implementation). */
static const char *const validatename_delimiters = "}{:;[]";
static const char *const validatename_whitespace = " \t\r\n";

static char **validatename(const char *text, int *out_n) {
    size_t len = strlen(text);
    char *buf = xstrdup(text);
    int cap = 4, n = 0;
    char **parts = xrealloc(NULL, sizeof(char *) * (size_t)cap);

    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || buf[i] == '.') {
            size_t seglen = i - start;
            if (seglen == 0) {
                free(buf);
                free(parts);
                return NULL;
            }
            for (size_t j = start; j < i; j++) {
                if (strchr(validatename_delimiters, buf[j]) || strchr(validatename_whitespace, buf[j])) {
                    free(buf);
                    free(parts);
                    return NULL;
                }
            }
            if (n == cap) {
                cap *= 2;
                parts = xrealloc(parts, sizeof(char *) * (size_t)cap);
            }
            parts[n] = xstrndup(buf + start, seglen);
            n++;
            start = i + 1;
        }
    }
    free(buf);
    *out_n = n;
    return parts;
}

static void free_parts(char **parts, int n) {
    for (int i = 0; i < n; i++)
        free(parts[i]);
    free(parts);
}

/* ########################################################################
 * Documentation */

static rpl_thunk bi_firstobj(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    stack_push(rt, rt->context->context.names);
    return cont(rt);
}

static rpl_thunk bi_dir(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *obj = stack_pop(rt);
    rpl_obj *n = rpl_new_list(&rt->gc, RPL_LIST);
    rpl_obj *nam = obj->dir.next;
    while (nam != rt->lastobj) {
        if (strlen(nam->dir.tag->tag.name)) {
            char *parts[1] = { nam->dir.tag->tag.name };
            rpl_list_push(&rt->gc, n, rpl_new_symbol(&rt->gc, parts, 1));
        }
        nam = nam->dir.next;
    }
    stack_push(rt, n);
    return cont(rt);
}

static rpl_thunk bi_romid(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *o = stack_pop(rt);
    char *parts[1] = { (char *)caller_string(o) };
    stack_push(rt, rpl_new_symbol(&rt->gc, parts, 1));
    return cont(rt);
}

static rpl_thunk bi_ramid(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *o = stack_pop(rt);
    char idbuf[32];
    snprintf(idbuf, sizeof(idbuf), "%ld", (long)(intptr_t)o);
    char *parts[2] = { "ram", idbuf };
    stack_push(rt, rpl_new_symbol(&rt->gc, parts, 2));
    return cont(rt);
}

static rpl_thunk bi_type(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *o = stack_pop(rt);
    stack_push(rt, rpl_new_int(&rt->gc, o->type));
    return cont(rt);
}

static rpl_thunk bi_self(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    stack_push(rt, rt->context->context.code);
    return cont(rt);
}

static rpl_thunk bi_getcontext(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    stack_push(rt, rt->context);
    return cont(rt);
}

static rpl_thunk bi_setcontext(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rt->context = stack_pop(rt);
    return cont(rt);
}

static rpl_thunk bi_nextcontext(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *o = stack_pop(rt);
    stack_push(rt, o->context.next);
    return cont(rt);
}

static rpl_thunk bi_clrrun(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rt->running = 0;
    return cont(rt);
}

static rpl_thunk bi_errstate(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    const char *callerstr = caller_string(rt->caller);
    const char *reasonstr = rt->reason ? rt->reason : "";
    stack_push(rt, rpl_new_string(&rt->gc, callerstr, strlen(callerstr)));
    stack_push(rt, rpl_new_string(&rt->gc, reasonstr, strlen(reasonstr)));
    stack_push(rt, rpl_new_int(&rt->gc, rt->interrupt));
    rt->caller = rt->nullcaller;
    free(rt->reason);
    rt->reason = NULL;
    rt->interrupt = 0;
    return cont(rt);
}

static rpl_thunk bi_lastcall(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    return rpl_obj_eval(rt, rt->caller);
}

/* ########################################################################
 * Input/Output */

static rpl_thunk bi_fopen(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *options = stack_pop(rt);
    rpl_obj *filename = stack_pop(rt);
    FILE *f = fopen(filename->string.data, options->string.data);
    if (!f) {
        stack_push(rt, filename);
        stack_push(rt, options);
        return rpl_ded(rt, "Perhaps opening this file was a daydream after all");
    }
    stack_push(rt, rpl_new_handle(&rt->gc, f));
    return cont(rt);
}

static rpl_thunk bi_feof(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *h = stack_pop(rt);
    stack_push(rt, rpl_new_int(&rt->gc, feof(h->handle) ? 1 : 0));
    return cont(rt);
}

static rpl_thunk bi_fclose(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *h = stack_pop(rt);
    if (fclose(h->handle) != 0) {
        stack_push(rt, h);
        return rpl_ded(rt, "One can only close a file so hard");
    }
    return cont(rt);
}

static rpl_thunk bi_freadline(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *h = stack_pop(rt);
    enum { MAXREAD = 256000 };
    char *buf = xrealloc(NULL, MAXREAD + 1);
    clearerr(h->handle);
    char *got = fgets(buf, MAXREAD + 1, h->handle);
    if (!got && ferror(h->handle)) {
        free(buf);
        stack_push(rt, h);
        return rpl_ded(rt, "You may read a book, but not this file");
    }
    size_t len = got ? strlen(got) : 0;
    if (len && buf[len - 1] == '\n')
        len--;
    stack_push(rt, rpl_new_string(&rt->gc, buf, len));
    free(buf);
    return cont(rt);
}

static rpl_thunk bi_fread(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *chars = stack_pop(rt);
    rpl_obj *h = stack_pop(rt);
    enum { MAXREAD = 256000 };
    int64_t request = chars->integer;
    if (request < 1 || request > MAXREAD)
        request = MAXREAD;
    char *buf = xrealloc(NULL, (size_t)request);
    clearerr(h->handle);
    size_t got = fread(buf, 1, (size_t)request, h->handle);
    if (got == 0 && ferror(h->handle)) {
        free(buf);
        stack_push(rt, h);
        stack_push(rt, chars);
        return rpl_ded(rt, "You may read a book, but not this file");
    }
    stack_push(rt, rpl_new_string(&rt->gc, buf, got));
    free(buf);
    return cont(rt);
}

static rpl_thunk bi_fwriten(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *h = stack_pop(rt);
    rpl_obj *text = stack_pop(rt);
    size_t wrote = fwrite(text->string.data, 1, text->string.len, h->handle);
    if (wrote != text->string.len) {
        stack_push(rt, text);
        stack_push(rt, h);
        return rpl_ded(rt, "You may write a friend, but not this file");
    }
    return cont(rt);
}

static rpl_thunk bi_fwrite(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *h = stack_pop(rt);
    rpl_obj *text = stack_pop(rt);
    size_t wrote = fwrite(text->string.data, 1, text->string.len, h->handle);
    int ok = (wrote == text->string.len) && (fputc('\n', h->handle) != EOF);
    if (!ok) {
        stack_push(rt, text);
        stack_push(rt, h);
        return rpl_ded(rt, "You may write a friend, but not this file");
    }
    return cont(rt);
}

static rpl_thunk bi_disp(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *o = stack_pop(rt);
    switch (o->type) {
        case RPL_STRING:
        case RPL_COMMENT:
            printf("%.*s\n", (int)o->string.len, o->string.data);
            break;
        case RPL_INTEGER:
            printf("%lld\n", (long long)o->integer);
            break;
        case RPL_FLOAT:
            printf("%g\n", o->floating);
            break;
        default:
            printf("(%s)\n", typename_of(o));
            break;
    }
    return cont(rt);
}

static rpl_thunk bi_dispn(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *o = stack_pop(rt);
    switch (o->type) {
        case RPL_STRING:
        case RPL_COMMENT:
            printf("%.*s", (int)o->string.len, o->string.data);
            break;
        case RPL_INTEGER:
            printf("%lld", (long long)o->integer);
            break;
        case RPL_FLOAT:
            printf("%g", o->floating);
            break;
        default:
            printf("(%s)", typename_of(o));
            break;
    }
    return cont(rt);
}

/* Mirrors pysys/internals.py's prompt() exactly, including the SIGINT
 * story: Python sets dieanyway, then a SIGINT during the blocked input()
 * raises KeyboardInterrupt there, caught by prompt's bare `except:`, which
 * pushes the prompt string back, clears Break, and calls ded() -- the same
 * path an ordinary read failure (e.g. Ctrl-D/EOFError) takes. C has no
 * exception to unwind a blocked fgets() with, so main.c's SIGINT handler
 * instead calls siglongjmp(rt->intr_buf, 1) when dieanyway is set, landing
 * back here via the sigsetjmp() below as if the read had failed -- both
 * paths (real EOF/error and a SIGINT-driven jump) funnel into the same
 * ded() call with the same message, matching Python's single except-clause
 * handling both cases identically. */
static rpl_thunk bi_prompt(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *volatile promptobj = stack_pop(rt);
    printf("%.*s", (int)promptobj->string.len, promptobj->string.data);
    fflush(stdout);

    enum { MAXLINE = 65536 };
    char *volatile buf = xrealloc(NULL, MAXLINE);

    rt->dieanyway = 1;
    if (sigsetjmp(rt->intr_buf, 1)) {
        /* Arrived here via siglongjmp from catchsigint, not a normal
         * return from sigsetjmp -- a SIGINT landed while fgets below was
         * blocked. */
        rt->intr_buf_active = 0;
        rt->dieanyway = 0;
        free(buf);
        stack_push(rt, promptobj);
        rt->brk = 0;
        return rpl_ded(rt, "The user has typed unforgivably");
    }
    rt->intr_buf_active = 1;
    char *got = fgets(buf, MAXLINE, stdin);
    rt->intr_buf_active = 0;
    rt->dieanyway = 0;

    if (!got) {
        free(buf);
        stack_push(rt, promptobj);
        rt->brk = 0;
        return rpl_ded(rt, "The user has typed unforgivably");
    }
    size_t len = strlen(got);
    if (len && got[len - 1] == '\n')
        len--;
    stack_push(rt, rpl_new_string(&rt->gc, got, len));
    free(buf);
    return cont(rt);
}

static rpl_thunk bi_epoch(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    stack_push(rt, rpl_new_float(&rt->gc, (double)tv.tv_sec + (double)tv.tv_usec / 1e6));
    return cont(rt);
}

/* ########################################################################
 * Stack manipulation */

static rpl_thunk bi_stack(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    stack_push(rt, rpl_cp(&rt->gc, rt->stack));
    return cont(rt);
}

static rpl_thunk bi_drop(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    stack_pop(rt);
    return cont(rt);
}

static rpl_thunk bi_dropn(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *lines = stack_pop(rt);
    int len = stack_len(rt);
    if (lines->integer < 0 || lines->integer > len) {
        stack_push(rt, lines);
        return rpl_ded(rt, "That is not a reasonable number of lines to drop");
    }
    rt->stack->list.len = len - (int)lines->integer;
    return cont(rt);
}

static rpl_thunk bi_pick(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *line = stack_pop(rt);
    int len = stack_len(rt);
    if (line->integer <= 0 || line->integer > len) {
        stack_push(rt, line);
        return rpl_ded(rt, "A pick beyond one's reach");
    }
    stack_push(rt, rt->stack->list.data[len - (int)line->integer]);
    return cont(rt);
}

static rpl_thunk bi_over(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    stack_push(rt, stack_peek(rt, 2));
    return cont(rt);
}

static rpl_thunk bi_eval(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    return rpl_obj_eval(rt, stack_pop(rt));
}

static rpl_thunk bi_swap(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    stack_push(rt, x);
    stack_push(rt, y);
    return cont(rt);
}

static rpl_thunk bi_dup(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    stack_push(rt, stack_peek(rt, 1));
    return cont(rt);
}

static rpl_thunk bi_dup2(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *a = stack_peek(rt, 2);
    rpl_obj *b = stack_peek(rt, 1);
    stack_push(rt, a);
    stack_push(rt, b);
    return cont(rt);
}

static rpl_thunk bi_dupn(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *count = stack_pop(rt);
    int len = stack_len(rt);
    if (count->integer > 0 && count->integer <= len) {
        int n = (int)count->integer;
        rpl_obj **saved = xrealloc(NULL, sizeof(rpl_obj *) * (size_t)n);
        for (int i = 0; i < n; i++)
            saved[i] = rt->stack->list.data[len - n + i];
        for (int i = 0; i < n; i++)
            stack_push(rt, saved[i]);
        free(saved);
    } else {
        stack_push(rt, count);
        return rpl_ded(rt, "Duplicate how many things now");
    }
    return cont(rt);
}

static rpl_thunk bi_rot(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    int n = stack_len(rt);
    rpl_obj **d = rt->stack->list.data;
    rpl_obj *a = d[n - 3];
    d[n - 3] = d[n - 2];
    d[n - 2] = d[n - 1];
    d[n - 1] = a;
    return cont(rt);
}

static rpl_thunk bi_rotd(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    int n = stack_len(rt);
    rpl_obj **d = rt->stack->list.data;
    rpl_obj *c = d[n - 1];
    d[n - 1] = d[n - 2];
    d[n - 2] = d[n - 3];
    d[n - 3] = c;
    return cont(rt);
}

static rpl_thunk bi_roll(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *qty = stack_pop(rt);
    if (stack_len(rt) < qty->integer) {
        stack_push(rt, qty);
        return rpl_ded(rt, "Your katamari is not big enough to roll this much");
    }
    if (qty->integer > 0) {
        int n = stack_len(rt);
        int idx = n - (int)qty->integer;
        rpl_obj **d = rt->stack->list.data;
        rpl_obj *v = d[idx];
        for (int i = idx; i < n - 1; i++)
            d[i] = d[i + 1];
        d[n - 1] = v;
    }
    return cont(rt);
}

static rpl_thunk bi_rolld(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *qty = stack_pop(rt);
    if (stack_len(rt) < qty->integer) {
        stack_push(rt, qty);
        return rpl_ded(rt, "Your katamari is not big enough to roll this much");
    }
    if (qty->integer > 0) {
        int n = stack_len(rt);
        int idx = n - (int)qty->integer;
        rpl_obj **d = rt->stack->list.data;
        rpl_obj *v = d[n - 1];
        for (int i = n - 1; i > idx; i--)
            d[i] = d[i - 1];
        d[idx] = v;
    }
    return cont(rt);
}

static rpl_thunk bi_require(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *qty = stack_pop(rt);
    if (stack_len(rt) < qty->integer) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Successful persons have %lld or more objects on the stack",
                 (long long)qty->integer);
        stack_push(rt, qty);
        return rpl_ded(rt, msg);
    }
    return cont(rt);
}

/* ########################################################################
 * Named storage */

static rpl_thunk bi_rcl(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *sym = stack_pop(rt);
    rpl_obj *obj = rpl_rcl(rt, sym->symbol.parts, sym->symbol.nparts);
    if (!obj) {
        stack_push(rt, sym);
        char *name = symtostr(sym->symbol.parts, sym->symbol.nparts);
        char *msg = xrealloc(NULL, strlen(name) + 32);
        sprintf(msg, "What even is %s", name);
        rpl_thunk t = rpl_ded(rt, msg);
        free(msg);
        free(name);
        return t;
    }
    stack_push(rt, obj);
    return cont(rt);
}

static rpl_thunk bi_rclfrom(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *tag = stack_pop(rt);
    stack_push(rt, tag->tag.obj);
    return cont(rt);
}

/* Shared "undo" tail for sto: restores either the original binding (og,
 * if there was one) or erases the newly-added name, then pushes the two
 * originally-popped operands back (matching Python's usded closure) and
 * reports reason via ded(). */
static rpl_thunk sto_usded(rpl_runtime *rt, rpl_obj *og, rpl_obj *one, rpl_obj *two,
                            char *const *nameparts, int namen, const char *reason) {
    stack_push(rt, one);
    stack_push(rt, two);
    if (og)
        rpl_sto(rt, nameparts, namen, og);
    else
        rpl_rm(rt, nameparts, namen);
    return rpl_ded(rt, reason);
}

static rpl_thunk bi_sto(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *name = stack_pop(rt);
    rpl_obj *obj = stack_pop(rt);
    rpl_obj *og = rpl_rcl(rt, name->symbol.parts, name->symbol.nparts);

    if (!rpl_sto(rt, name->symbol.parts, name->symbol.nparts, obj))
        return sto_usded(rt, NULL, obj, name, name->symbol.parts, name->symbol.nparts,
                          "To store to a directory, first the directory must exist");

    if (obj->type == RPL_DIRECTORY && rpl_circdir(rt, obj))
        return sto_usded(rt, og, obj, name, name->symbol.parts, name->symbol.nparts,
                          "That directory contains circular references");
    if (obj->type == RPL_SYMBOL && rpl_circsym(rt, obj->symbol.parts, obj->symbol.nparts))
        return sto_usded(rt, og, obj, name, name->symbol.parts, name->symbol.nparts,
                          "cDonalds Theorem does not apply to symbolic references");
    return cont(rt);
}

static rpl_thunk bi_stoto(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *tag = stack_pop(rt);
    rpl_obj *thing = stack_pop(rt);
    if (thing->type == RPL_SYMBOL) {
        rpl_obj *original = tag->tag.obj;
        tag->tag.obj = thing;
        if (rpl_circsym(rt, thing->symbol.parts, thing->symbol.nparts)) {
            tag->tag.obj = original;
            stack_push(rt, thing);
            stack_push(rt, tag);
            return rpl_ded(rt, "A valiant effort to reference oneself, thwarted");
        }
    } else if (thing == tag) {
        stack_push(rt, thing);
        stack_push(rt, tag);
        return rpl_ded(rt, "A valiant effort to reference oneself, thwarted");
    } else {
        tag->tag.obj = thing;
    }
    return cont(rt);
}

static rpl_thunk bi_deref(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *sym = stack_pop(rt);
    rpl_obj *obj = rpl_deref(rt, sym->symbol.parts, sym->symbol.nparts);
    if (!obj) {
        stack_push(rt, sym);
        return rpl_ded(rt, "It is difficult to dereference what does not exist");
    }
    stack_push(rt, obj);
    return cont(rt);
}

static rpl_thunk bi_exists(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *sym = stack_pop(rt);
    rpl_obj *obj = rpl_rcl(rt, sym->symbol.parts, sym->symbol.nparts);
    stack_push(rt, rpl_new_int(&rt->gc, obj != NULL));
    return cont(rt);
}

static rpl_thunk bi_rm(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    if (!rpl_rm(rt, x->symbol.parts, x->symbol.nparts)) {
        stack_push(rt, x);
        return rpl_ded(rt, "You have failed to erase what isn't here!");
    }
    return cont(rt);
}

static rpl_thunk bi_cp(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    stack_push(rt, rpl_cp(&rt->gc, stack_pop(rt)));
    return cont(rt);
}

static rpl_thunk bi_id(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *o = stack_pop(rt);
    stack_push(rt, rpl_new_int(&rt->gc, (int64_t)(intptr_t)o));
    return cont(rt);
}

static rpl_thunk bi_tlocal(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *prog = stack_pop(rt);
    rpl_obj *tag = stack_pop(rt);
    rpl_obj *names = rpl_new_dir(&rt->gc, tag, rt->context->context.names);
    return rpl_newlocall(rt, prog, names);
}

/* See the file-header comment block for why this is a stub rather than a
 * real implementation: the C port's type numbers are the fixed RPL_* enum,
 * and there's no vtable-per-type-number mechanism yet for a genuinely new
 * user type to plug into gc_mark/rpl_cp/rpl_obj_eval. */
static rpl_thunk bi_regtype(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *name = stack_pop(rt);
    rpl_obj *usreval = stack_pop(rt);
    rpl_obj *proto = stack_pop(rt);
    stack_push(rt, proto);
    stack_push(rt, usreval);
    stack_push(rt, name);
    return rpl_ded(rt, "User-defined types are not yet supported in the C port (regtype)");
}

/* Local variable context. The most complex internal in this file -- see
 * PLAN.md's Phase 5 entry and pysys/internals.py's `local` (~lines
 * 514-581), which this is a line-for-line translation of, bug-for-bug
 * (see the comment on the `else` branch below).
 *
 * Strategy: snapshot the stack length and the current context up front.
 * Build a throwaway context whose only real purpose is to hold `.names`
 * as we thread a chain of new typedir/typetag nodes through it while
 * running circulation checks (rpl_circsym/rpl_circdir) after each binding.
 * On any failure, reset rt->stack->list.len back to the snapshot (safe
 * because nothing is pushed between the snapshot and any failure path --
 * only popped) and rt->context back to the original, then report the
 * error via rpl_ded. On success, restore rt->context and either
 * rpl_newcall (if no names actually got bound -- names_obj was empty or
 * turned out to be a no-op) or rpl_newlocall with the built chain. */
static rpl_thunk bi_local(rpl_runtime *rt, rpl_obj *self) {
    (void)self;

    int origlen = stack_len(rt);
    rpl_obj *origcontext = rt->context;
    gc_root_push(&rt->gc, &origcontext);

    rpl_obj *names_obj = stack_pop(rt);
    rpl_obj *prog = stack_pop(rt);
    gc_root_push(&rt->gc, &names_obj);
    gc_root_push(&rt->gc, &prog);

    rt->context = rpl_new_context(&rt->gc, prog, origcontext->context.names, NULL);

    rpl_obj *nextob = origcontext->context.names;
    gc_root_push(&rt->gc, &nextob);

    char *circname_buf[1] = { NULL };
    char **circparts = NULL;
    int circn = 0;

    for (int idx = 0; idx < names_obj->list.len; idx++) {
        rpl_obj *item = names_obj->list.data[idx];

        if (item->type == RPL_SYMBOL) {
            if (item->symbol.nparts > 1) {
                rt->context = origcontext;
                rt->stack->list.len = origlen;
                gc_root_pop(&rt->gc, 4);
                return rpl_ded(rt, "Ain't no dots in local variable names");
            }
            rpl_obj *thisob = stack_pop(rt);
            if (thisob != NULL) {
                rpl_obj *tag = rpl_new_tag(&rt->gc, item->symbol.parts[0], thisob);
                nextob = rpl_new_dir(&rt->gc, tag, nextob);
                circname_buf[0] = item->symbol.parts[0];
                circparts = circname_buf;
                circn = 1;
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg), "You gotta have %d things on the stack!", names_obj->list.len);
                rt->context = origcontext;
                rt->stack->list.len = origlen;
                gc_root_pop(&rt->gc, 4);
                return rpl_ded(rt, msg);
            }
        } else if (item->type == RPL_TAG) {
            circname_buf[0] = item->tag.name;
            circparts = circname_buf;
            circn = 1;
            nextob = rpl_new_dir(&rt->gc, rpl_cp(&rt->gc, item), nextob);
        } else if (item->type == RPL_COMMENT) {
            /* Suppressed: repeats the last circulation check below with
             * whatever circparts/nextob were left by the previous
             * iteration (matches Python's comment about this exactly). */
        } else {
            /* Faithful bug-for-bug port: pysys/internals.py's `local` is
             * missing a `return` before this usded() call:
             *   else: usded("Only symbols and tags lead to success")
             * usded() itself (a *different*, one-argument nested closure
             * from the one in bi_sto/sto_usded above -- see pysys's local()
             * lines 517-520) unconditionally resets rt.Context/rt.Stack
             * and calls rt.ded(reason), but since the call above is missing
             * `return`, that reset-and-ded happens as a side effect and
             * then the loop just keeps going, now operating on whatever
             * context ded() left behind (the error-handler-wrapping
             * context) instead of the temporary one being built. This is
             * clearly an accidental bug in the reference, but it's
             * reproduced here on purpose, not an oversight -- PLAN.md and
             * this function's docstring call it out explicitly. */
            rt->context = origcontext;
            rt->stack->list.len = origlen;
            rpl_ded(rt, "Only symbols and tags lead to success"); /* return value intentionally discarded */
        }

        rt->context->context.names = nextob;
        if (nextob->dir.tag->tag.obj->type == RPL_SYMBOL) {
            if (circn && rpl_circsym(rt, circparts, circn)) {
                char *nm = symtostr(circparts, circn);
                char *msg = xrealloc(NULL, strlen(nm) * 3 + 64);
                sprintf(msg, "Round and round the %s bush the %s chased the %s", nm, nm, nm);
                rt->context = origcontext;
                rt->stack->list.len = origlen;
                gc_root_pop(&rt->gc, 4);
                rpl_thunk t = rpl_ded(rt, msg);
                free(msg);
                free(nm);
                return t;
            }
        } else if (nextob->dir.tag->tag.obj->type == RPL_DIRECTORY) {
            if (rpl_circdir(rt, nextob->dir.tag->tag.obj)) {
                rt->context = origcontext;
                rt->stack->list.len = origlen;
                gc_root_pop(&rt->gc, 4);
                return rpl_ded(rt, "This directory circulates if you put it there");
            }
        }
    }

    nextob = rt->context->context.names;
    rt->context = origcontext;
    gc_root_pop(&rt->gc, 4);

    if (nextob == origcontext->context.names)
        return rpl_newcall(rt, prog);
    return rpl_newlocall(rt, prog, nextob);
}

/* ########################################################################
 * Flow control */

static rpl_thunk bi_evalnext(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *next = stack_pop(rt);
    rpl_thunk this = rpl_context_eval(rt, rt->context);
    for (;;) {
        if (this.fn == rpl_context_eval && this.self == rt->context)
            break;
        this = this.fn(rt, this.self);
    }
    return rpl_obj_eval(rt, next);
}

static rpl_thunk bi_bail(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    if (rt->context->context.ip + 1 == rt->context->context.code->list.len)
        rpl_ret(rt, NULL);
    return rpl_ret(rt, NULL);
}

static rpl_thunk bi_beval(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    if (rt->context->context.ip + 1 == rt->context->context.code->list.len)
        rpl_ret(rt, NULL);
    rpl_ret(rt, NULL);
    rt->running = 1;
    return rpl_obj_eval(rt, stack_pop(rt));
}

static rpl_thunk bi_ift(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *th = stack_pop(rt);
    rpl_obj *cond = stack_pop(rt);
    if (truthy(cond))
        return rpl_obj_eval(rt, th);
    return cont(rt);
}

static rpl_thunk bi_ifte(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *el = stack_pop(rt);
    rpl_obj *th = stack_pop(rt);
    rpl_obj *cond = stack_pop(rt);
    return rpl_obj_eval(rt, truthy(cond) ? th : el);
}

static rpl_thunk bi_rst(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rt->context->context.ip = 0;
    return cont(rt);
}

/* ########################################################################
 * Mathemagics */

static rpl_thunk bi_odd(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    stack_push(rt, rpl_new_int(&rt->gc, (x->integer % 2) != 0));
    return cont(rt);
}

static rpl_thunk bi_absint(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    stack_push(rt, rpl_new_int(&rt->gc, x->integer < 0 ? -x->integer : x->integer));
    return cont(rt);
}

static rpl_thunk bi_absfloat(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    stack_push(rt, rpl_new_float(&rt->gc, fabs(x->floating)));
    return cont(rt);
}

static rpl_thunk bi_modfloat(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    if (x->floating) {
        stack_push(rt, rpl_new_float(&rt->gc, pymod_float(y->floating, x->floating)));
    } else {
        stack_push(rt, y);
        stack_push(rt, x);
        return rpl_ded(rt, "Excuse you");
    }
    return cont(rt);
}

static rpl_thunk bi_modint(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    if (x->integer) {
        stack_push(rt, rpl_new_int(&rt->gc, pymod_int(y->integer, x->integer)));
    } else {
        stack_push(rt, y);
        stack_push(rt, x);
        return rpl_ded(rt, "Excuse you");
    }
    return cont(rt);
}

static rpl_thunk bi_addfloat(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    double x = stack_pop(rt)->floating;
    double y = stack_pop(rt)->floating;
    stack_push(rt, rpl_new_float(&rt->gc, x + y));
    return cont(rt);
}

static rpl_thunk bi_addint(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    int64_t x = stack_pop(rt)->integer;
    int64_t y = stack_pop(rt)->integer;
    stack_push(rt, rpl_new_int(&rt->gc, x + y));
    return cont(rt);
}

static rpl_thunk bi_addstr(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    size_t len = x->string.len + y->string.len;
    char *buf = xrealloc(NULL, len ? len : 1);
    memcpy(buf, y->string.data, y->string.len);
    memcpy(buf + y->string.len, x->string.data, x->string.len);
    stack_push(rt, rpl_new_string(&rt->gc, buf, len));
    free(buf);
    return cont(rt);
}

static rpl_thunk bi_addlist(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *src = stack_pop(rt);
    rpl_obj *dest = rpl_cp(&rt->gc, stack_pop(rt));
    rpl_list_push(&rt->gc, dest, src);
    stack_push(rt, dest);
    return cont(rt);
}

static rpl_thunk bi_listadd(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = rpl_cp(&rt->gc, stack_pop(rt));
    rpl_obj *front = rpl_cp(&rt->gc, stack_pop(rt));
    rpl_obj *n = rpl_new_list(&rt->gc, x->type);
    rpl_list_push(&rt->gc, n, front);
    for (int i = 0; i < x->list.len; i++)
        rpl_list_push(&rt->gc, n, x->list.data[i]);
    stack_push(rt, n);
    return cont(rt);
}

static rpl_thunk bi_catlist(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    rpl_obj *n = rpl_new_list(&rt->gc, RPL_LIST);
    for (int i = 0; i < y->list.len; i++)
        rpl_list_push(&rt->gc, n, y->list.data[i]);
    for (int i = 0; i < x->list.len; i++)
        rpl_list_push(&rt->gc, n, x->list.data[i]);
    stack_push(rt, n);
    return cont(rt);
}

static rpl_thunk bi_catcode(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    rpl_obj *n = rpl_new_list(&rt->gc, RPL_CODE);
    for (int i = 0; i < y->list.len - 1; i++)
        rpl_list_push(&rt->gc, n, y->list.data[i]);
    for (int i = 0; i < x->list.len; i++)
        rpl_list_push(&rt->gc, n, x->list.data[i]);
    stack_push(rt, n);
    return cont(rt);
}

static rpl_thunk bi_addsym(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    int n = y->symbol.nparts + x->symbol.nparts;
    char **parts = xrealloc(NULL, sizeof(char *) * (size_t)(n ? n : 1));
    for (int i = 0; i < y->symbol.nparts; i++)
        parts[i] = y->symbol.parts[i];
    for (int i = 0; i < x->symbol.nparts; i++)
        parts[y->symbol.nparts + i] = x->symbol.parts[i];
    stack_push(rt, rpl_new_symbol(&rt->gc, parts, n));
    free(parts);
    return cont(rt);
}

static rpl_thunk bi_powfloat(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    double r = pow(y->floating, x->floating);
    if (!isfinite(r)) {
        stack_push(rt, y);
        stack_push(rt, x);
        return rpl_ded(rt, "This produces an intolerably high number");
    }
    stack_push(rt, rpl_new_float(&rt->gc, r));
    return cont(rt);
}

static rpl_thunk bi_powint(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    double r = pow((double)y->integer, (double)x->integer);
    if (!isfinite(r) || r > 9.2e18 || r < -9.2e18) {
        stack_push(rt, y);
        stack_push(rt, x);
        return rpl_ded(rt, "This produces an intolerably high number");
    }
    stack_push(rt, rpl_new_int(&rt->gc, (int64_t)r));
    return cont(rt);
}

static rpl_thunk bi_mulfloat(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    double x = stack_pop(rt)->floating;
    double y = stack_pop(rt)->floating;
    stack_push(rt, rpl_new_float(&rt->gc, x * y));
    return cont(rt);
}

static rpl_thunk bi_mulint(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    int64_t x = stack_pop(rt)->integer;
    int64_t y = stack_pop(rt)->integer;
    stack_push(rt, rpl_new_int(&rt->gc, x * y));
    return cont(rt);
}

static rpl_thunk bi_mulstr(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    if (x->integer >= 0) {
        size_t len = y->string.len * (size_t)x->integer;
        char *buf = xrealloc(NULL, len ? len : 1);
        for (int64_t i = 0; i < x->integer; i++)
            memcpy(buf + (size_t)i * y->string.len, y->string.data, y->string.len);
        stack_push(rt, rpl_new_string(&rt->gc, buf, len));
        free(buf);
    } else {
        stack_push(rt, y);
        stack_push(rt, x);
        return rpl_ded(rt, "wat");
    }
    return cont(rt);
}

static rpl_thunk bi_mullst(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    if (x->integer >= 0) {
        rpl_obj *z = rpl_new_list(&rt->gc, RPL_LIST);
        for (int64_t i = 0; i < x->integer; i++)
            for (int j = 0; j < y->list.len; j++)
                rpl_list_push(&rt->gc, z, y->list.data[j]);
        stack_push(rt, z);
    } else {
        stack_push(rt, y);
        stack_push(rt, x);
        return rpl_ded(rt, "wat");
    }
    return cont(rt);
}

static rpl_thunk bi_mulcode(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    if (x->integer >= 0) {
        rpl_obj *z = rpl_new_list(&rt->gc, RPL_CODE);
        for (int64_t i = 0; i < x->integer; i++)
            for (int j = 0; j < y->list.len - 1; j++)
                rpl_list_push(&rt->gc, z, y->list.data[j]);
        rpl_list_push(&rt->gc, z, rt->ret_internal);
        stack_push(rt, z);
    } else {
        stack_push(rt, y);
        stack_push(rt, x);
        return rpl_ded(rt, "wat");
    }
    return cont(rt);
}

static rpl_thunk bi_subfloat(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    double x = stack_pop(rt)->floating;
    double y = stack_pop(rt)->floating;
    stack_push(rt, rpl_new_float(&rt->gc, -x + y));
    return cont(rt);
}

static rpl_thunk bi_subint(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    int64_t x = stack_pop(rt)->integer;
    int64_t y = stack_pop(rt)->integer;
    stack_push(rt, rpl_new_int(&rt->gc, -x + y));
    return cont(rt);
}

static rpl_thunk bi_divfloat(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    if (x->floating) {
        stack_push(rt, rpl_new_float(&rt->gc, y->floating / x->floating));
    } else {
        stack_push(rt, y);
        stack_push(rt, x);
        return rpl_ded(rt, "Excuse you");
    }
    return cont(rt);
}

static rpl_thunk bi_divint(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    if (x->integer) {
        stack_push(rt, rpl_new_int(&rt->gc, y->integer / x->integer));
    } else {
        stack_push(rt, y);
        stack_push(rt, x);
        return rpl_ded(rt, "Excuse you");
    }
    return cont(rt);
}

static rpl_thunk bi_neg(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *o = stack_pop(rt);
    if (o->type == RPL_FLOAT)
        stack_push(rt, rpl_new_float(&rt->gc, -o->floating));
    else
        stack_push(rt, rpl_new_int(&rt->gc, -o->integer));
    return cont(rt);
}

static rpl_thunk bi_rnd(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    stack_push(rt, rpl_new_float(&rt->gc, (double)rand() / ((double)RAND_MAX + 1.0)));
    return cont(rt);
}

static rpl_thunk bi_ip(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *o = stack_pop(rt);
    stack_push(rt, rpl_new_float(&rt->gc, trunc(o->floating)));
    return cont(rt);
}

/* ########################################################################
 * Conversions */

static rpl_thunk bi_toquote(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    stack_push(rt, rpl_new_quote(&rt->gc, stack_pop(rt)));
    return cont(rt);
}

static rpl_thunk bi_totag(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *name = stack_pop(rt);
    rpl_obj *obj = stack_pop(rt);
    if (name->symbol.nparts > 1) {
        stack_push(rt, obj);
        stack_push(rt, name);
        return rpl_ded(rt, "Tags don't have last names");
    }
    stack_push(rt, rpl_new_tag(&rt->gc, name->symbol.parts[0], obj));
    return cont(rt);
}

static rpl_thunk bi_tobin(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    int origlen = stack_len(rt);
    rpl_obj *name = stack_pop(rt);
    rpl_obj *hint = stack_pop(rt);
    rpl_obj *argct = stack_pop(rt);
    if (argct->integer < 0) {
        rt->stack->list.len = origlen;
        return rpl_ded(rt, "It's hard to win a negative argument");
    }
    rpl_obj *bin = rpl_new_builtin(&rt->gc, name->symbol.parts[0], hint->string.data, (int)argct->integer);
    stack_push(rt, bin);
    return cont(rt);
}

static rpl_thunk bi_binfrom(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *bin = stack_pop(rt);
    rpl_obj *table = rpl_new_list(&rt->gc, RPL_LIST);
    for (int i = 0; i < bin->builtin.ndispatches; i++) {
        rpl_obj *line = rpl_new_list(&rt->gc, RPL_LIST);
        rpl_list_push(&rt->gc, line, bin->builtin.dispatches[i]);
        for (int j = 0; j < bin->builtin.argct; j++)
            rpl_list_push(&rt->gc, line, rpl_new_int(&rt->gc, bin->builtin.argck[i * bin->builtin.argct + j]));
        rpl_list_push(&rt->gc, table, line);
    }
    stack_push(rt, table);
    stack_push(rt, rpl_new_int(&rt->gc, bin->builtin.argct));
    stack_push(rt, rpl_new_string(&rt->gc, bin->builtin.hint ? bin->builtin.hint : "", bin->builtin.hint ? strlen(bin->builtin.hint) : 0));
    char *parts[1] = { bin->builtin.name };
    stack_push(rt, rpl_new_symbol(&rt->gc, parts, 1));
    return cont(rt);
}

static rpl_thunk bi_setdispatch(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *table = stack_pop(rt);
    rpl_obj *bin = stack_pop(rt);

    free(bin->builtin.dispatches);
    free(bin->builtin.argck);
    bin->builtin.dispatches = NULL;
    bin->builtin.argck = NULL;
    bin->builtin.ndispatches = 0;

    for (int i = 0; i < table->list.len; i++) {
        rpl_obj *line = table->list.data[i];
        int *row = xrealloc(NULL, sizeof(int) * (size_t)bin->builtin.argct);
        for (int j = 0; j < bin->builtin.argct; j++)
            row[j] = (int)line->list.data[j + 1]->integer;
        rpl_builtin_add_dispatch(&rt->gc, bin, row, line->list.data[0]);
        free(row);
    }
    return cont(rt);
}

static rpl_thunk bi_binhook(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    int origlen = stack_len(rt);
    rpl_obj *bin = stack_pop(rt);
    rpl_obj *patches = stack_pop(rt);
    int argct = bin->builtin.argct;

    int cap = patches->list.len > 0 ? patches->list.len : 1;
    rpl_obj **newdispatches = xrealloc(NULL, sizeof(rpl_obj *) * (size_t)cap);
    int *newargck = xrealloc(NULL, sizeof(int) * (size_t)argct * (size_t)cap);
    int ncount = 0;

    for (int idx = 0; idx < patches->list.len; idx++) {
        rpl_obj *i = patches->list.data[idx];
        if (i->type != RPL_LIST && i->type != RPL_CODE) {
            free(newdispatches);
            free(newargck);
            rt->stack->list.len = origlen;
            return rpl_ded(rt, "If you want a built-in, you should consider a less broken dispatch table");
        }
        if (i->list.len > argct) {
            rpl_obj *ourdispatch = i->list.data[0];
            if (ourdispatch->type == RPL_SYMBOL) {
                rpl_obj *tryin = rpl_rcl(rt, ourdispatch->symbol.parts, ourdispatch->symbol.nparts);
                if (tryin)
                    ourdispatch = tryin;
            }
            newdispatches[ncount] = ourdispatch;
            for (int j = 0; j < argct; j++) {
                rpl_obj *ourarg = i->list.data[j + 1];
                if (ourarg->type == RPL_SYMBOL) {
                    rpl_obj *tryin = rpl_rcl(rt, ourarg->symbol.parts, ourarg->symbol.nparts);
                    if (tryin)
                        ourarg = tryin;
                }
                if (ourarg->type != RPL_INTEGER) {
                    free(newdispatches);
                    free(newargck);
                    rt->stack->list.len = origlen;
                    return rpl_ded(rt, "Type numbers have to be a number which represents a type");
                }
                newargck[ncount * argct + j] = (int)ourarg->integer;
            }
            ncount++;
        } else {
            free(newdispatches);
            free(newargck);
            rt->stack->list.len = origlen;
            return rpl_ded(rt, "Next time try including the number of arguments you asked for");
        }
    }

    int oldn = bin->builtin.ndispatches;
    rpl_obj **combined_d = xrealloc(NULL, sizeof(rpl_obj *) * (size_t)(ncount + oldn > 0 ? ncount + oldn : 1));
    memcpy(combined_d, newdispatches, sizeof(rpl_obj *) * (size_t)ncount);
    if (oldn)
        memcpy(combined_d + ncount, bin->builtin.dispatches, sizeof(rpl_obj *) * (size_t)oldn);

    int *combined_a = xrealloc(NULL, sizeof(int) * (size_t)argct * (size_t)(ncount + oldn > 0 ? ncount + oldn : 1));
    memcpy(combined_a, newargck, sizeof(int) * (size_t)argct * (size_t)ncount);
    if (oldn)
        memcpy(combined_a + argct * ncount, bin->builtin.argck, sizeof(int) * (size_t)argct * (size_t)oldn);

    free(bin->builtin.dispatches);
    free(bin->builtin.argck);
    bin->builtin.dispatches = combined_d;
    bin->builtin.argck = combined_a;
    bin->builtin.ndispatches = ncount + oldn;

    free(newdispatches);
    free(newargck);
    stack_push(rt, bin);
    return cont(rt);
}

static rpl_thunk bi_mkdir(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    stack_push(rt, rpl_firstdir(rt, rt->lastobj));
    return cont(rt);
}

static rpl_thunk bi_numtoint(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *number = stack_pop(rt);
    double v = number->type == RPL_FLOAT ? number->floating : (double)number->integer;
    if (!isfinite(v)) {
        stack_push(rt, number);
        return rpl_ded(rt, "This float is too weird to be an integer");
    }
    stack_push(rt, rpl_new_int(&rt->gc, (int64_t)v));
    return cont(rt);
}

static rpl_thunk bi_strtoint(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    char *end;
    char *buf = xstrndup(x->string.data, x->string.len);
    long long v = strtoll(buf, &end, 10);
    /* Python's int(str) tolerates surrounding whitespace but nothing else. */
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
        end++;
    int ok = (end != buf) && (*end == '\0');
    free(buf);
    if (!ok) {
        stack_push(rt, x);
        return rpl_ded(rt, "That will never be an integer, my friend");
    }
    stack_push(rt, rpl_new_int(&rt->gc, v));
    return cont(rt);
}

static rpl_thunk bi_numtofloat(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *o = stack_pop(rt);
    double v = o->type == RPL_FLOAT ? o->floating : (double)o->integer;
    stack_push(rt, rpl_new_float(&rt->gc, v));
    return cont(rt);
}

static rpl_thunk bi_strtofloat(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    char *end;
    char *buf = xstrndup(x->string.data, x->string.len);
    double v = strtod(buf, &end);
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
        end++;
    int ok = (end != buf) && (*end == '\0');
    free(buf);
    if (!ok) {
        stack_push(rt, x);
        return rpl_ded(rt, "Sir/ma'am, this is a Wendy's");
    }
    stack_push(rt, rpl_new_float(&rt->gc, v));
    return cont(rt);
}

static rpl_thunk bi_basicval(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    char *end;
    char *buf = xstrndup(x->string.data, x->string.len);
    double v = strtod(buf, &end);
    if (end == buf)
        v = 0.0;
    free(buf);
    stack_push(rt, rpl_new_float(&rt->gc, v));
    return cont(rt);
}

static rpl_thunk bi_asctoint(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *s = stack_pop(rt);
    if (s->string.len) {
        stack_push(rt, rpl_new_int(&rt->gc, (unsigned char)s->string.data[0]));
    } else {
        stack_push(rt, s);
        return rpl_ded(rt, "It would be 0 if it was anything at all");
    }
    return cont(rt);
}

static rpl_thunk bi_inttoasc(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *s = stack_pop(rt);
    if (s->integer >= 0 && s->integer < 1114112) {
        /* Encode as UTF-8, mirroring Python's chr() -> str. */
        unsigned char buf[4];
        int len;
        int64_t cp = s->integer;
        if (cp < 0x80) { buf[0] = (unsigned char)cp; len = 1; }
        else if (cp < 0x800) {
            buf[0] = (unsigned char)(0xC0 | (cp >> 6));
            buf[1] = (unsigned char)(0x80 | (cp & 0x3F));
            len = 2;
        } else if (cp < 0x10000) {
            buf[0] = (unsigned char)(0xE0 | (cp >> 12));
            buf[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            buf[2] = (unsigned char)(0x80 | (cp & 0x3F));
            len = 3;
        } else {
            buf[0] = (unsigned char)(0xF0 | (cp >> 18));
            buf[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
            buf[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            buf[3] = (unsigned char)(0x80 | (cp & 0x3F));
            len = 4;
        }
        stack_push(rt, rpl_new_string(&rt->gc, (char *)buf, (size_t)len));
    } else {
        stack_push(rt, s);
        return rpl_ded(rt, "This number could not possibly be a character");
    }
    return cont(rt);
}

static rpl_thunk bi_strtosym(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *ourstring = stack_pop(rt);
    char *buf = xstrndup(ourstring->string.data, ourstring->string.len);
    int n;
    char **parts = validatename(buf, &n);
    free(buf);
    if (!parts) {
        stack_push(rt, ourstring);
        return rpl_ded(rt, "This can be a string, but it won't be a symbol");
    }
    stack_push(rt, rpl_new_symbol(&rt->gc, parts, n));
    free_parts(parts, n);
    return cont(rt);
}

static rpl_thunk bi_strtostr(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *o = stack_pop(rt);
    stack_push(rt, rpl_new_string(&rt->gc, o->string.data, o->string.len));
    return cont(rt);
}

static rpl_thunk bi_strtorem(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *o = stack_pop(rt);
    stack_push(rt, rpl_new_comment(&rt->gc, o->string.data, o->string.len));
    return cont(rt);
}

static rpl_thunk bi_numtostr(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *o = stack_pop(rt);
    char buf[64];
    if (o->type == RPL_FLOAT)
        snprintf(buf, sizeof(buf), "%g", o->floating);
    else
        snprintf(buf, sizeof(buf), "%lld", (long long)o->integer);
    stack_push(rt, rpl_new_string(&rt->gc, buf, strlen(buf)));
    return cont(rt);
}

static rpl_thunk bi_symtostr(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *o = stack_pop(rt);
    char *s = symtostr(o->symbol.parts, o->symbol.nparts);
    stack_push(rt, rpl_new_string(&rt->gc, s, strlen(s)));
    free(s);
    return cont(rt);
}

/* ########################################################################
 * Comparisons */

static rpl_thunk bi_eq(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    stack_push(rt, rpl_new_int(&rt->gc, data_equal(x, y)));
    return cont(rt);
}

static rpl_thunk bi_ne(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    stack_push(rt, rpl_new_int(&rt->gc, !data_equal(x, y)));
    return cont(rt);
}

static rpl_thunk bi_eqref(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    stack_push(rt, rpl_new_int(&rt->gc, x == y));
    return cont(rt);
}

static rpl_thunk bi_neref(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    stack_push(rt, rpl_new_int(&rt->gc, x != y));
    return cont(rt);
}

static rpl_thunk bi_lt(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    stack_push(rt, rpl_new_int(&rt->gc, data_gt(x, y)));
    return cont(rt);
}

static rpl_thunk bi_gt(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    stack_push(rt, rpl_new_int(&rt->gc, data_gt(y, x)));
    return cont(rt);
}

static rpl_thunk bi_le(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    stack_push(rt, rpl_new_int(&rt->gc, data_gt(x, y) || data_equal(x, y)));
    return cont(rt);
}

static rpl_thunk bi_ge(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    rpl_obj *y = stack_pop(rt);
    stack_push(rt, rpl_new_int(&rt->gc, data_gt(y, x) || data_equal(x, y)));
    return cont(rt);
}

static rpl_thunk bi_and(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    int x = truthy(stack_pop(rt));
    int y = truthy(stack_pop(rt));
    stack_push(rt, rpl_new_int(&rt->gc, x && y));
    return cont(rt);
}

static rpl_thunk bi_or(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    int x = truthy(stack_pop(rt));
    int y = truthy(stack_pop(rt));
    stack_push(rt, rpl_new_int(&rt->gc, x || y));
    return cont(rt);
}

static rpl_thunk bi_not(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    int x = truthy(stack_pop(rt));
    stack_push(rt, rpl_new_int(&rt->gc, !x));
    return cont(rt);
}

/* ########################################################################
 * List functions */

static int generic_len(rpl_obj *o) {
    switch (o->type) {
        case RPL_LIST:
        case RPL_CODE:    return o->list.len;
        case RPL_STRING:
        case RPL_COMMENT: return (int)o->string.len;
        case RPL_SYMBOL:  return o->symbol.nparts;
        default:          return 0;
    }
}

static rpl_thunk bi_len(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *o = stack_pop(rt);
    stack_push(rt, rpl_new_int(&rt->gc, generic_len(o)));
    return cont(rt);
}

static rpl_thunk bi_lencode(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *o = stack_pop(rt);
    stack_push(rt, rpl_new_int(&rt->gc, o->list.len - 1));
    return cont(rt);
}

static rpl_thunk bi_poplist(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *list = stack_pop(rt);
    if (list->list.len) {
        rpl_obj *newlist = rpl_new_list(&rt->gc, list->type);
        for (int i = 0; i < list->list.len - 1; i++)
            rpl_list_push(&rt->gc, newlist, list->list.data[i]);
        rpl_obj *thing = list->list.data[list->list.len - 1];
        stack_push(rt, newlist);
        stack_push(rt, thing);
    } else {
        stack_push(rt, list);
        return rpl_ded(rt, "Once you pop, you must eventually stop");
    }
    return cont(rt);
}

static rpl_thunk bi_compositeout(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *obj = stack_pop(rt);
    int n = 0;
    if (obj->type == RPL_LIST || obj->type == RPL_CODE) {
        n = obj->list.len;
        for (int i = 0; i < n; i++)
            stack_push(rt, obj->list.data[i]);
    } else if (obj->type == RPL_DIRECTORY) {
        n = 2;
        stack_push(rt, obj->dir.tag);
        stack_push(rt, obj->dir.next);
    } else if (obj->type == RPL_TAG) {
        n = 2;
        char *parts[1] = { obj->tag.name };
        stack_push(rt, rpl_new_symbol(&rt->gc, parts, 1));
        stack_push(rt, obj->tag.obj);
    }
    /* Other types: best-effort no-op (0 components) -- composite> is meant
     * for List/Code/Directory/Tag in practice; this is a simplification
     * relative to Python's fully generic `for i in obj.data`. */
    stack_push(rt, rpl_new_int(&rt->gc, n));
    return cont(rt);
}

static rpl_thunk bi_todir(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *next = stack_pop(rt);
    rpl_obj *tag = stack_pop(rt);
    stack_push(rt, rpl_new_dir(&rt->gc, tag, next));
    return cont(rt);
}

static rpl_thunk bi_dirout(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *obj = stack_pop(rt);
    stack_push(rt, obj->dir.tag);
    stack_push(rt, obj->dir.next);
    return cont(rt);
}

static rpl_thunk bi_tagout(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *obj = stack_pop(rt);
    stack_push(rt, obj->tag.obj);
    char *parts[1] = { obj->tag.name };
    stack_push(rt, rpl_new_symbol(&rt->gc, parts, 1));
    return cont(rt);
}

static rpl_thunk bi_contextout(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *obj = stack_pop(rt);
    stack_push(rt, rpl_new_int(&rt->gc, RPL_CALLDEPTH - obj->context.depth));
    stack_push(rt, obj->context.code);
    stack_push(rt, rpl_new_int(&rt->gc, obj->context.ip));
    stack_push(rt, obj->context.names);
    stack_push(rt, obj->context.next);
    return cont(rt);
}

static rpl_thunk bi_tocontext(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *next = stack_pop(rt);
    rpl_obj *names = stack_pop(rt);
    rpl_obj *ip = stack_pop(rt);
    rpl_obj *code = stack_pop(rt);
    rpl_obj *newcontext = rpl_new_context(&rt->gc, code, names, next);
    newcontext->context.ip = (int)ip->integer;
    stack_push(rt, newcontext);
    return cont(rt);
}

static rpl_thunk bi_tobasecontext(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *names = stack_pop(rt);
    rpl_obj *code = stack_pop(rt);
    stack_push(rt, rpl_new_context(&rt->gc, code, names, NULL));
    return cont(rt);
}

static rpl_thunk bi_left(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *jobj = stack_pop(rt);
    rpl_obj *lst = stack_pop(rt);
    int64_t j = jobj->integer;
    if (j >= 0) {
        int len = generic_len(lst);
        int use = j < len ? (int)j : len;
        if (lst->type == RPL_STRING) {
            stack_push(rt, rpl_new_string(&rt->gc, lst->string.data, (size_t)use));
        } else {
            rpl_obj *n = rpl_new_list(&rt->gc, lst->type);
            for (int i = 0; i < use; i++)
                rpl_list_push(&rt->gc, n, lst->list.data[i]);
            stack_push(rt, n);
        }
    } else {
        stack_push(rt, lst);
        stack_push(rt, jobj);
        return rpl_ded(rt, "Ask at least for zero, maybe more");
    }
    return cont(rt);
}

static rpl_thunk bi_right(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *jobj = stack_pop(rt);
    rpl_obj *lst = stack_pop(rt);
    int64_t j = jobj->integer;
    if (j >= 0) {
        int len = generic_len(lst);
        int start = len - (int)j;
        if (start < 0)
            start = 0;
        if (lst->type == RPL_STRING) {
            stack_push(rt, rpl_new_string(&rt->gc, lst->string.data + start, (size_t)(len - start)));
        } else {
            rpl_obj *n = rpl_new_list(&rt->gc, lst->type);
            for (int i = start; i < len; i++)
                rpl_list_push(&rt->gc, n, lst->list.data[i]);
            stack_push(rt, n);
        }
    } else {
        stack_push(rt, lst);
        stack_push(rt, jobj);
        return rpl_ded(rt, "Ask at least for zero, maybe more");
    }
    return cont(rt);
}

static rpl_thunk bi_subs(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *jobj = stack_pop(rt);
    rpl_obj *iobj = stack_pop(rt);
    rpl_obj *lst = stack_pop(rt);
    int64_t i = iobj->integer;
    int64_t j = jobj->integer;
    int len = generic_len(lst);
    if (i >= 0 && i < len) {
        int64_t end = j + 1;
        if (end > len)
            end = len;
        if (end < i)
            end = i;
        if (lst->type == RPL_STRING) {
            stack_push(rt, rpl_new_string(&rt->gc, lst->string.data + i, (size_t)(end - i)));
        } else {
            rpl_obj *n = rpl_new_list(&rt->gc, lst->type);
            for (int64_t k = i; k < end; k++)
                rpl_list_push(&rt->gc, n, lst->list.data[k]);
            stack_push(rt, n);
        }
    } else {
        stack_push(rt, lst);
        stack_push(rt, jobj);
        stack_push(rt, iobj);
        return rpl_ded(rt, "It would help to have a valid starting subscript");
    }
    return cont(rt);
}

static rpl_thunk bi_gete(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *iobj = stack_pop(rt);
    rpl_obj *lst = stack_pop(rt);
    int64_t i = iobj->integer;
    if (i >= 0 && i < lst->list.len)
        return rpl_obj_eval(rt, lst->list.data[i]);
    stack_push(rt, lst);
    stack_push(rt, iobj);
    char msg[96];
    snprintf(msg, sizeof(msg), "This %s deserves a better subscript", typename_of(lst));
    return rpl_ded(rt, msg);
}

static rpl_thunk bi_get(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *iobj = stack_pop(rt);
    rpl_obj *lst = stack_pop(rt);
    int64_t i = iobj->integer;
    int len = generic_len(lst);
    if (i >= 0 && i < len) {
        if (lst->type == RPL_STRING)
            stack_push(rt, rpl_new_string(&rt->gc, lst->string.data + i, 1));
        else
            stack_push(rt, lst->list.data[i]);
    } else {
        stack_push(rt, lst);
        stack_push(rt, iobj);
        char msg[96];
        snprintf(msg, sizeof(msg), "This %s deserves a better subscript", typename_of(lst));
        return rpl_ded(rt, msg);
    }
    return cont(rt);
}

static rpl_thunk bi_put(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *iobj = stack_pop(rt);
    rpl_obj *obj = stack_pop(rt);
    rpl_obj *lst = stack_pop(rt);
    int64_t i = iobj->integer;
    if (i >= 0 && i < lst->list.len) {
        rpl_obj *n = rpl_cp(&rt->gc, lst);
        n->list.data[i] = obj;
        stack_push(rt, n);
    } else {
        stack_push(rt, lst);
        stack_push(rt, obj);
        stack_push(rt, iobj);
        char msg[96];
        snprintf(msg, sizeof(msg), "This %s deserves a better subscript", typename_of(lst));
        return rpl_ded(rt, msg);
    }
    return cont(rt);
}

static rpl_thunk bi_tosym(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *itemsobj = stack_pop(rt);
    int items = (int)itemsobj->integer;
    int len = stack_len(rt);
    int use = items < len ? items : len;
    char **parts = xrealloc(NULL, sizeof(char *) * (size_t)(use ? use : 1));
    for (int i = 0; i < use; i++) {
        rpl_obj *o = rt->stack->list.data[len - use + i];
        parts[i] = o->type == RPL_STRING || o->type == RPL_COMMENT ? o->string.data
                                                                     : (char *)"";
    }
    rt->stack->list.len = len - use;
    stack_push(rt, rpl_new_symbol(&rt->gc, parts, use));
    free(parts);
    return cont(rt);
}

static rpl_thunk bi_tolst(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *itemsobj = stack_pop(rt);
    int items = (int)itemsobj->integer;
    if (stack_len(rt) < items) {
        stack_push(rt, itemsobj);
        char msg[160];
        snprintf(msg, sizeof(msg), "If you want %d things in a list, maybe you should have %d things on the stack", items, items);
        return rpl_ded(rt, msg);
    }
    int len = stack_len(rt);
    rpl_obj *n = rpl_new_list(&rt->gc, RPL_LIST);
    for (int i = len - items; i < len; i++)
        rpl_list_push(&rt->gc, n, rt->stack->list.data[i]);
    rt->stack->list.len = len - items;
    stack_push(rt, n);
    return cont(rt);
}

static rpl_thunk bi_codetolst(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *lst = stack_pop(rt);
    rpl_obj *n = rpl_new_list(&rt->gc, RPL_LIST);
    for (int i = 0; i < lst->list.len - 1; i++)
        rpl_list_push(&rt->gc, n, lst->list.data[i]);
    stack_push(rt, n);
    return cont(rt);
}

static rpl_thunk bi_tocode(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *itemsobj = stack_pop(rt);
    int items = (int)itemsobj->integer;
    if (stack_len(rt) < items) {
        stack_push(rt, itemsobj);
        char msg[64];
        snprintf(msg, sizeof(msg), "%d is more items than you have", items);
        return rpl_ded(rt, msg);
    }
    int len = stack_len(rt);
    rpl_obj *n = rpl_new_list(&rt->gc, RPL_CODE);
    for (int i = len - items; i < len; i++)
        rpl_list_push(&rt->gc, n, rt->stack->list.data[i]);
    rpl_list_push(&rt->gc, n, rt->ret_internal);
    rt->stack->list.len = len - items;
    stack_push(rt, n);
    return cont(rt);
}

static rpl_thunk bi_lsttocode(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *lst = stack_pop(rt);
    rpl_obj *n = rpl_new_list(&rt->gc, RPL_CODE);
    for (int i = 0; i < lst->list.len; i++)
        rpl_list_push(&rt->gc, n, lst->list.data[i]);
    rpl_list_push(&rt->gc, n, rt->ret_internal);
    stack_push(rt, n);
    return cont(rt);
}

/* ########################################################################
 * Error handling */

static rpl_thunk bi_ded(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *o = stack_pop(rt);
    return rpl_ded(rt, o->string.data);
}

static rpl_thunk bi_blame(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rt->caller = stack_pop(rt);
    return cont(rt);
}

/* ########################################################################
 * Bitwise operations */

static rpl_thunk bi_bnot(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_obj *x = stack_pop(rt);
    stack_push(rt, rpl_new_int(&rt->gc, ~x->integer));
    return cont(rt);
}

static rpl_thunk bi_bshl(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    int64_t bits = stack_pop(rt)->integer;
    int64_t x = stack_pop(rt)->integer;
    stack_push(rt, rpl_new_int(&rt->gc, x << bits));
    return cont(rt);
}

static rpl_thunk bi_bshr(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    int64_t bits = stack_pop(rt)->integer;
    int64_t x = stack_pop(rt)->integer;
    stack_push(rt, rpl_new_int(&rt->gc, x >> bits));
    return cont(rt);
}

static rpl_thunk bi_band(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    int64_t x = stack_pop(rt)->integer;
    int64_t y = stack_pop(rt)->integer;
    stack_push(rt, rpl_new_int(&rt->gc, x & y));
    return cont(rt);
}

static rpl_thunk bi_bor(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    int64_t x = stack_pop(rt)->integer;
    int64_t y = stack_pop(rt)->integer;
    stack_push(rt, rpl_new_int(&rt->gc, x | y));
    return cont(rt);
}

static rpl_thunk bi_bxor(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    int64_t x = stack_pop(rt)->integer;
    int64_t y = stack_pop(rt)->integer;
    stack_push(rt, rpl_new_int(&rt->gc, x ^ y));
    return cont(rt);
}

/* ########################################################################
 * Registration table + stoprocs */

typedef struct {
    const char *name;
    rpl_eval_fn fn;
} rpl_internal_entry;

static const rpl_internal_entry rpl_internals_table[] = {
    /* Documentation */
    { "firstobj", bi_firstobj }, { "dir", bi_dir }, { "romid", bi_romid },
    { "ramid", bi_ramid }, { "type", bi_type }, { "self", bi_self },
    { "getcontext", bi_getcontext }, { "setcontext", bi_setcontext },
    { "nextcontext", bi_nextcontext }, { "clrrun", bi_clrrun },
    { "errstate", bi_errstate }, { "lastcall", bi_lastcall },

    /* Input/Output */
    { "fopen", bi_fopen }, { "feof", bi_feof }, { "fclose", bi_fclose },
    { "freadline", bi_freadline }, { "fread", bi_fread },
    { "fwriten", bi_fwriten }, { "fwrite", bi_fwrite },
    { "disp", bi_disp }, { "dispn", bi_dispn }, { "prompt", bi_prompt },
    { "epoch", bi_epoch },

    /* Stack manipulation */
    { "stack", bi_stack }, { "drop", bi_drop }, { "dropn", bi_dropn },
    { "pick", bi_pick }, { "over", bi_over }, { "eval", bi_eval },
    { "swap", bi_swap }, { "dup", bi_dup }, { "dup2", bi_dup2 },
    { "dupn", bi_dupn }, { "rot", bi_rot }, { "rotd", bi_rotd },
    { "roll", bi_roll }, { "rolld", bi_rolld }, { "require", bi_require },

    /* Named storage */
    { "rcl", bi_rcl }, { "rclfrom", bi_rclfrom }, { "sto", bi_sto },
    { "stoto", bi_stoto }, { "deref", bi_deref }, { "exists", bi_exists },
    { "rm", bi_rm }, { "cp", bi_cp }, { "id", bi_id },
    { "tlocal", bi_tlocal }, { "regtype", bi_regtype }, { "local", bi_local },

    /* Flow control */
    { "evalnext", bi_evalnext }, { "bail", bi_bail }, { "beval", bi_beval },
    { "ift", bi_ift }, { "ifte", bi_ifte }, { "rst", bi_rst },

    /* Mathemagics */
    { "odd", bi_odd }, { "absint", bi_absint }, { "absfloat", bi_absfloat },
    { "modfloat", bi_modfloat }, { "modint", bi_modint },
    { "+float", bi_addfloat }, { "+int", bi_addint }, { "+str", bi_addstr },
    { "+list", bi_addlist }, { "list+", bi_listadd }, { "catlist", bi_catlist },
    { "catcode", bi_catcode }, { "+sym", bi_addsym },
    { "^float", bi_powfloat }, { "^int", bi_powint },
    { "*float", bi_mulfloat }, { "*int", bi_mulint }, { "*str", bi_mulstr },
    { "*lst", bi_mullst }, { "*code", bi_mulcode },
    { "-float", bi_subfloat }, { "-int", bi_subint },
    { "/float", bi_divfloat }, { "/int", bi_divint },
    { "neg", bi_neg }, { "rnd", bi_rnd }, { "ip", bi_ip },

    /* Conversions */
    { ">quote", bi_toquote }, { ">tag", bi_totag }, { ">bin", bi_tobin },
    { "bin>", bi_binfrom }, { "setdispatch", bi_setdispatch },
    { "binhook", bi_binhook }, { "mkdir", bi_mkdir },
    { "num>int", bi_numtoint }, { "str>int", bi_strtoint },
    { "num>float", bi_numtofloat }, { "str>float", bi_strtofloat },
    { "basicval", bi_basicval }, { "asc>", bi_asctoint }, { ">asc", bi_inttoasc },
    { "str>sym", bi_strtosym }, { "str>str", bi_strtostr },
    { "str>rem", bi_strtorem }, { "num>str", bi_numtostr },
    { "sym>str", bi_symtostr },

    /* Comparisons */
    { "==", bi_eq }, { "!=", bi_ne }, { "==ref", bi_eqref }, { "!=ref", bi_neref },
    { "<", bi_lt }, { ">", bi_gt }, { "<=", bi_le }, { ">=", bi_ge },
    { "and", bi_and }, { "or", bi_or }, { "not", bi_not },

    /* List functions */
    { "len", bi_len }, { "lencode", bi_lencode }, { "pop", bi_poplist },
    { "composite>", bi_compositeout }, { ">dir", bi_todir }, { "dir>", bi_dirout },
    { "tag>", bi_tagout }, { "context>", bi_contextout }, { ">context", bi_tocontext },
    { ">basecontext", bi_tobasecontext }, { "left", bi_left }, { "right", bi_right },
    { "subs", bi_subs }, { "gete", bi_gete }, { "get", bi_get }, { "put", bi_put },
    { ">sym", bi_tosym }, { ">lst", bi_tolst }, { "code>lst", bi_codetolst },
    { ">code", bi_tocode }, { "lst>code", bi_lsttocode },

    /* Error handling */
    { "ded", bi_ded }, { "blame", bi_blame },

    /* Bitwise operations */
    { "bnot", bi_bnot }, { "bshl", bi_bshl }, { "bshr", bi_bshr },
    { "band", bi_band }, { "bor", bi_bor }, { "bxor", bi_bxor },
};

void rpl_stoprocs(rpl_runtime *rt, const char *dir) {
    size_t n = sizeof(rpl_internals_table) / sizeof(rpl_internals_table[0]);
    for (size_t i = 0; i < n; i++) {
        char *path[2] = { (char *)dir, (char *)rpl_internals_table[i].name };
        rpl_obj *proc = rpl_new_binproc(&rt->gc, rpl_internals_table[i].name, rpl_internals_table[i].fn);
        rpl_sto(rt, path, 2, proc);
    }
    /* "evalrom" mirrors pysys's stoprocs(), which stores this one out of
     * pysys/rom.py rather than out of the makebinprocs() list itself. */
    char *evalrom_path[2] = { (char *)dir, "evalrom" };
    rpl_sto(rt, evalrom_path, 2, rpl_new_binproc(&rt->gc, "evalrom", rpl_evalrom));
}
