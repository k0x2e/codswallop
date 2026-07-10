#ifndef RPL_ROM_H
#define RPL_ROM_H

#include "rpl.h"
#include "runtime.h"

/* Mirrors pysys/rom.py. The ROM wire format is unchanged from the Python
 * reference (see the comment at the top of pysys/rom.py):
 *
 *   Title[typenum:typename,...]CROM<count> <typenum>:<objdata> ... .
 *
 * Numbers are read directly; String/Comment/Symbol/Internal are preceded
 * by a byte count and then read raw; List/Code give an item count followed
 * by that many store-index references; Directory/Tag/Context/Builtin/
 * Dispatch each have their own compact encoding (see rom.c for the
 * per-type breakdown, which follows loadrom()'s branches one for one).
 *
 * The ROM's header type numbers are *not* assumed to already match our
 * fixed RPL_* enum (even though empirically, for personality/default.rom,
 * they do -- rpl.h's enum was deliberately ordered to mirror
 * pysys/rtypes.py's baseregistry()). loadrom translates each header entry
 * from its declared name to the RPL_* value via rpl_type_by_name, honoring
 * the format's "self-describing" design instead of hard-coding the
 * coincidence. */

/* Pops a String off rt->stack (the whole ROM text) and parses it in place,
 * building every object it describes and pushing the final one (or, if
 * the stream's last entry is a Dispatch patch, the builtin it patched)
 * back onto rt->stack. Returns 1 on success. On a malformed/unsupported
 * ROM, prints a diagnostic to stderr and reports the error via rpl_ded,
 * returning 0 -- this is a deliberate improvement over the Python
 * original's bare `except: print(...); ded` handler, which is a no-op
 * (referencing the imported `ded` function without calling it) and so
 * silently continues with a partially-built, likely-corrupt store; that
 * is not a safe thing to reproduce in C, where the equivalent would risk
 * dereferencing NULL store slots. */
int rpl_loadrom(rpl_runtime *rt);

/* Mirrors romboot(rt, file): reads the whole named file, pushes it as a
 * String, calls rpl_loadrom, and installs the result as rt->context (the
 * stored object is expected to be a Context -- this is a boot-time-only
 * operation, not general-purpose deserialization). Also mirrors two
 * details of the Python original faithfully, oddities included:
 *   - it captures whatever's currently stored at ["I*"] into a local
 *     before loading (matching `ints=rt.rcl(['I*'])`), then re-stores it
 *     under the literal path ["i**"] afterward (matching
 *     `rt.sto(['i**'],ints)`) -- note the lowercase/doubled-asterisk name
 *     is *not* the same path as INTERNALSDIR ("I*"); this looks like an
 *     unused leftover/typo in the Python reference (nothing else in
 *     pysys ever reads "i**"), reproduced here rather than silently
 *     "fixed", since this phase's job is a faithful port
 *   - the caller must have already stored the internals directory at
 *     ["I*"] (see rpl_stoprocs's precondition) before calling this
 * Returns 1 on success, 0 if the file couldn't be read or the ROM failed
 * to parse (rpl_ded has already been called in the latter case).
 */
int rpl_romboot(rpl_runtime *rt, const char *filename);

/* The "evalrom" internal: pops a String, loads it via rpl_loadrom, and
 * delegates (uncalled, like typesym.eval) to the resulting object's own
 * eval thunk -- mirrors pysys/rom.py's evalrom(rt). This is the internal
 * pysys/internals.py's stoprocs() stores from pysys/rom.py rather than
 * pysys/internals.py itself; Phase 5's rpl_stoprocs deliberately left it
 * unregistered pending this phase. Callers wiring up the full internals
 * table (see rpl_stoprocs in internals.c) should register this under
 * "evalrom" once rom.c is linked in. */
rpl_thunk rpl_evalrom(rpl_runtime *rt, rpl_obj *self);

#endif /* RPL_ROM_H */
