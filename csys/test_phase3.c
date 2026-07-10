/* Smoke tests for Phase 3 (per-type eval methods): Quote, Comment, Tag,
 * Code, Symbol, and Builtin, plus rpl_register_types populating the Types
 * directory. Integer/Float/String/Directory/Handle are exercised
 * indirectly already (Phase 1/2 tests push them via rpl_default_eval). */

#include "types.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Runs one eval step through the trampoline manually (without halting the
 * whole runtime): calls thunk.fn once and returns the result, so tests can
 * inspect intermediate state without running rpl_rs() to completion. */
static rpl_thunk step(rpl_runtime *rt, rpl_thunk t) {
    (void)rt;
    return t.fn(rt, t.self);
}

/* Evaluating 'X should push X's contents, not the quote wrapper itself. */
static void test_quote_eval_pushes_inner(void) {
    rpl_runtime rt;
    rpl_runtime_init(&rt);

    rpl_obj *inner = rpl_new_int(&rt.gc, 5);
    rpl_obj *quote = rpl_new_quote(&rt.gc, inner);

    step(&rt, rpl_obj_eval(&rt, quote));

    assert(rt.stack->list.len == 1);
    assert(rt.stack->list.data[0] == inner);

    rpl_runtime_destroy(&rt);
}

/* A Comment vanishes at eval time: nothing gets pushed. */
static void test_comment_eval_pushes_nothing(void) {
    rpl_runtime rt;
    rpl_runtime_init(&rt);

    rpl_obj *rem = rpl_new_comment(&rt.gc, "hi", 2);
    step(&rt, rpl_obj_eval(&rt, rem));

    assert(rt.stack->list.len == 0);

    rpl_runtime_destroy(&rt);
}

/* A Tag pushes itself, then resolves through usreval (the base case of
 * which is just "resume the current context", same as the default). */
static void test_tag_eval_pushes_self_then_usreval(void) {
    rpl_runtime rt;
    rpl_runtime_init(&rt);

    rpl_obj *tag = rpl_new_tag(&rt.gc, "X", rpl_new_int(&rt.gc, 1));
    rpl_thunk next = step(&rt, rpl_obj_eval(&rt, tag));

    assert(rt.stack->list.len == 1);
    assert(rt.stack->list.data[0] == tag);
    assert(next.fn == rpl_tag_usreval);
    assert(next.self == tag);

    rpl_runtime_destroy(&rt);
}

/* Evaluating a Code object doesn't push it -- it calls it, via newcall.
 * Landing on the current (bare) context's tail position, this is a tail
 * call: the context slot is reused rather than growing the call stack. */
static void test_code_eval_calls_via_newcall(void) {
    rpl_runtime rt;
    rpl_runtime_init(&rt);
    rpl_obj *before = rt.context;

    rpl_obj *code = rpl_new_list(&rt.gc, RPL_CODE);
    rpl_list_push(&rt.gc, code, rpl_new_int(&rt.gc, 7));
    rpl_list_push(&rt.gc, code, rt.ret_internal);

    rpl_thunk next = step(&rt, rpl_obj_eval(&rt, code));
    assert(rt.context == before);   /* tail-call reused, not replaced */

    rpl_rs(&rt, next);
    assert(rt.running == 0);
    assert(rt.stack->list.len == 1);
    assert(rt.stack->list.data[0]->integer == 7);

    rpl_runtime_destroy(&rt);
}

/* A Symbol resolves through the named store and delegates to the
 * resolved object's own eval, unresolved (i.e. it returns a thunk, it
 * doesn't run it) -- exactly like Python's `return x.eval`. */
static void test_symbol_eval_resolves_and_delegates(void) {
    rpl_runtime rt;
    rpl_runtime_init(&rt);

    char *path[1] = { "X" };
    rpl_obj *val = rpl_new_int(&rt.gc, 42);
    rpl_sto(&rt, path, 1, val);

    rpl_obj *sym = rpl_new_symbol(&rt.gc, path, 1);
    rpl_thunk next = step(&rt, rpl_obj_eval(&rt, sym));

    /* Nothing pushed yet -- symbol eval only delegates, it's the returned
     * thunk (val's own default eval) that actually pushes on the next step. */
    assert(rt.stack->list.len == 0);
    step(&rt, next);
    assert(rt.stack->list.len == 1);
    assert(rt.stack->list.data[0] == val);

    rpl_runtime_destroy(&rt);
}

/* An unresolvable Symbol reports a ded() error naming the missing path and
 * blames rtcaller, mirroring Python's typesym.eval failure branch. */
static void test_symbol_eval_missing_deds(void) {
    rpl_runtime rt;
    rpl_runtime_init(&rt);

    char *path[2] = { "NO", "SUCH" };
    rpl_obj *sym = rpl_new_symbol(&rt.gc, path, 2);
    step(&rt, rpl_obj_eval(&rt, sym));

    assert(rt.caller == rt.rtcaller);
    assert(rt.reason != NULL);
    assert(strstr(rt.reason, "NO.SUCH") != NULL);

    rpl_runtime_destroy(&rt);
}

/* Builds a two-dispatch Builtin: (Integer) -> handlerA, (Any) -> handlerB
 * (a catch-all fallback), each a distinct binproc that just pushes a
 * marker so the test can tell which one fired. */
static rpl_obj *make_test_builtin(rpl_runtime *rt, rpl_eval_fn markA, rpl_eval_fn markB) {
    rpl_obj *bin = rpl_new_builtin(&rt->gc, "TESTOP", "test op", 1);
    int int_type[1] = { RPL_INTEGER };
    int any_type[1] = { RPL_ANY };
    rpl_builtin_add_dispatch(&rt->gc, bin, int_type, rpl_new_binproc(&rt->gc, "A", markA));
    rpl_builtin_add_dispatch(&rt->gc, bin, any_type, rpl_new_binproc(&rt->gc, "B", markB));
    return bin;
}

static rpl_thunk mark_a(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_list_push(&rt->gc, rt->stack, rpl_new_string(&rt->gc, "A", 1));
    return (rpl_thunk){ rpl_context_eval, rt->context };
}

static rpl_thunk mark_b(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    rpl_list_push(&rt->gc, rt->stack, rpl_new_string(&rt->gc, "B", 1));
    return (rpl_thunk){ rpl_context_eval, rt->context };
}

/* First matching row (top to bottom) wins: an Integer argument matches
 * the first (Integer-typed) row before it ever reaches the Any fallback. */
static void test_builtin_eval_dispatches_first_match(void) {
    rpl_runtime rt;
    rpl_runtime_init(&rt);

    rpl_obj *bin = make_test_builtin(&rt, mark_a, mark_b);
    rpl_list_push(&rt.gc, rt.stack, rpl_new_int(&rt.gc, 3));   /* the one arg */

    rpl_thunk next = step(&rt, rpl_obj_eval(&rt, bin));
    step(&rt, next);   /* run the delegated-to handler */

    assert(rt.caller == bin);
    assert(rt.stack->list.len == 2);   /* the Integer arg, then "A" */
    assert(strcmp(rt.stack->list.data[1]->string.data, "A") == 0);

    rpl_runtime_destroy(&rt);
}

/* A non-Integer argument skips the Integer-typed row and falls through to
 * the Any-typed fallback row. */
static void test_builtin_eval_falls_through_to_any(void) {
    rpl_runtime rt;
    rpl_runtime_init(&rt);

    rpl_obj *bin = make_test_builtin(&rt, mark_a, mark_b);
    rpl_list_push(&rt.gc, rt.stack, rpl_new_string(&rt.gc, "not an int", 10));

    rpl_thunk next = step(&rt, rpl_obj_eval(&rt, bin));
    step(&rt, next);

    assert(rt.stack->list.len == 2);
    assert(strcmp(rt.stack->list.data[1]->string.data, "B") == 0);

    rpl_runtime_destroy(&rt);
}

/* Too few arguments on the stack reports a ded() error instead of
 * underflowing the stack. */
static void test_builtin_eval_too_few_args_deds(void) {
    rpl_runtime rt;
    rpl_runtime_init(&rt);

    rpl_obj *bin = make_test_builtin(&rt, mark_a, mark_b);
    /* Stack is empty: bin wants 1 argument. */
    step(&rt, rpl_obj_eval(&rt, bin));

    assert(rt.caller == bin);
    assert(rt.reason != NULL);
    assert(strstr(rt.reason, "arguments") != NULL);

    rpl_runtime_destroy(&rt);
}

/* rpl_register_types must populate Types.<Name> = <RPL_* number> for every
 * type, plus Types.n as an in-order list of their names. */
static void test_register_types(void) {
    rpl_runtime rt;
    rpl_runtime_init(&rt);
    rpl_register_types(&rt);

    char *int_path[2] = { "Types", "Integer" };
    rpl_obj *v = rpl_rcl(&rt, int_path, 2);
    assert(v && v->type == RPL_INTEGER && v->integer == RPL_INTEGER);

    char *quote_path[2] = { "Types", "Quote" };
    v = rpl_rcl(&rt, quote_path, 2);
    assert(v && v->integer == RPL_QUOTE);

    char *n_path[2] = { "Types", "n" };
    rpl_obj *n = rpl_rcl(&rt, n_path, 2);
    assert(n && n->type == RPL_LIST);
    assert(n->list.len == RPL_NTYPES);
    assert(strcmp(n->list.data[0]->string.data, "Any") == 0);
    assert(strcmp(n->list.data[RPL_INTEGER]->string.data, "Integer") == 0);

    rpl_runtime_destroy(&rt);
}

int main(void) {
    test_quote_eval_pushes_inner();
    test_comment_eval_pushes_nothing();
    test_tag_eval_pushes_self_then_usreval();
    test_code_eval_calls_via_newcall();
    test_symbol_eval_resolves_and_delegates();
    test_symbol_eval_missing_deds();
    test_builtin_eval_dispatches_first_match();
    test_builtin_eval_falls_through_to_any();
    test_builtin_eval_too_few_args_deds();
    test_register_types();

    printf("phase3: all tests passed\n");
    return 0;
}
