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
 * `raise KeyboardInterrupt` unwinds out of the blocking `input()` call
 * inside `prompt` (the only place dieanyway is ever set) via an exception,
 * caught by prompt's own bare `except:`, which reports it through the
 * ordinary ded() path -- the interpreter survives a Ctrl-C at the prompt.
 * An earlier version of this port instead re-raised SIGINT with its
 * default disposition here, terminating the whole process -- simpler, but
 * a real behavior change from the Python reference, not just an
 * implementation detail. Fixed by giving bi_prompt (internals.c) a
 * sigsetjmp() around its blocking fgets(); this handler now calls
 * siglongjmp(rt->intr_buf, 1) instead, landing back there as if the read
 * had failed, which funnels into the same ded() call a real EOF/read error
 * would. intr_buf_active guards the (should-never-happen) case of a
 * stray SIGINT with dieanyway set but no sigsetjmp currently active, in
 * which case this falls back to the ordinary rt->brk = 1 path rather than
 * jumping to an unarmed buffer. */

#include "boot.h"
#include "internals.h"
#include "rom.h"
#include "types.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

/* Cap the process's total address space so a runaway allocation (an
 * infinite-growth bug hit while running test programs, say) fails fast
 * instead of eating all system memory. Every allocation in csys/ already
 * funnels through an xrealloc() helper (gc.c, obj.c) that treats a failed
 * malloc/realloc as a hard abort with a message rather than returning NULL
 * to a caller that won't check it, so once this limit is hit the process
 * dies cleanly on its own instead of needing a SIGKILL from the OOM killer.
 * This has no Python-reference analogue -- it's a C-port-only safety net,
 * not a language feature -- so it lives here rather than in rpl.h next to
 * CALLDEPTH/CPDEPTH. */
#define RPL_MEM_LIMIT_BYTES ((rlim_t)128 * 1024 * 1024)

static void limit_memory(void) {
    struct rlimit rl = { RPL_MEM_LIMIT_BYTES, RPL_MEM_LIMIT_BYTES };
    if (setrlimit(RLIMIT_AS, &rl) != 0)
        perror("rpl: setrlimit(RLIMIT_AS) failed, continuing without a memory cap");
}

static rpl_runtime *g_rt = NULL;

static void catchsigint(int signum) {
    (void)signum;
    if (g_rt && g_rt->dieanyway && g_rt->intr_buf_active) {
        /* Jump back to the sigsetjmp() in bi_prompt (internals.c), as if
         * its blocked fgets() had failed -- see the file-header comment. */
        siglongjmp(g_rt->intr_buf, 1);
    }
    if (g_rt)
        g_rt->brk = 1;
}

int main(int argc, char **argv) {
    limit_memory();

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
