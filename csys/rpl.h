#ifndef RPL_H
#define RPL_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* Mirrors pysys/trivia.py */
#define RPL_CALLDEPTH 2048
#define RPL_CPDEPTH   64

/* Type numbers, in the same order pysys/rtypes.py:baseregistry() registers
 * them (Any is always 0; registration order determines parse priority,
 * which matters once the parser is ported in Phase 4). */
enum rpl_type {
    RPL_ANY       = 0,
    RPL_CONTEXT   = 1,
    RPL_INTERNAL  = 2,
    RPL_SYMBOL    = 3,
    RPL_FLOAT     = 4,
    RPL_STRING    = 5,
    RPL_COMMENT   = 6,
    RPL_BUILTIN   = 7,
    RPL_DIRECTORY = 8,
    RPL_TAG       = 9,
    RPL_LIST      = 10,
    RPL_CODE      = 11,
    RPL_INTEGER   = 12,
    RPL_HANDLE    = 13,
    RPL_QUOTE     = 14,
    RPL_NTYPES
};

typedef struct rpl_obj rpl_obj;
typedef struct rpl_runtime rpl_runtime;

/* The trampoline unit: a (fn, self) pair covering what Python expresses
 * as a bound eval method. */
typedef struct rpl_thunk {
    struct rpl_thunk (*fn)(rpl_runtime *, rpl_obj *);
    rpl_obj *self;
} rpl_thunk;

typedef rpl_thunk (*rpl_eval_fn)(rpl_runtime *, rpl_obj *);

struct rpl_obj {
    rpl_obj  *alloc_next;  /* intrusive GC heap list; not visible to RPL */
    uint16_t  type;        /* type number, matches rpltypes registry */
    uint16_t  flags;       /* GC mark bit (bit 0) + reserved */

    union {
        int64_t integer;
        double  floating;

        struct { char *data; size_t len; }          string;   /* String, Comment */
        struct { char **parts; int nparts; }        symbol;   /* Symbol (dotted path) */
        struct { rpl_obj **data; int len; int cap; } list;    /* List, Code */
        struct { rpl_obj *tag; rpl_obj *next; }      dir;      /* Directory */
        struct { char *name; rpl_obj *obj; }         tag;      /* Tag */
        struct { rpl_obj *inner; }                   quote;    /* Quote */
        FILE *                                       handle;  /* Handle */

        struct {
            char        *name;
            char        *hint;
            int          argct;
            int         *argck;       /* ndispatches * argct type numbers */
            rpl_obj    **dispatches;  /* ndispatches handler objects */
            int          ndispatches;
        } builtin;

        struct { char *name; rpl_eval_fn fn; } binproc;

        struct {
            rpl_obj *code;
            rpl_obj *names;
            rpl_obj *next;
            int      ip;
            int      depth;
        } context;
    };
};

#define RPL_MARK_BIT     0x1u
#define rpl_marked(o)    (((o)->flags & RPL_MARK_BIT) != 0)
#define rpl_set_mark(o)  ((o)->flags |= RPL_MARK_BIT)
#define rpl_clear_mark(o) ((o)->flags &= ~RPL_MARK_BIT)

#endif /* RPL_H */
