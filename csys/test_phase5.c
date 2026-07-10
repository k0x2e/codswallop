/* Smoke tests for Phase 5 (internals / makebinprocs port): a representative
 * cross-section, not all ~90 -- stack ops, named storage (incl. circular
 * rejection), local (both the tail-call-no-names path and the
 * newlocall-with-bound-names path, plus a circulation rejection), flow
 * control, a few conversions, arithmetic/comparison, and list/composite
 * ops. Matches the assert-based style of test_phase1-3.c. */

#include "internals.h"
#include "types.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static rpl_thunk step(rpl_runtime *rt, rpl_thunk t) {
    (void)rt;
    return t.fn(rt, t.self);
}

/* Runs an internal (looked up by name out of "I") exactly once: fetches
 * its binproc object out of the store and steps its eval thunk one time
 * (an RPL_INTERNAL's eval thunk *is* its C function, so this just calls
 * it directly). */
static rpl_thunk call_internal(rpl_runtime *rt, const char *name) {
    char *path[2] = { "I", (char *)name };
    rpl_obj *proc = rpl_rcl(rt, path, 2);
    assert(proc && proc->type == RPL_INTERNAL);
    return step(rt, rpl_obj_eval(rt, proc));
}

static rpl_runtime *make_rt(void) {
    rpl_runtime *rt = malloc(sizeof(*rt));
    rpl_runtime_init(rt);
    rpl_register_types(rt);
    /* Mirrors rpl.py: the internals directory itself must exist before
     * stoprocs() populates it -- rpl_sto only ever creates a leaf entry,
     * never an intermediate directory. */
    char *dirpath[1] = { "I" };
    rpl_sto(rt, dirpath, 1, rpl_firstdir(rt, rt->lastobj));
    rpl_stoprocs(rt, "I");
    return rt;
}

static void free_rt(rpl_runtime *rt) {
    rpl_runtime_destroy(rt);
    free(rt);
}

/* ---- Stack manipulation ---- */

static void test_dup(void) {
    rpl_runtime *rt = make_rt();
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 7));
    call_internal(rt, "dup");
    assert(rt->stack->list.len == 2);
    assert(rt->stack->list.data[0]->integer == 7);
    assert(rt->stack->list.data[1]->integer == 7);
    free_rt(rt);
}

static void test_swap(void) {
    rpl_runtime *rt = make_rt();
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 1));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 2));
    call_internal(rt, "swap");
    assert(rt->stack->list.data[0]->integer == 2);
    assert(rt->stack->list.data[1]->integer == 1);
    free_rt(rt);
}

static void test_rot(void) {
    rpl_runtime *rt = make_rt();
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 1));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 2));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 3));
    call_internal(rt, "rot");
    /* 1 2 3 -> 2 3 1 */
    assert(rt->stack->list.data[0]->integer == 2);
    assert(rt->stack->list.data[1]->integer == 3);
    assert(rt->stack->list.data[2]->integer == 1);
    free_rt(rt);
}

static void test_pick(void) {
    rpl_runtime *rt = make_rt();
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 10));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 20));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 2)); /* pick 2nd from top */
    call_internal(rt, "pick");
    assert(rt->stack->list.len == 3);
    assert(rt->stack->list.data[2]->integer == 10);
    free_rt(rt);
}

static void test_dropn(void) {
    rpl_runtime *rt = make_rt();
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 1));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 2));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 3));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 2)); /* drop 2 */
    call_internal(rt, "dropn");
    assert(rt->stack->list.len == 1);
    assert(rt->stack->list.data[0]->integer == 1);
    free_rt(rt);
}

static void test_dropn_too_many_deds(void) {
    rpl_runtime *rt = make_rt();
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 99)); /* drop 99: too many */
    call_internal(rt, "dropn");
    assert(rt->reason != NULL);
    assert(strstr(rt->reason, "reasonable number of lines") != NULL);
    free_rt(rt);
}

static void test_roll(void) {
    rpl_runtime *rt = make_rt();
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 1));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 2));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 3));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 3)); /* roll 3 */
    call_internal(rt, "roll");
    /* 1 2 3 -> 2 3 1 (same rotation shape as rot, width 3) */
    assert(rt->stack->list.data[0]->integer == 2);
    assert(rt->stack->list.data[1]->integer == 3);
    assert(rt->stack->list.data[2]->integer == 1);
    free_rt(rt);
}

/* ---- Named storage ---- */

static void test_sto_rcl_exists_rm(void) {
    rpl_runtime *rt = make_rt();
    char *path[1] = { "FOO" };

    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 42));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_symbol(&rt->gc, path, 1));
    call_internal(rt, "sto");
    assert(rt->stack->list.len == 0);

    rpl_list_push(&rt->gc, rt->stack, rpl_new_symbol(&rt->gc, path, 1));
    call_internal(rt, "exists");
    assert(rt->stack->list.data[rt->stack->list.len - 1]->integer == 1);
    rt->stack->list.len--;

    rpl_list_push(&rt->gc, rt->stack, rpl_new_symbol(&rt->gc, path, 1));
    call_internal(rt, "rcl");
    assert(rt->stack->list.data[rt->stack->list.len - 1]->integer == 42);
    rt->stack->list.len--;

    rpl_list_push(&rt->gc, rt->stack, rpl_new_symbol(&rt->gc, path, 1));
    call_internal(rt, "deref");
    rpl_obj *tag = rt->stack->list.data[rt->stack->list.len - 1];
    assert(tag->type == RPL_TAG && tag->tag.obj->integer == 42);
    rt->stack->list.len--;

    rpl_list_push(&rt->gc, rt->stack, rpl_new_symbol(&rt->gc, path, 1));
    call_internal(rt, "rm");
    assert(rt->stack->list.len == 0);

    rpl_list_push(&rt->gc, rt->stack, rpl_new_symbol(&rt->gc, path, 1));
    call_internal(rt, "exists");
    assert(rt->stack->list.data[rt->stack->list.len - 1]->integer == 0);

    free_rt(rt);
}

/* Storing a symbol that refers to itself must be rejected. */
static void test_sto_rejects_circular_symbol(void) {
    rpl_runtime *rt = make_rt();
    char *path[1] = { "SELF" };

    rpl_list_push(&rt->gc, rt->stack, rpl_new_symbol(&rt->gc, path, 1)); /* value: symbol SELF */
    rpl_list_push(&rt->gc, rt->stack, rpl_new_symbol(&rt->gc, path, 1)); /* name: SELF */
    call_internal(rt, "sto");

    assert(rt->reason != NULL);
    assert(strstr(rt->reason, "circular") == NULL); /* first sto succeeds: SELF didn't exist yet */

    free_rt(rt);
}

/* A genuine A -> B -> A symbol cycle must be caught. */
static void test_sto_rejects_symbol_cycle(void) {
    rpl_runtime *rt = make_rt();
    char *apath[1] = { "A" };
    char *bpath[1] = { "B" };

    /* A -> B */
    rpl_list_push(&rt->gc, rt->stack, rpl_new_symbol(&rt->gc, bpath, 1));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_symbol(&rt->gc, apath, 1));
    call_internal(rt, "sto");
    assert(rt->reason == NULL);

    /* B -> A, completing a cycle */
    rpl_list_push(&rt->gc, rt->stack, rpl_new_symbol(&rt->gc, apath, 1));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_symbol(&rt->gc, bpath, 1));
    call_internal(rt, "sto");
    assert(rt->reason != NULL);
    assert(strstr(rt->reason, "cDonalds") != NULL);

    free_rt(rt);
}

/* ---- local: tail-call path with no names bound ---- */

static void test_local_no_names_tailcalls(void) {
    rpl_runtime *rt = make_rt();
    rpl_obj *before = rt->context;

    rpl_obj *prog = rpl_new_list(&rt->gc, RPL_CODE);
    rpl_list_push(&rt->gc, prog, rpl_new_int(&rt->gc, 99));
    rpl_list_push(&rt->gc, prog, rt->ret_internal);

    rpl_obj *names = rpl_new_list(&rt->gc, RPL_LIST); /* empty: nothing to bind */

    rpl_list_push(&rt->gc, rt->stack, prog);
    rpl_list_push(&rt->gc, rt->stack, names);

    rpl_thunk next = call_internal(rt, "local");
    assert(rt->context == before); /* tail call reused the frame */

    rpl_rs(rt, next);
    assert(rt->stack->list.len == 1);
    assert(rt->stack->list.data[0]->integer == 99);

    free_rt(rt);
}

/* ---- local: binds a name and the bound code can see it ---- */

static void test_local_binds_name(void) {
    rpl_runtime *rt = make_rt();

    char *xpath[1] = { "X" };
    rpl_obj *prog = rpl_new_list(&rt->gc, RPL_CODE);
    rpl_list_push(&rt->gc, prog, rpl_new_symbol(&rt->gc, xpath, 1)); /* push X's value */
    rpl_list_push(&rt->gc, prog, rt->ret_internal);

    rpl_obj *names = rpl_new_list(&rt->gc, RPL_LIST);
    rpl_list_push(&rt->gc, names, rpl_new_symbol(&rt->gc, xpath, 1)); /* bind name X */

    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 5)); /* value bound to X */
    rpl_list_push(&rt->gc, rt->stack, prog);
    rpl_list_push(&rt->gc, rt->stack, names);

    rpl_thunk next = call_internal(rt, "local");
    rpl_rs(rt, next);

    assert(rt->stack->list.len == 1);
    assert(rt->stack->list.data[0]->integer == 5);

    free_rt(rt);
}

/* ---- local: binding a self-referential symbol value is rejected ---- */

static void test_local_rejects_circulation(void) {
    rpl_runtime *rt = make_rt();

    char *xpath[1] = { "X" };
    rpl_obj *prog = rpl_new_list(&rt->gc, RPL_CODE);
    rpl_list_push(&rt->gc, prog, rt->ret_internal);

    rpl_obj *names = rpl_new_list(&rt->gc, RPL_LIST);
    rpl_list_push(&rt->gc, names, rpl_new_symbol(&rt->gc, xpath, 1));

    rpl_obj *origcontext = rt->context;
    rpl_list_push(&rt->gc, rt->stack, rpl_new_symbol(&rt->gc, xpath, 1)); /* value: symbol X itself */
    rpl_list_push(&rt->gc, rt->stack, prog);
    rpl_list_push(&rt->gc, rt->stack, names);
    int origlen = rt->stack->list.len;

    call_internal(rt, "local");

    assert(rt->reason != NULL);
    assert(strstr(rt->reason, "Round and round") != NULL);
    /* usded() resets rt->context to origcontext and *then* calls rpl_ded,
     * which wraps whatever the current context is with a fresh
     * error-handler context -- so the context left behind is a new frame
     * whose ->next is origcontext, not origcontext itself. */
    assert(rt->context->context.next == origcontext);
    assert(rt->stack->list.len == origlen); /* stack fully restored */

    free_rt(rt);
}

/* ---- Flow control ---- */

static void test_ift_and_ifte(void) {
    rpl_runtime *rt = make_rt();

    rpl_obj *th = rpl_new_int(&rt->gc, 111);
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 1)); /* truthy */
    rpl_list_push(&rt->gc, rt->stack, th);
    rpl_thunk next = call_internal(rt, "ift");
    next = step(rt, next); /* run th's own eval (pushes itself, default eval) */
    assert(rt->stack->list.len == 1);
    assert(rt->stack->list.data[0]->integer == 111);

    free_rt(rt);
    rt = make_rt();
    rpl_obj *th2 = rpl_new_int(&rt->gc, 1);
    rpl_obj *el2 = rpl_new_int(&rt->gc, 2);
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 0)); /* falsy */
    rpl_list_push(&rt->gc, rt->stack, th2);
    rpl_list_push(&rt->gc, rt->stack, el2);
    rpl_thunk n2 = call_internal(rt, "ifte");
    step(rt, n2);
    assert(rt->stack->list.data[rt->stack->list.len - 1]->integer == 2);

    free_rt(rt);
}

static void test_bail_and_beval(void) {
    rpl_runtime *rt = make_rt();

    /* bail from a nested (non-tail) frame drops back to the caller. */
    rpl_obj *outer_before = rt->context;
    rpl_obj *inner = rpl_new_list(&rt->gc, RPL_CODE);
    rpl_list_push(&rt->gc, inner, rpl_new_int(&rt->gc, 1)); /* padding so bail isn't at the end */
    rpl_list_push(&rt->gc, inner, rt->ret_internal);
    rpl_list_push(&rt->gc, inner, rt->ret_internal);
    /* Force a real (non-tail) call by making the current instruction not
     * be ret_internal: wrap in an outer code object. */
    rpl_obj *outer = rpl_new_list(&rt->gc, RPL_CODE);
    rpl_list_push(&rt->gc, outer, inner);
    rpl_list_push(&rt->gc, outer, rpl_new_int(&rt->gc, 77));
    rpl_list_push(&rt->gc, outer, rt->ret_internal);
    rpl_rs(rt, rpl_newcall(rt, outer));
    (void)outer_before;
    assert(rt->stack->list.len >= 1);
    assert(rt->stack->list.data[rt->stack->list.len - 1]->integer == 77);

    free_rt(rt);
}

/* ---- Conversions ---- */

static void test_num_to_int(void) {
    rpl_runtime *rt = make_rt();
    rpl_list_push(&rt->gc, rt->stack, rpl_new_float(&rt->gc, 3.9));
    call_internal(rt, "num>int");
    assert(rt->stack->list.data[0]->type == RPL_INTEGER);
    assert(rt->stack->list.data[0]->integer == 3);
    free_rt(rt);
}

static void test_str_to_sym(void) {
    rpl_runtime *rt = make_rt();
    rpl_list_push(&rt->gc, rt->stack, rpl_new_string(&rt->gc, "A.B", 3));
    call_internal(rt, "str>sym");
    rpl_obj *sym = rt->stack->list.data[0];
    assert(sym->type == RPL_SYMBOL);
    assert(sym->symbol.nparts == 2);
    assert(strcmp(sym->symbol.parts[0], "A") == 0);
    assert(strcmp(sym->symbol.parts[1], "B") == 0);
    free_rt(rt);
}

static void test_to_tag(void) {
    rpl_runtime *rt = make_rt();
    char *path[1] = { "T" };
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 5));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_symbol(&rt->gc, path, 1));
    call_internal(rt, ">tag");
    rpl_obj *tag = rt->stack->list.data[0];
    assert(tag->type == RPL_TAG);
    assert(strcmp(tag->tag.name, "T") == 0);
    assert(tag->tag.obj->integer == 5);
    free_rt(rt);
}

/* ---- Arithmetic / comparison ---- */

static void test_addint_and_lt(void) {
    rpl_runtime *rt = make_rt();
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 3));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 4));
    call_internal(rt, "+int");
    assert(rt->stack->list.data[0]->integer == 7);
    rt->stack->list.len--;

    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 3));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 10));
    call_internal(rt, "<"); /* 3 < 10 */
    assert(rt->stack->list.data[rt->stack->list.len - 1]->integer == 1);

    free_rt(rt);
}

/* ---- List / composite ---- */

static void test_tolst_tocode_composite_get_put(void) {
    rpl_runtime *rt = make_rt();

    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 1));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 2));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 3));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 3)); /* count */
    call_internal(rt, ">lst");
    rpl_obj *lst = rt->stack->list.data[rt->stack->list.len - 1];
    assert(lst->type == RPL_LIST);
    assert(lst->list.len == 3);

    /* composite>: explode back onto the stack plus a count */
    call_internal(rt, "composite>");
    assert(rt->stack->list.data[rt->stack->list.len - 1]->integer == 3); /* count */
    rt->stack->list.len -= 4; /* drop count + the 3 exploded items */

    /* >code: build a Code object (with trailing Return) from 2 items */
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 10));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 20));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 2));
    call_internal(rt, ">code");
    rpl_obj *code = rt->stack->list.data[rt->stack->list.len - 1];
    assert(code->type == RPL_CODE);
    assert(code->list.len == 3); /* 10, 20, Return */
    assert(code->list.data[2] == rt->ret_internal);
    rt->stack->list.len--;

    /* get / put on a fresh list */
    rpl_obj *l2 = rpl_new_list(&rt->gc, RPL_LIST);
    rpl_list_push(&rt->gc, l2, rpl_new_int(&rt->gc, 100));
    rpl_list_push(&rt->gc, l2, rpl_new_int(&rt->gc, 200));
    rpl_list_push(&rt->gc, rt->stack, l2);
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 1));
    call_internal(rt, "get");
    assert(rt->stack->list.data[rt->stack->list.len - 1]->integer == 200);
    rt->stack->list.len--;

    rpl_list_push(&rt->gc, rt->stack, l2);
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 999));
    rpl_list_push(&rt->gc, rt->stack, rpl_new_int(&rt->gc, 0));
    call_internal(rt, "put");
    rpl_obj *l3 = rt->stack->list.data[rt->stack->list.len - 1];
    assert(l3->list.data[0]->integer == 999);
    assert(l3->list.data[1]->integer == 200);
    assert(l2->list.data[0]->integer == 100); /* original untouched */

    free_rt(rt);
}

int main(void) {
    test_dup();
    test_swap();
    test_rot();
    test_pick();
    test_dropn();
    test_dropn_too_many_deds();
    test_roll();

    test_sto_rcl_exists_rm();
    test_sto_rejects_circular_symbol();
    test_sto_rejects_symbol_cycle();

    test_local_no_names_tailcalls();
    test_local_binds_name();
    test_local_rejects_circulation();

    test_ift_and_ifte();
    test_bail_and_beval();

    test_num_to_int();
    test_str_to_sym();
    test_to_tag();

    test_addint_and_lt();

    test_tolst_tocode_composite_get_put();

    printf("phase5: all tests passed\n");
    return 0;
}
