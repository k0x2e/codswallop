#ifndef RPL_OBJ_H
#define RPL_OBJ_H

#include "rpl.h"
#include "gc.h"

/* Constructors. Each returns a freshly gc_alloc'd object with its payload
 * filled in; none of them push a GC root themselves, so callers building
 * up a structure across multiple allocations must root intermediate
 * results with gc_root_push/gc_root_pop. */

rpl_obj *rpl_new_int(rpl_gc *gc, int64_t v);
rpl_obj *rpl_new_float(rpl_gc *gc, double v);

/* Copies [data, data+len) into owned storage. */
rpl_obj *rpl_new_string(rpl_gc *gc, const char *data, size_t len);
rpl_obj *rpl_new_comment(rpl_gc *gc, const char *data, size_t len);

/* Copies the parts array (and each string within it) into owned storage. */
rpl_obj *rpl_new_symbol(rpl_gc *gc, char **parts, int nparts);

rpl_obj *rpl_new_quote(rpl_gc *gc, rpl_obj *inner);
rpl_obj *rpl_new_handle(rpl_gc *gc, FILE *f);

/* type must be RPL_LIST or RPL_CODE. Starts empty; use rpl_list_push. */
rpl_obj *rpl_new_list(rpl_gc *gc, uint16_t type);
void     rpl_list_push(rpl_gc *gc, rpl_obj *list, rpl_obj *item);

rpl_obj *rpl_new_tag(rpl_gc *gc, const char *name, rpl_obj *obj);

/* If next is NULL, the new directory is self-referential (the lastobj
 * sentinel pattern); otherwise next is used as-is. */
rpl_obj *rpl_new_dir(rpl_gc *gc, rpl_obj *tag, rpl_obj *next);

/* Mirrors typecontext.__init__: if next is NULL the context is its own
 * next (call-stack bottom) and depth starts at RPL_CALLDEPTH; otherwise
 * depth = next->context.depth - 1. */
rpl_obj *rpl_new_context(rpl_gc *gc, rpl_obj *code, rpl_obj *names, rpl_obj *next);

rpl_obj *rpl_new_binproc(rpl_gc *gc, const char *name, rpl_eval_fn fn);

rpl_obj *rpl_new_builtin(rpl_gc *gc, const char *name, const char *hint, int argct);
/* types must point to argct type numbers (0 == Any); copied in. */
void     rpl_builtin_add_dispatch(rpl_gc *gc, rpl_obj *bin, const int *types, rpl_obj *handler);

/* Duplicate an object per Python objarchetype.cp() semantics:
 *   - most types are immutable and return themselves
 *   - List/Code: shallow copy (new array, same element pointers)
 *   - Tag: new tag, same name and same contained object
 *   - Builtin: new builtin with copied argck/dispatches arrays
 *   - Directory: recursive copy bounded by RPL_CPDEPTH, recursing only
 *     into tag values that are themselves directories (subdirectories)
 */
rpl_obj *rpl_cp(rpl_gc *gc, rpl_obj *o);

#endif /* RPL_OBJ_H */
