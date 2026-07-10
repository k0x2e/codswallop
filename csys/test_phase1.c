/* Smoke tests for the Phase 1 object model + GC: allocation, mark/sweep
 * collection, and cp() for every type that needs a non-trivial copy
 * (List/Code, Tag, Builtin, Directory -- including the sentinel and
 * subdirectory-recursion cases from typedir.cp()). */

#include "obj.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static rpl_gc gc;

static void test_int_float_immutable_cp(void) {
    rpl_obj *i = rpl_new_int(&gc, 42);
    rpl_obj *f = rpl_new_float(&gc, 3.5);
    assert(rpl_cp(&gc, i) == i);   /* immutable types return themselves */
    assert(rpl_cp(&gc, f) == f);
}

static void test_list_cp_is_shallow(void) {
    rpl_obj *list = rpl_new_list(&gc, RPL_LIST);
    rpl_obj *a = rpl_new_int(&gc, 1);
    rpl_obj *b = rpl_new_int(&gc, 2);
    rpl_list_push(&gc, list, a);
    rpl_list_push(&gc, list, b);

    rpl_obj *copy = rpl_cp(&gc, list);
    assert(copy != list);
    assert(copy->list.data != list->list.data);
    assert(copy->list.len == 2);
    assert(copy->list.data[0] == a);   /* same elements, new array */
    assert(copy->list.data[1] == b);
}

static void test_tag_cp(void) {
    rpl_obj *val = rpl_new_int(&gc, 7);
    rpl_obj *tag = rpl_new_tag(&gc, "X", val);
    rpl_obj *copy = rpl_cp(&gc, tag);
    assert(copy != tag);
    assert(strcmp(copy->tag.name, "X") == 0);
    assert(copy->tag.obj == val);  /* same contained object, not deep-copied */
}

static void test_builtin_cp(void) {
    rpl_obj *bin = rpl_new_builtin(&gc, "PLUS", "adds things", 2);
    int types[2] = {RPL_INTEGER, RPL_INTEGER};
    rpl_obj *handler = rpl_new_int(&gc, 0); /* stand-in for a real handler */
    rpl_builtin_add_dispatch(&gc, bin, types, handler);

    rpl_obj *copy = rpl_cp(&gc, bin);
    assert(copy != bin);
    assert(copy->builtin.argck != bin->builtin.argck);
    assert(copy->builtin.dispatches != bin->builtin.dispatches);
    assert(copy->builtin.ndispatches == 1);
    assert(copy->builtin.argck[0] == RPL_INTEGER);
    assert(copy->builtin.dispatches[0] == handler);
}

/* Builds a directory chain: head -> mid -> lastobj (self-referential
 * sentinel), mirroring how the named store terminates in Python. */
static rpl_obj *make_sentinel(void) {
    rpl_obj *tag = rpl_new_tag(&gc, "lastobj", rpl_new_int(&gc, 0));
    return rpl_new_dir(&gc, tag, NULL);
}

static void test_dir_cp_stops_at_sentinel(void) {
    rpl_obj *lastobj = make_sentinel();
    rpl_obj *mid = rpl_new_dir(&gc, rpl_new_tag(&gc, "mid", rpl_new_int(&gc, 2)), lastobj);
    rpl_obj *head = rpl_new_dir(&gc, rpl_new_tag(&gc, "head", rpl_new_int(&gc, 1)), mid);

    rpl_obj *copy = rpl_cp(&gc, head);
    assert(copy != head);
    assert(strcmp(copy->dir.tag->tag.name, "head") == 0);
    assert(copy->dir.next != mid);
    assert(strcmp(copy->dir.next->dir.tag->tag.name, "mid") == 0);
    /* The chain must terminate at the very same sentinel, not a copy of it. */
    assert(copy->dir.next->dir.next == lastobj);
}

/* Mirrors a quirk of the original Python typedir.cp(): the subdirectory
 * recursion check only runs on entries visited inside the copy loop (i.e.
 * self.next onward), never on the head node's own tag. So the subdirectory
 * has to sit one level in for recursion to kick in. */
static void test_dir_cp_recurses_into_subdirectories(void) {
    rpl_obj *lastobj = make_sentinel();
    rpl_obj *subdir = rpl_new_dir(&gc, rpl_new_tag(&gc, "inner", rpl_new_int(&gc, 99)), lastobj);
    rpl_obj *sub_node = rpl_new_dir(&gc, rpl_new_tag(&gc, "sub", subdir), lastobj);
    rpl_obj *head = rpl_new_dir(&gc, rpl_new_tag(&gc, "head", rpl_new_int(&gc, 1)), sub_node);

    rpl_obj *copy = rpl_cp(&gc, head);
    rpl_obj *copied_sub = copy->dir.next->dir.tag->tag.obj;
    assert(copied_sub != subdir);              /* subdirectory was recursed into */
    assert(copied_sub->type == RPL_DIRECTORY);
    assert(strcmp(copied_sub->dir.tag->tag.name, "inner") == 0);
    assert(copied_sub->dir.next == lastobj);    /* still terminates at the real sentinel */
}

static void test_gc_frees_unreachable(void) {
    rpl_obj *keep = rpl_new_int(&gc, 1);
    gc_add_perm_root(&gc, &keep);

    rpl_new_int(&gc, 2);   /* garbage: nothing roots this */
    rpl_new_int(&gc, 3);   /* garbage too */

    gc_collect(&gc);
    /* `keep` survives; everything else -- these two ints plus all the
     * unrooted objects earlier tests left behind -- gets reclaimed. */
    assert(gc.count == 1);
    assert(keep->integer == 1);

    gc_remove_perm_root(&gc, &keep);
}

static void test_gc_root_stack_protects_local(void) {
    rpl_obj *local = rpl_new_int(&gc, 123);
    gc_root_push(&gc, &local);

    /* Simulate other allocations happening while `local` is only reachable
     * via the root stack. */
    rpl_new_float(&gc, 1.0);
    gc_collect(&gc);

    assert(local->integer == 123);  /* still alive and untouched */
    gc_root_pop(&gc, 1);

    gc_collect(&gc);  /* now unrooted; should be reclaimed without crashing */
}

int main(void) {
    gc_init(&gc);

    test_int_float_immutable_cp();
    test_list_cp_is_shallow();
    test_tag_cp();
    test_builtin_cp();
    test_dir_cp_stops_at_sentinel();
    test_dir_cp_recurses_into_subdirectories();
    test_gc_frees_unreachable();
    test_gc_root_stack_protects_local();

    gc_destroy(&gc);
    printf("phase1: all tests passed\n");
    return 0;
}
