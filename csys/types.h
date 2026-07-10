#ifndef RPL_TYPES_H
#define RPL_TYPES_H

#include "rpl.h"
#include "runtime.h"

/* Per-type eval overrides that aren't just "push self, resume the current
 * context" (that shared default lives in runtime.c as rpl_default_eval).
 * Each of these mirrors the objarchetype subclass's eval() method from
 * pysys/rtypes.py. They're wired into rpl_obj_eval's dispatch switch in
 * runtime.c -- this header just gives that switch something to point at. */

/* typequote.eval: pushes the quoted *contents* (self->quote.inner), not
 * the quote object itself, then resumes the current context. This is what
 * lets 'X shield a symbol or code block from being evaluated on the spot. */
rpl_thunk rpl_quote_eval(rpl_runtime *rt, rpl_obj *self);

/* typerem.eval: a Comment is inert at runtime -- it vanishes without
 * pushing anything, just resuming the current context. */
rpl_thunk rpl_comment_eval(rpl_runtime *rt, rpl_obj *self);

/* typetag.eval: pushes the tag itself, then hands off to rpl_tag_usreval
 * rather than the current context directly, mirroring Python's
 * self.usreval indirection (the hook user-defined types patch to get
 * method-call-like behavior out of a stored tag). */
rpl_thunk rpl_tag_eval(rpl_runtime *rt, rpl_obj *self);

/* typetag.usreval: the base behavior rpl_tag_eval hands off to -- just
 * resume the current context. Exists as its own function (rather than
 * being inlined into rpl_tag_eval) so it's a distinct thunk target user
 * types could eventually override. */
rpl_thunk rpl_tag_usreval(rpl_runtime *rt, rpl_obj *self);

/* typecode.eval: evaluating a code object doesn't push it to the data
 * stack -- it calls it, via rpl_newcall, growing (or tail-call-reusing)
 * the call stack instead. */
rpl_thunk rpl_code_eval(rpl_runtime *rt, rpl_obj *self);

/* typesym.eval: looks the symbol's dotted path up via rpl_rcl and
 * delegates to whatever it finds (returning that object's own eval thunk,
 * unresolved/uncalled, exactly like Python's `return x.eval`). Reports a
 * ded() error if the name doesn't resolve, or if a Break arrives here --
 * most circular references are caught at store time (rpl_circdir/circsym),
 * but this is the other place one could surface. */
rpl_thunk rpl_symbol_eval(rpl_runtime *rt, rpl_obj *self);

/* typebin.eval: checks the data stack holds at least argct items, reads
 * their type numbers, scans argck/dispatches for the first row whose
 * per-position type (0 == Any, matching anything) agrees with what's on
 * the stack, and delegates (uncalled, like typesym.eval) to that handler's
 * eval. ded()s if there aren't enough arguments or nothing matches. */
rpl_thunk rpl_builtin_eval(rpl_runtime *rt, rpl_obj *self);

/* Populates the Types directory in the named store the way
 * pysys/rtypes.py's baseregistry() + rpltypes.updatestore() do: Types.Any,
 * one Types.<Name> = <number> entry per registered type (in the same
 * order the enum in rpl.h already mirrors), and Types.n holding that name
 * list in order. There's no dynamic type-class registry to build here --
 * the C port's type numbers are the fixed RPL_* enum from rpl.h -- so this
 * only needs to reproduce the named-store side effect the parser (Phase 4)
 * and RPL-level introspection code will rely on. */
void rpl_register_types(rpl_runtime *rt);

/* Looks up an RPL_* type number by its canonical name (the same strings
 * rpl_register_types stores under Types.<Name>, e.g. "Integer", "String").
 * Used by rom.c's ROM-header parsing to translate a ROM's self-described
 * type-name table into this build's fixed RPL_* numbering, rather than
 * assuming the two happen to already agree (see rom.h's file-header
 * comment). Returns -1 if name doesn't match any registered type. */
int rpl_type_by_name(const char *name);

#endif /* RPL_TYPES_H */
