/* Mirrors pysys/rpl.py: the real entry point of the interpreter. Argument
 * parsing, SIGINT handling, and the final rpl_rs() call live here; the
 * runtime-construction/ROM-loading sequence itself is factored out into
 * rpl_boot() (boot.h/boot.c) so it can be exercised by a non-interactive
 * test (test_phase7.c) without also dragging in signal handling or a
 * blocking rs() run.
 *
 * ##########################################################################
 * argv parsing -- ported *exactly*, quirk included:
 *
 * pysys/rpl.py's argv handling is:
 *
 *   argv = sys.argv
 *   if len(argv)>2 and argv[1]=='-p':
 *     personality = argv[2]
 *     argv = argv[3:]
 *   else:
 *     personality = PERSONALITY
 *   if len(argv)>1:
 *     ourRT.Stack.push(typestr(argv[1]))
 *   else:
 *     ourRT.Stack.push(typestr(""))
 *
 * Read closely, this has a real off-by-one bug: in the `-p` branch, argv is
 * rebound to argv[3:], which -- unlike the untouched argv in the `else`
 * branch -- no longer has the script name occupying slot 0. So `argv[1]`
 * after the rebind is the *second* remaining positional argument, not the
 * first. Concretely, for `rpl.py -p pycold myprogram.rpl` (the exact
 * invocation CLAUDE.md's own "Running" section documents!):
 *   sys.argv = ['rpl.py', '-p', 'pycold', 'myprogram.rpl']   (len 4)
 *   argv = argv[3:] = ['myprogram.rpl']                       (len 1)
 *   len(argv)>1 is False -> pushes "" instead of the filename.
 * So specifying -p together with a filename silently drops the filename
 * in the reference implementation. This is reproduced faithfully below
 * (see the identical slicing arithmetic) rather than "fixed", per this
 * phase's job being a faithful port -- flagged here and in the final
 * report for whoever owns PLAN.md next.
 * ##########################################################################
 *
 * SIGINT handling: mirrors catchsigint(signal, frame) in pysys/rpl.py:
 *   def catchsigint(signal, frame):
 *     if ourRT.dieanyway:
 *       raise KeyboardInterrupt
 *     else:
 *       ourRT.Break = True
 * Setting rt->brk is a direct port. The `dieanyway` branch is not: Python's
 * `raise KeyboardInterrupt` unwinds the *entire* call stack via an
 * exception, including out of the blocking `input()` call inside `prompt`
 * (the only place dieanyway is ever set). C has no equivalent unwind
 * mechanism through a blocked fgets(3) frame, and this codebase has no
 * longjmp-based error path (rpl_ded works by returning control to the
 * trampoline via a thunk, which only works between eval steps, not out of
 * a blocking libc call). Rather than build a bespoke longjmp/sigsetjmp
 * unwind for this one case, the handler here restores the default SIGINT
 * disposition and re-raises, which terminates the process the same way a
 * second Ctrl-C during a Python `input()` prompt effectively does from the
 * user's perspective (the program stops right there) -- simpler, and matches
 * user-visible behavior, at the cost of not running any of our own cleanup.
 * This judgment call is called out again in the Phase 7 report. */

#include "boot.h"
#include "internals.h"
#include "rom.h"
#include "types.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static rpl_runtime *g_rt = NULL;

static void catchsigint(int signum) {
    (void)signum;
    if (g_rt && g_rt->dieanyway) {
        /* Restore default disposition and re-raise: terminates the process,
         * mirroring the user-visible effect of Python's KeyboardInterrupt
         * unwinding out of the blocked input() call in `prompt`. See the
         * file-header comment for the full reasoning. */
        signal(SIGINT, SIG_DFL);
        raise(SIGINT);
        return;
    }
    if (g_rt)
        g_rt->brk = 1;
}

int main(int argc, char **argv) {
    const char *personality;
    int n;
    char **args;

    if (argc > 2 && strcmp(argv[1], "-p") == 0) {
        personality = argv[2];
        args = argv + 3;
        n = argc - 3;
    } else {
        personality = "default";
        args = argv;
        n = argc;
    }

    const char *argstr = (n > 1) ? args[1] : "";

    rpl_runtime rt;
    rpl_runtime_init(&rt);

    if (!rpl_boot(&rt, personality, argstr)) {
        fprintf(stderr, "rpl: failed to boot personality '%s'\n", personality);
        rpl_runtime_destroy(&rt);
        return 1;
    }

    g_rt = &rt;
    signal(SIGINT, catchsigint);

    rpl_rs(&rt, (rpl_thunk){ rpl_context_eval, rt.context });

    g_rt = NULL;
    rpl_runtime_destroy(&rt);
    return 0;
}
