/* Smoke tests for Phase 2 (runtime core): the rs() trampoline, newcall's
 * tail-call reuse vs. real call-stack growth, ded()'s recursion-limit and
 * error-context behavior, and the named-store routines (rcl/sto/rm/deref,
 * circsym/circdir cycle detection). */

#include "runtime.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* A fresh context whose code is just [ret_internal] behaves like the
 * bottom of the call stack: running the trampoline on it should halt
 * immediately without pushing anything to the data stack. */
static void test_rs_halts_on_bare_context(void) {
    rpl_runtime rt;
    rpl_runtime_init(&rt);

    rpl_rs(&rt, (rpl_thunk){ rpl_context_eval, rt.context });

    assert(rt.running == 0);
    assert(rt.stack->list.len == 0);

    rpl_runtime_destroy(&rt);
}

/* newcall() onto a context whose next instruction is ret_internal must
 * reuse the current context slot (tail call) rather than growing the call
 * stack. Running the resulting code should push both integers in order,
 * then halt on the trailing ret_internal. */
static void test_newcall_tail_reuses_context(void) {
    rpl_runtime rt;
    rpl_runtime_init(&rt);
    rpl_obj *before = rt.context;

    rpl_obj *code = rpl_new_list(&rt.gc, RPL_CODE);
    rpl_list_push(&rt.gc, code, rpl_new_int(&rt.gc, 11));
    rpl_list_push(&rt.gc, code, rpl_new_int(&rt.gc, 22));
    rpl_list_push(&rt.gc, code, rt.ret_internal);

    rpl_thunk next = rpl_newcall(&rt, code);
    assert(rt.context == before);   /* reused, not replaced */

    rpl_rs(&rt, next);

    assert(rt.running == 0);
    assert(rt.stack->list.len == 2);
    assert(rt.stack->list.data[0]->integer == 11);
    assert(rt.stack->list.data[1]->integer == 22);

    rpl_runtime_destroy(&rt);
}

/* newcall() when the next instruction is NOT ret_internal must instead
 * push a brand new context (real call-stack growth), leaving the caller's
 * frame exactly where it was so it can be resumed later. */
static void test_newcall_grows_stack_when_not_tail(void) {
    rpl_runtime rt;
    rpl_runtime_init(&rt);

    /* Land the current context on a non-tail instruction: code = [int, ret]. */
    rpl_obj *outer = rpl_new_list(&rt.gc, RPL_CODE);
    rpl_list_push(&rt.gc, outer, rpl_new_int(&rt.gc, 1));
    rpl_list_push(&rt.gc, outer, rt.ret_internal);
    rpl_newcall(&rt, outer);   /* tail reuse: current context now runs `outer`, ip 0 */
    rpl_obj *caller_ctx = rt.context;

    rpl_obj *inner = rpl_new_list(&rt.gc, RPL_CODE);
    rpl_list_push(&rt.gc, inner, rpl_new_int(&rt.gc, 99));
    rpl_list_push(&rt.gc, inner, rt.ret_internal);

    rpl_newcall(&rt, inner);   /* not a tail call: outer.data[0] is an int */

    assert(rt.context != caller_ctx);
    assert(rt.context->context.next == caller_ctx);
    assert(rt.context->context.depth == caller_ctx->context.depth - 1);
    assert(caller_ctx->context.code == outer);
    assert(caller_ctx->context.ip == 0);   /* untouched by the nested call */

    rpl_runtime_destroy(&rt);
}

/* Exhausting the call-depth budget must route through ded() rather than
 * actually recursing past RPL_CALLDEPTH: newcall reports the recursion
 * error, setting Caller to rtcaller and a reason mentioning "recursions". */
static void test_newcall_depth_exhaustion_deds(void) {
    rpl_runtime rt;
    rpl_runtime_init(&rt);

    rpl_obj *nontail = rpl_new_list(&rt.gc, RPL_CODE);
    rpl_list_push(&rt.gc, nontail, rpl_new_int(&rt.gc, 0));
    rpl_list_push(&rt.gc, nontail, rt.ret_internal);

    /* First call is a tail reuse (initial context's next instr is
     * ret_internal); every call after that is non-tail and burns one unit
     * of depth, since nontail.data[0] is never ret_internal. */
    rpl_thunk next = rpl_newcall(&rt, nontail);
    for (int i = 0; i < RPL_CALLDEPTH + 1; i++)
        next = rpl_newcall(&rt, nontail);

    assert(rt.caller == rt.rtcaller);
    assert(rt.reason != NULL);
    assert(strstr(rt.reason, "recursions") != NULL);
    /* What running `next` would actually do (resolve and evaluate the
     * EXCEPT handler) is Symbol eval's concern, covered in test_phase3.c;
     * here we only care that newcall routed the failure through ded(). */
    (void)next;

    rpl_runtime_destroy(&rt);
}

/* rcl/sto/deref/rm round-trip through a flat, top-level name. */
static void test_named_store_flat(void) {
    rpl_runtime rt;
    rpl_runtime_init(&rt);

    char *path[1] = { "X" };
    rpl_obj *val = rpl_new_int(&rt.gc, 42);
    assert(rpl_sto(&rt, path, 1, val) == 1);
    assert(rpl_rcl(&rt, path, 1) == val);

    rpl_obj *tag = rpl_deref(&rt, path, 1);
    assert(tag->type == RPL_TAG);
    assert(strcmp(tag->tag.name, "X") == 0);
    assert(tag->tag.obj == val);

    rpl_obj *val2 = rpl_new_int(&rt.gc, 43);
    assert(rpl_sto(&rt, path, 1, val2) == 1);   /* overwrite in place */
    assert(rpl_rcl(&rt, path, 1) == val2);

    assert(rpl_rm(&rt, path, 1) == 1);
    assert(rpl_rcl(&rt, path, 1) == NULL);
    assert(rpl_rm(&rt, path, 1) == 0);   /* already gone */

    rpl_runtime_destroy(&rt);
}

/* sto() descending into a nested directory: SUB.Y, where SUB must already
 * exist (sto never creates intermediate directories) but Y is a new leaf. */
static void test_named_store_nested(void) {
    rpl_runtime rt;
    rpl_runtime_init(&rt);

    char *missing[2] = { "NOPE", "Y" };
    rpl_obj *v = rpl_new_int(&rt.gc, 1);
    assert(rpl_sto(&rt, missing, 2, v) == 0);   /* SUB doesn't exist yet */

    char *subpath[1] = { "SUB" };
    assert(rpl_sto(&rt, subpath, 1, rpl_firstdir(&rt, NULL)) == 1);

    char *ypath[2] = { "SUB", "Y" };
    assert(rpl_sto(&rt, ypath, 2, v) == 1);
    assert(rpl_rcl(&rt, ypath, 2) == v);

    rpl_runtime_destroy(&rt);
}

/* Two symbols pointing at each other (A -> B, B -> A) must be flagged as
 * circular; a symbol pointing at a plain value must not be. */
static void test_circsym(void) {
    rpl_runtime rt;
    rpl_runtime_init(&rt);

    char *bpart[1] = { "B" };
    char *apart[1] = { "A" };
    char *a[1] = { "A" };
    char *b[1] = { "B" };
    assert(rpl_sto(&rt, a, 1, rpl_new_symbol(&rt.gc, bpart, 1)) == 1);
    assert(rpl_sto(&rt, b, 1, rpl_new_symbol(&rt.gc, apart, 1)) == 1);
    assert(rpl_circsym(&rt, a, 1) == 1);

    char *dpart[1] = { "D" };
    char *c[1] = { "C" };
    char *d[1] = { "D" };
    assert(rpl_sto(&rt, c, 1, rpl_new_symbol(&rt.gc, dpart, 1)) == 1);
    assert(rpl_sto(&rt, d, 1, rpl_new_int(&rt.gc, 7)) == 1);
    assert(rpl_circsym(&rt, c, 1) == 0);

    rpl_runtime_destroy(&rt);
}

/* circdir must find a circular symbol chain (A <-> B, as above) even when
 * the entry point into it is a symbol tucked inside a subdirectory. */
static void test_circdir_finds_nested_cycle(void) {
    rpl_runtime rt;
    rpl_runtime_init(&rt);

    char *bpart[1] = { "B" };
    char *apart[1] = { "A" };
    char *a[1] = { "A" };
    char *b[1] = { "B" };
    rpl_sto(&rt, a, 1, rpl_new_symbol(&rt.gc, bpart, 1));
    rpl_sto(&rt, b, 1, rpl_new_symbol(&rt.gc, apart, 1));

    char *sub[1] = { "SUB" };
    rpl_sto(&rt, sub, 1, rpl_firstdir(&rt, NULL));
    char *subx[2] = { "SUB", "X" };
    rpl_sto(&rt, subx, 2, rpl_new_symbol(&rt.gc, apart, 1));   /* SUB.X -> A */

    assert(rpl_circdir(&rt, rt.context->context.names) == 1);

    rpl_runtime_destroy(&rt);
}

/* A directory with no symbols at all (just plain values, including a
 * harmless empty subdirectory) must not be flagged as circular. */
static void test_circdir_benign(void) {
    rpl_runtime rt;
    rpl_runtime_init(&rt);

    char *n[1] = { "N" };
    rpl_sto(&rt, n, 1, rpl_new_int(&rt.gc, 5));
    char *sub[1] = { "SUB" };
    rpl_sto(&rt, sub, 1, rpl_firstdir(&rt, NULL));

    assert(rpl_circdir(&rt, rt.context->context.names) == 0);

    rpl_runtime_destroy(&rt);
}

int main(void) {
    test_rs_halts_on_bare_context();
    test_newcall_tail_reuses_context();
    test_newcall_grows_stack_when_not_tail();
    test_newcall_depth_exhaustion_deds();
    test_named_store_flat();
    test_named_store_nested();
    test_circsym();
    test_circdir_finds_nested_cycle();
    test_circdir_benign();

    printf("phase2: all tests passed\n");
    return 0;
}
