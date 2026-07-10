/* Smoke tests for Phase 6 (ROM loader): a synthetic hand-built ROM string
 * exercising loadrom's core object types (with header type numbers chosen
 * deliberately *not* to match the RPL_* enum, to prove the header's
 * name-based translation actually does something rather than coincidentally
 * working), plus an integration check that personality/default.rom (the
 * real boot ROM shipped in the repo) parses end to end into a Context
 * object without error.
 *
 * Note: we deliberately do NOT run the loaded default.rom context through
 * rpl_rs() here -- the real boot sequence ends up blocking on `prompt`
 * (stdin) once it reaches the REPL, which would hang an automated test.
 * Actually executing the booted ROM under a controlled stdin is Phase 7/8
 * territory (main()'s boot sequence + validation), not this phase's job;
 * this test's job is "did the ROM parse into the right shape." */

#include "internals.h"
#include "rom.h"
#include "types.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mirrors rpl.py's boot sequence (the parts of it that matter for loading
 * a ROM): the internals directory, stoprocs(), and three extra bindings
 * rpl.py stores by hand *in addition to* stoprocs() --
 *   ourRT.sto([INTERNALSDIR, 'semicolon'], ourRT.Return)
 *   ourRT.sto([INTERNALSDIR, 'lastobj'], ourRT.lastobj)
 *   ourRT.sto([INTERNALSDIR, 'nulltag'], ourRT.nulltag)
 * -- because personality/default.rom's "Internal" objects reference
 * I*.semicolon/I*.lastobj/I*.nulltag by name, and none of those three are
 * part of makebinprocs()/rpl_internals_table. This is really Phase 7
 * (main/boot) territory; replicated here only so this phase's ROM-loader
 * test can exercise the real shipped ROM end to end. */
static rpl_runtime *make_rt(void) {
    rpl_runtime *rt = malloc(sizeof(*rt));
    rpl_runtime_init(rt);
    rpl_register_types(rt);
    char *dirpath[1] = { "I*" };
    rpl_sto(rt, dirpath, 1, rpl_firstdir(rt, rt->lastobj));
    rpl_stoprocs(rt, "I*");

    char *semipath[2] = { "I*", "semicolon" };
    rpl_sto(rt, semipath, 2, rt->ret_internal);
    char *lastpath[2] = { "I*", "lastobj" };
    rpl_sto(rt, lastpath, 2, rt->lastobj);
    char *nullpath[2] = { "I*", "nulltag" };
    rpl_sto(rt, nullpath, 2, rt->nulltag);

    return rt;
}

static void free_rt(rpl_runtime *rt) {
    rpl_runtime_destroy(rt);
    free(rt);
}

/* A tiny synthetic ROM: header declares Integer/String/Symbol/List under
 * made-up numbers (20-23) rather than their real RPL_* values (12, 5, 3,
 * 10), so this only passes if rpl_loadrom actually consults the header's
 * name mapping instead of assuming header numbers == RPL_* numbers.
 * (Numbers must stay under ROM_MAX_HEADER_TYPES in rom.c.) */
static const char *const synthetic_rom =
    "MyRom[20:Integer,21:String,22:Symbol,23:List]"
    "CROM4 20:5 21:3 abc 22:1 x 23:3 0 1 2 ";

static void test_loadrom_basic_types(void) {
    rpl_runtime *rt = make_rt();

    rpl_obj *text = rpl_new_string(&rt->gc, synthetic_rom, strlen(synthetic_rom));
    rpl_list_push(&rt->gc, rt->stack, text);

    int ok = rpl_loadrom(rt);
    assert(ok);
    assert(rt->stack->list.len == 1);

    rpl_obj *lst = rt->stack->list.data[0];
    assert(lst->type == RPL_LIST);
    assert(lst->list.len == 3);

    assert(lst->list.data[0]->type == RPL_INTEGER);
    assert(lst->list.data[0]->integer == 5);

    assert(lst->list.data[1]->type == RPL_STRING);
    assert(lst->list.data[1]->string.len == 3);
    assert(memcmp(lst->list.data[1]->string.data, "abc", 3) == 0);

    assert(lst->list.data[2]->type == RPL_SYMBOL);
    assert(lst->list.data[2]->symbol.nparts == 1);
    assert(strcmp(lst->list.data[2]->symbol.parts[0], "x") == 0);

    free_rt(rt);
}

/* A malformed ROM (missing CROM marker) should report an error via ded(),
 * not crash. */
static void test_loadrom_rejects_garbage(void) {
    rpl_runtime *rt = make_rt();
    const char *garbage = "nope[1:Integer]NOTCROM0 ";
    rpl_list_push(&rt->gc, rt->stack, rpl_new_string(&rt->gc, garbage, strlen(garbage)));

    int ok = rpl_loadrom(rt);
    assert(!ok);
    assert(rt->reason != NULL);

    free_rt(rt);
}

/* Round-trip a Dispatch-patched Builtin: build a ROM by hand that defines
 * a builtin, a one-row dispatch table for it, and a Dispatch entry ('-'
 * prefixed) patching it in. */
static void test_loadrom_builtin_and_dispatch(void) {
    rpl_runtime *rt = make_rt();

    /* Objects (0-indexed):
     * 0: Symbol "NAME"      (3:4 NAME)
     * 1: String "a hint"    (5:6 a hint)
     * 2: Integer 1          (12:1)              -- argct
     * 3: Builtin            (7:0 1 2)           -- name=store0, hint=store1, argct=store2
     * 4: Integer 12         (12:12)             -- one argck entry (Integer's own type number)
     * 5: Internal "dup"     (2:3 dup)           -- dispatch target (must exist under I*)
     * 6: List [5,4]         (10:2 5 4)          -- one dispatch line: [handler, argtype...]
     * 7: List [6]           (10:1 6)           -- the dispatch table (list of lines)
     * 8: Dispatch on 3 using table 7            (-3:7 )
     * Final object pushed is store[3] (last was Dispatch -> binobj). */
    char rom[512];
    snprintf(rom, sizeof(rom),
             "T[3:Symbol,5:String,12:Integer,7:Builtin,2:Internal,10:List]"
             "CROM9 3:4 NAME 5:6 a hint 12:1 7:0 1 2 12:12 2:3 dup 10:2 5 4 10:1 6 "
             "-3:7 ");

    rpl_list_push(&rt->gc, rt->stack, rpl_new_string(&rt->gc, rom, strlen(rom)));
    int ok = rpl_loadrom(rt);
    assert(ok);
    assert(rt->reason == NULL);

    rpl_obj *bin = rt->stack->list.data[rt->stack->list.len - 1];
    assert(bin->type == RPL_BUILTIN);
    assert(strcmp(bin->builtin.name, "NAME") == 0);
    assert(strcmp(bin->builtin.hint, "a hint") == 0);
    assert(bin->builtin.argct == 1);
    assert(bin->builtin.ndispatches == 1);
    assert(bin->builtin.argck[0] == RPL_INTEGER);
    assert(bin->builtin.dispatches[0]->type == RPL_INTERNAL);
    assert(strcmp(bin->builtin.dispatches[0]->binproc.name, "dup") == 0);

    free_rt(rt);
}

/* Integration check: the real boot ROM shipped in the repo parses into a
 * Context without error. Run from csys/, so the file lives one directory
 * up. */
static void test_romboot_default_rom(void) {
    rpl_runtime *rt = make_rt();

    int ok = rpl_romboot(rt, "../personality/default.rom");
    assert(ok);
    assert(rt->reason == NULL);
    assert(rt->context != NULL);
    assert(rt->context->type == RPL_CONTEXT);

    free_rt(rt);
}

int main(void) {
    test_loadrom_basic_types();
    test_loadrom_rejects_garbage();
    test_loadrom_builtin_and_dispatch();
    test_romboot_default_rom();

    printf("phase6: all tests passed\n");
    return 0;
}
