#ifndef RPL_RUNTIME_H
#define RPL_RUNTIME_H

#include "rpl.h"
#include "gc.h"
#include "obj.h"

/* Mirrors pysys/runtime.py's rplruntime. Holds everything the trampoline,
 * named store, and (eventually) internals need to run a program: the gc,
 * the data stack, the call-stack chain (context), the handful of sentinel
 * objects that terminate/fill structures, and the small bits of error
 * state builtins set (Caller/Reason/Interrupt). */
typedef struct rpl_runtime {
    rpl_gc gc;

    rpl_obj *stack;    /* data stack: a RPL_LIST, top = last element */
    rpl_obj *context;  /* current call-stack frame (RPL_CONTEXT) */

    rpl_obj *nulltag;    /* null-named tag used to fill firstdir/lastobj nodes */
    rpl_obj *lastobj;    /* self-referential RPL_DIRECTORY: terminates every chain */
    rpl_obj *nullcode;   /* RPL_CODE containing just ret_internal; filler for ded() */
    rpl_obj *ret_internal; /* the shared "semicolon"/Return internal (rpl_ret) */
    rpl_obj *dedsym;     /* pre-built ['EXCEPT'] symbol; what ded() evaluates */

    rpl_obj *nullcaller; /* Caller reset value: empty string */
    rpl_obj *rtcaller;   /* Caller value used for runtime-raised errors */
    rpl_obj *caller;     /* current "who signaled this error" string, builtin-settable */

    char *reason;   /* owned copy of the last ded() reason string */

    int running;    /* cleared to stop the rs() trampoline */
    int brk;        /* set asynchronously (SIGINT) to request a Break error */
    int interrupt;  /* set once a Break has actually been delivered via ded() */
    int dieanyway;
} rpl_runtime;

/* Allocates and wires up every sentinel object (nulltag, lastobj, nullcode,
 * ret_internal, dedsym, caller strings), registers them as permanent GC
 * roots, and creates the first Context so the runtime is ready for rs().
 * Mirrors rplruntime.__init__ minus the Types-registry bookkeeping, which
 * belongs to Phase 3. */
void rpl_runtime_init(rpl_runtime *rt);

/* Frees the reason string and tears down the gc (and therefore every
 * object the runtime ever allocated). */
void rpl_runtime_destroy(rpl_runtime *rt);

/* The generic "what does evaluating this object do" dispatch -- the C
 * equivalent of taking Python's obj.eval bound method, without calling it.
 * Returns a thunk (fn, self) for the caller to invoke later; never has a
 * side effect itself. Context, Internal, Quote, Comment, Tag, Code, Symbol,
 * and Builtin each have their own eval behavior (see types.h for the latter
 * six); every other type (Integer, Float, String, Directory, Handle) falls
 * through to rpl_default_eval: push self, keep running the current context. */
rpl_thunk rpl_obj_eval(rpl_runtime *rt, rpl_obj *o);

/* Default per-type eval: push self onto the data stack and resume the
 * current context. Used by every type that doesn't override evaluation
 * (Integer, Float, String, Directory, Tag, List, Quote, Handle, ...). */
rpl_thunk rpl_default_eval(rpl_runtime *rt, rpl_obj *self);

/* typecontext.eval: checks for a pending Break first (turning it into a
 * ded() call), otherwise fetches the eval thunk for the instruction at
 * code->list.data[ip], advances ip, and returns it. self must be a
 * RPL_CONTEXT object (normally rt->context). */
rpl_thunk rpl_context_eval(rpl_runtime *rt, rpl_obj *self);

/* The "semicolon"/Return internal every code object ends with. Drops the
 * current context: if it's the bottom of the call stack (its own next),
 * the runtime halts; otherwise control resumes in the enclosing context. */
rpl_thunk rpl_ret(rpl_runtime *rt, rpl_obj *self);

/* Runtime error handler: records reason, forces a new context (wrapping
 * the current one, sharing its names) so the handler isn't subject to the
 * ordinary call-depth limit or a tail-call collision, and returns the
 * eval thunk for the DEDEVAL symbol (['EXCEPT']). If depth has gone
 * sufficiently negative (the handler itself keeps erroring), halts the
 * runtime instead of recursing forever. */
rpl_thunk rpl_ded(rpl_runtime *rt, const char *reason);

/* The trampoline: repeatedly calls next.fn(rt, next.self) and replaces
 * next with the result, until rt->running is cleared. */
void rpl_rs(rpl_runtime *rt, rpl_thunk next);

/* Queues obj (a code object) as the next thing to run. Reuses the current
 * context slot (tail-call optimization) if the instruction about to run is
 * ret_internal; otherwise pushes a new context, unless the call-depth
 * budget is exhausted, in which case this reports the recursion error via
 * rpl_ded instead. Returns the eval thunk to resume with. */
rpl_thunk rpl_newcall(rpl_runtime *rt, rpl_obj *obj);

/* Like rpl_newcall, but also gives the new frame its own local namespace
 * (wrapping names in a fresh firstdir), for calls that bind locals. */
rpl_thunk rpl_newlocall(rpl_runtime *rt, rpl_obj *obj, rpl_obj *names);

/* ##################################################### */
/* Hierarchical named store                              */

/* Builds a new head node for a names chain: a directory whose tag is the
 * shared nulltag and whose next is obj, or rt->lastobj if obj is NULL.
 * Only one lastobj is needed globally, but every scope needs its own
 * distinct first entry (so sto() has somewhere to grow from). */
rpl_obj *rpl_firstdir(rpl_runtime *rt, rpl_obj *obj);

/* Looks up a dotted path (parts[0..n)) starting from the current context's
 * names, descending into nested directories one component at a time.
 * Returns the stored object, or NULL if any component is missing or a
 * non-directory is encountered before the path is exhausted. */
rpl_obj *rpl_rcl(rpl_runtime *rt, char *const *parts, int n);

/* Like rpl_rcl, but returns the RPL_TAG holding the final component
 * instead of unwrapping it to the tag's contained object -- lets a caller
 * mutate the binding in place (e.g. tag.obj = ...) rather than just read
 * its current value. */
rpl_obj *rpl_deref(rpl_runtime *rt, char *const *parts, int n);

/* Stores value at the dotted path parts[0..n), creating a new entry at the
 * end of the relevant directory's chain if the final component doesn't
 * exist yet. Fails (returns 0) if an intermediate component is missing --
 * sto() never creates intermediate directories, only leaf entries -- or if
 * a non-directory is encountered before the path is exhausted. */
int rpl_sto(rpl_runtime *rt, char *const *parts, int n, rpl_obj *value);

/* Removes the entry at the dotted path parts[0..n) by splicing it out of
 * its parent directory's chain. Returns 0 (and leaves the store untouched)
 * if any component along the path doesn't exist. */
int rpl_rm(rpl_runtime *rt, char *const *parts, int n);

/* Checks whether storing a symbol whose value chain starts at
 * parts[0..n) would circulate: follows Symbol -> Symbol chains via rcl,
 * failing (no circulation) as soon as a non-symbol or missing entry is
 * reached, and succeeding (circulation found) if any path in the chain
 * repeats. */
int rpl_circsym(rpl_runtime *rt, char *const *parts, int n);

/* Walks every entry already stored under dirtop (recursively, up to
 * RPL_CPDEPTH levels of subdirectory), checking each Symbol found via
 * rpl_circsym and recursing into each subdirectory found. Used before
 * completing a store that would introduce a new subdirectory, to make
 * sure nothing inside it already circulates back out. */
int rpl_circdir(rpl_runtime *rt, rpl_obj *dirtop);

#endif /* RPL_RUNTIME_H */
