#ifndef RPL_BOOT_H
#define RPL_BOOT_H

#include "rpl.h"
#include "runtime.h"

/* Mirrors the body of pysys/rpl.py (everything between constructing the
 * runtime and installing the SIGINT handler / calling rs()): given an
 * *already inited* runtime (rpl_runtime_init has been called, but
 * rpl_register_types has NOT yet -- this function does that too, matching
 * the fact that rpl.py's baseregistry()/rplruntime() constructor pairing
 * happens before any of the "I*" / stoprocs / argv / romboot work), this:
 *
 *   1. calls rpl_register_types(rt) (Phase 3 registry, done once here so
 *      callers don't have to remember the ordering)
 *   2. creates the "I*" directory and fills it via rpl_stoprocs, matching
 *      rpl.py's exact sequence:
 *        ourRT.sto([INTERNALSDIR], ourRT.firstdir(ourRT.lastobj))
 *        stoprocs(ourRT, INTERNALSDIR)
 *        ourRT.sto([INTERNALSDIR,'semicolon'], ourRT.Return)
 *        ourRT.sto([INTERNALSDIR,'lastobj'],   ourRT.lastobj)
 *        ourRT.sto([INTERNALSDIR,'nulltag'],   ourRT.nulltag)
 *   3. pushes `argstr` (typically the filename argument, or "" if none)
 *      onto rt->stack as a String, matching rpl.py's
 *        ourRT.Stack.push(typestr(argv[1]))  /  typestr("")
 *   4. calls rpl_romboot(rt, "personality/<personality>.rom"), matching
 *        romboot(ourRT, f'personality/{personality}.rom')
 *      (relative to the current working directory -- callers are expected
 *      to run from the repo root, exactly like the Python original; this
 *      function does not attempt to locate the repo root itself)
 *
 * Does NOT install the SIGINT handler or call rpl_rs -- those stay in
 * main() (a signal handler needs process-wide state a boot helper
 * shouldn't own), and are also not meaningful to unit-test the same way.
 *
 * Returns 1 on success, 0 if rpl_romboot failed (rpl_ded has already been
 * called in that case; rt->reason describes what went wrong). */
int rpl_boot(rpl_runtime *rt, const char *personality, const char *argstr);

#endif /* RPL_BOOT_H */
