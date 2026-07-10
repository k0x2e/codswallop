#ifndef RPL_INTERNALS_H
#define RPL_INTERNALS_H

#include "rpl.h"
#include "runtime.h"

/* Mirrors pysys/internals.py's stoprocs(rt, dir): stores every internal
 * built by (the C equivalent of) makebinprocs() into the named store under
 * [dir, <name>], each wrapped as an RPL_INTERNAL (rpl_new_binproc). `dir`
 * is an unqualified name (e.g. "I*") -- matches how pysys/trivia.py's
 * INTERNALSDIR is used as a single-component path prefix.
 *
 * Precondition (same as the Python original): the `dir` directory itself
 * must already exist in the named store before calling this -- rpl_sto
 * only ever creates a *leaf* entry, never an intermediate directory.
 * pysys's rpl.py does this with
 *   ourRT.sto([INTERNALSDIR], ourRT.firstdir(ourRT.lastobj))
 * before calling stoprocs(); Phase 7's C main()/boot sequence should do
 * the C equivalent (rpl_sto(rt, {dir}, 1, rpl_firstdir(rt, rt->lastobj)))
 * before calling rpl_stoprocs.
 *
 * Deliberately NOT registered here (see internals.c's file-header comment
 * for the full rationale):
 *   - "evalrom": belongs to pysys/rom.py, i.e. Phase 6 (ROM loader), not
 *     Phase 5. Left out entirely; Phase 6 should add it once rpl_evalrom
 *     (or equivalent) exists.
 */
void rpl_stoprocs(rpl_runtime *rt, const char *dir);

#endif /* RPL_INTERNALS_H */
