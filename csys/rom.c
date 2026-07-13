/* Mirrors pysys/rom.py. See rom.h for the wire-format summary and the
 * notes on where this deliberately diverges from (or faithfully
 * reproduces an oddity in) the Python original. */

#include "rom.h"
#include "types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *xrealloc(void *p, size_t n) {
    void *r = realloc(p, n);
    if (n && !r) {
        fprintf(stderr, "rpl: out of memory\n");
        abort();
    }
    return r;
}

static char *xstrndup(const char *s, size_t n) {
    char *r = xrealloc(NULL, n + 1);
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

/* Same pop-returns-NULL-on-empty convention as internals.c's stack_pop
 * (mirrors typelst.pop()) -- including gc_hold()'ing the result so it
 * survives across further allocating calls made with it still in hand;
 * see internals.c's stack_pop and gc.h's file header for why. */
static rpl_obj *stack_pop(rpl_runtime *rt) {
    if (rt->stack->list.len == 0)
        return NULL;
    rpl_obj *o = rt->stack->list.data[--rt->stack->list.len];
    gc_hold(&rt->gc, o);
    return o;
}

static void stack_push(rpl_runtime *rt, rpl_obj *o) {
    rpl_list_push(&rt->gc, rt->stack, o);
}

/* ########################################################################
 * A small cursor over the (not necessarily nul-terminated -- RPL strings
 * can embed arbitrary bytes) ROM text, providing the `advance(delimiter)`
 * / `nextint()` helpers pysys/rom.py's loadrom() closes over. */
typedef struct {
    const char *text;
    size_t len;
    size_t cursor;
} romreader;

/* Returns 0 (leaving the output params untouched) if the delimiter isn't
 * found before the end of the buffer -- a malformed/truncated ROM. */
static int rr_advance(romreader *r, char delim, const char **out, size_t *outlen) {
    const void *p = memchr(r->text + r->cursor, delim, r->len - r->cursor);
    if (!p)
        return 0;
    *out = r->text + r->cursor;
    *outlen = (size_t)((const char *)p - (r->text + r->cursor));
    r->cursor = (size_t)((const char *)p - r->text) + 1;
    return 1;
}

static int rr_int_token(const char *tok, size_t toklen, long *out) {
    char buf[32];
    size_t n = toklen < sizeof(buf) - 1 ? toklen : sizeof(buf) - 1;
    memcpy(buf, tok, n);
    buf[n] = '\0';
    char *end;
    *out = strtol(buf, &end, 10);
    return end != buf;
}

/* nextint(): advance to the next space and parse the token as an int.
 * Returns 0 on a malformed stream (delimiter missing or non-numeric). */
static int rr_nextint(romreader *r, long *out) {
    const char *tok;
    size_t toklen;
    if (!rr_advance(r, ' ', &tok, &toklen))
        return 0;
    return rr_int_token(tok, toklen, out);
}

/* String/Comment/Symbol/Internal byte counts in the ROM format are
 * *character* counts in the sense pysys/rom.py means it: Python opens the
 * ROM file as text and slices `text[cursor:cursor+objdata]` on a `str`,
 * i.e. Unicode codepoints, not bytes. The C port reads the file as raw
 * bytes, so a multi-byte UTF-8 codepoint (e.g. the U+2026 "…" used in one
 * of the shipped ROMs) would otherwise desync every subsequent cursor
 * position by however many extra bytes it took. This walks `ncp` UTF-8
 * codepoints forward from s and returns how many *bytes* that consumed,
 * clamped to `avail` if the buffer is truncated or the encoding is
 * malformed (rather than reading out of bounds). */
static size_t utf8_codepoint_bytelen(const char *s, size_t avail, long ncp) {
    size_t pos = 0;
    for (long i = 0; i < ncp && pos < avail; i++) {
        unsigned char c = (unsigned char)s[pos];
        size_t seqlen;
        if ((c & 0x80) == 0x00)      seqlen = 1;
        else if ((c & 0xE0) == 0xC0) seqlen = 2;
        else if ((c & 0xF0) == 0xE0) seqlen = 3;
        else if ((c & 0xF8) == 0xF0) seqlen = 4;
        else                          seqlen = 1; /* invalid lead byte: take 1 byte and move on */
        if (pos + seqlen > avail)
            seqlen = avail - pos;
        pos += seqlen;
    }
    return pos;
}

/* ########################################################################
 * Header parsing: "<title>[<num>:<name>,<num>:<name>,...]CROM<count> ..."
 * translates each declared type number to our RPL_* enum via
 * rpl_type_by_name (see rom.h for why this isn't just assumed to match). */

#define ROM_MAX_HEADER_TYPES 64

static int parse_header(romreader *r, int headertypes[ROM_MAX_HEADER_TYPES]) {
    for (int i = 0; i < ROM_MAX_HEADER_TYPES; i++)
        headertypes[i] = -1;

    /* find '[' and print any title text before it */
    const void *lb = memchr(r->text + r->cursor, '[', r->len - r->cursor);
    if (!lb)
        return 0;
    size_t titlelen = (size_t)((const char *)lb - (r->text + r->cursor));
    if (titlelen)
        printf("%.*s", (int)titlelen, r->text + r->cursor);
    r->cursor = (size_t)((const char *)lb - r->text) + 1;

    for (;;) {
        const char *tok;
        size_t toklen;
        /* Each entry is "num:name", entries separated by ',', the whole
         * list terminated by ']'. We don't know in advance which
         * delimiter ends the current name, so scan for whichever of
         * ',' or ']' comes first. */
        const char *numtok;
        size_t numlen;
        if (!rr_advance(r, ':', &numtok, &numlen))
            return 0;
        long num;
        if (!rr_int_token(numtok, numlen, &num) || num < 0 || num >= ROM_MAX_HEADER_TYPES)
            return 0;

        const void *comma = memchr(r->text + r->cursor, ',', r->len - r->cursor);
        const void *bracket = memchr(r->text + r->cursor, ']', r->len - r->cursor);
        if (!bracket)
            return 0;
        const void *stop = comma && comma < bracket ? comma : bracket;
        tok = r->text + r->cursor;
        toklen = (size_t)((const char *)stop - tok);
        r->cursor = (size_t)((const char *)stop - r->text) + 1;

        char *name = xstrndup(tok, toklen);
        int rpltype = rpl_type_by_name(name);
        free(name);
        if (rpltype < 0)
            return 0;
        headertypes[num] = rpltype;

        if (stop == bracket)
            break;
    }
    return 1;
}

int rpl_loadrom(rpl_runtime *rt) {
    rpl_obj *textobj = stack_pop(rt);
    if (!textobj || (textobj->type != RPL_STRING && textobj->type != RPL_COMMENT)) {
        fprintf(stderr, "rpl_loadrom: expected a String on the stack\n");
        rpl_ded(rt, "This was a foolhardy proposal");
        return 0;
    }

    romreader r = { textobj->string.data, textobj->string.len, 0 };
    int headertypes[ROM_MAX_HEADER_TYPES];
    if (!parse_header(&r, headertypes)) {
        fprintf(stderr, "rpl_loadrom: malformed ROM header\n");
        rpl_ded(rt, "This was a foolhardy proposal");
        return 0;
    }

    /* Remainder must start with "CROM"; skip it and read the object count. */
    if (r.len - r.cursor < 4 || memcmp(r.text + r.cursor, "CROM", 4) != 0) {
        fprintf(stderr, "rpl_loadrom: missing CROM marker\n");
        rpl_ded(rt, "This was a foolhardy proposal");
        return 0;
    }
    r.cursor += 4;
    long count;
    if (!rr_nextint(&r, &count) || count < 0) {
        fprintf(stderr, "rpl_loadrom: bad object count\n");
        rpl_ded(rt, "This was a foolhardy proposal");
        return 0;
    }

    rpl_obj **store = xrealloc(NULL, sizeof(rpl_obj *) * (size_t)(count ? count : 1));
    memset(store, 0, sizeof(rpl_obj *) * (size_t)(count ? count : 1));

    int last_was_dispatch = 0;
    long last_binobj = -1;
    long i;

    for (i = 0; i < count; i++) {
        int objtype;
        long binobj = -1;
        int is_dispatch = 0;

        if (r.cursor >= r.len)
            goto fail;

        if (r.text[r.cursor] != '-') {
            const char *numtok;
            size_t numlen;
            if (!rr_advance(&r, ':', &numtok, &numlen))
                goto fail;
            long num;
            if (!rr_int_token(numtok, numlen, &num) || num < 0 || num >= ROM_MAX_HEADER_TYPES)
                goto fail;
            objtype = headertypes[num];
            if (objtype < 0)
                goto fail;
        } else {
            r.cursor++;
            is_dispatch = 1;
            objtype = -1;
            const char *tok;
            size_t toklen;
            if (!rr_advance(&r, ':', &tok, &toklen) || !rr_int_token(tok, toklen, &binobj))
                goto fail;
        }

        const char *rawtok;
        size_t rawlen;
        if (!rr_advance(&r, ' ', &rawtok, &rawlen))
            goto fail;
        char *rawstr = xstrndup(rawtok, rawlen);
        long objdata;
        int objdata_ok = rr_int_token(rawtok, rawlen, &objdata);

        if (is_dispatch) {
            if (!objdata_ok || binobj < 0 || binobj >= count || !store[binobj] ||
                store[binobj]->type != RPL_BUILTIN || objdata < 0 || objdata >= count || !store[objdata]) {
                free(rawstr);
                goto fail;
            }
            rpl_obj *bin = store[binobj];
            rpl_obj *table = store[objdata];
            free(bin->builtin.dispatches);
            free(bin->builtin.argck);
            bin->builtin.dispatches = NULL;
            bin->builtin.argck = NULL;
            bin->builtin.ndispatches = 0;
            for (int k = 0; k < table->list.len; k++) {
                rpl_obj *line = table->list.data[k];
                int *row = xrealloc(NULL, sizeof(int) * (size_t)bin->builtin.argct);
                for (int j = 0; j < bin->builtin.argct; j++)
                    row[j] = (int)line->list.data[j + 1]->integer;
                rpl_builtin_add_dispatch(&rt->gc, bin, row, line->list.data[0]);
                free(row);
            }
            free(rawstr);
            last_was_dispatch = 1;
            last_binobj = binobj;
            continue;
        }
        last_was_dispatch = 0;

        switch (objtype) {
            case RPL_INTEGER:
                if (!objdata_ok) { free(rawstr); goto fail; }
                store[i] = rpl_new_int(&rt->gc, objdata);
                break;
            case RPL_FLOAT:
                store[i] = rpl_new_float(&rt->gc, atof(rawstr));
                break;
            case RPL_STRING:
            case RPL_COMMENT: {
                if (!objdata_ok || objdata < 0) { free(rawstr); goto fail; }
                size_t nbytes = utf8_codepoint_bytelen(r.text + r.cursor, r.len - r.cursor, objdata);
                if (objtype == RPL_STRING)
                    store[i] = rpl_new_string(&rt->gc, r.text + r.cursor, nbytes);
                else
                    store[i] = rpl_new_comment(&rt->gc, r.text + r.cursor, nbytes);
                r.cursor += nbytes + 1;
                break;
            }
            case RPL_DIRECTORY: {
                if (!objdata_ok) { free(rawstr); goto fail; }
                if (objdata == i) {
                    store[i] = rt->lastobj;
                } else {
                    long tagidx;
                    if (!rr_nextint(&r, &tagidx) || tagidx < 0 || tagidx >= count || !store[tagidx] ||
                        objdata < 0 || objdata >= count || !store[objdata]) { free(rawstr); goto fail; }
                    store[i] = rpl_new_dir(&rt->gc, store[tagidx], store[objdata]);
                }
                break;
            }
            case RPL_SYMBOL: {
                if (!objdata_ok || objdata < 0) { free(rawstr); goto fail; }
                size_t symbytes = utf8_codepoint_bytelen(r.text + r.cursor, r.len - r.cursor, objdata);
                /* Manual '.'-split (avoiding strtok_r, which isn't in
                 * plain C11 without a feature-test macro): matches
                 * Python's `text[...].split('.')`. */
                char *nametext = xstrndup(r.text + r.cursor, symbytes);
                int cap = 4, n = 0;
                char **parts = xrealloc(NULL, sizeof(char *) * (size_t)cap);
                /* Note: even an empty name (objdata==0, nametext=="")
                 * yields exactly one part -- an empty string -- matching
                 * Python's `''.split('.') == ['']` (one element, not
                 * zero). The loop below handles that case naturally: the
                 * first iteration sees '\0' immediately and pushes the
                 * (empty) segment. */
                char *segstart = nametext;
                for (char *p = nametext;; p++) {
                    if (*p == '.' || *p == '\0') {
                        if (n == cap) { cap *= 2; parts = xrealloc(parts, sizeof(char *) * (size_t)cap); }
                        parts[n++] = segstart;
                        if (*p == '\0')
                            break;
                        *p = '\0';
                        segstart = p + 1;
                    }
                }
                store[i] = rpl_new_symbol(&rt->gc, parts, n);
                free(parts);
                free(nametext);
                r.cursor += symbytes + 1;
                break;
            }
            case RPL_INTERNAL: {
                if (!objdata_ok || objdata < 0) { free(rawstr); goto fail; }
                size_t namebytes = utf8_codepoint_bytelen(r.text + r.cursor, r.len - r.cursor, objdata);
                char *name = xstrndup(r.text + r.cursor, namebytes);
                char *path[2] = { "I*", name };
                store[i] = rpl_rcl(rt, path, 2);
                free(name);
                r.cursor += namebytes + 1;
                if (!store[i]) { free(rawstr); goto fail; }
                break;
            }
            case RPL_QUOTE:
                if (!objdata_ok || objdata < 0 || objdata >= count || !store[objdata]) { free(rawstr); goto fail; }
                store[i] = rpl_new_quote(&rt->gc, store[objdata]);
                break;
            case RPL_TAG: {
                if (!objdata_ok) { free(rawstr); goto fail; }
                if (objdata == -1) {
                    store[i] = rt->nulltag;
                } else {
                    long objidx;
                    if (objdata < 0 || objdata >= count || !store[objdata] || store[objdata]->type != RPL_SYMBOL ||
                        !rr_nextint(&r, &objidx) || objidx < 0 || objidx >= count || !store[objidx]) { free(rawstr); goto fail; }
                    store[i] = rpl_new_tag(&rt->gc, store[objdata]->symbol.parts[0], store[objidx]);
                }
                break;
            }
            case RPL_LIST:
            case RPL_CODE: {
                if (!objdata_ok || objdata < 0) { free(rawstr); goto fail; }
                rpl_obj *lst = rpl_new_list(&rt->gc, objtype);
                for (long j = 0; j < objdata; j++) {
                    long idx;
                    if (!rr_nextint(&r, &idx) || idx < 0 || idx >= count || !store[idx]) { free(rawstr); goto fail; }
                    rpl_list_push(&rt->gc, lst, store[idx]);
                }
                if (objtype == RPL_CODE)
                    rpl_list_push(&rt->gc, lst, rt->ret_internal);
                store[i] = lst;
                break;
            }
            case RPL_CONTEXT: {
                if (!objdata_ok || objdata < 0 || objdata >= count || !store[objdata]) { free(rawstr); goto fail; }
                long namesidx;
                if (!rr_nextint(&r, &namesidx) || namesidx < 0 || namesidx >= count || !store[namesidx]) {
                    free(rawstr); goto fail;
                }
                /* Mirrors Python's `store[i] = typecontext(store[objdata],
                 * store[nextint()])` running *before* next/depth/ip are
                 * read: the base (bottom-of-call-stack) context's `next`
                 * field is a self-reference by store index, which only
                 * resolves if store[i] already exists by the time we look
                 * it up -- so we must publish store[i] here, before
                 * reading depthidx/ipidx/nextidx below. */
                rpl_obj *ctx = rpl_new_context(&rt->gc, store[objdata], store[namesidx], NULL);
                store[i] = ctx;

                long depthidx, ipidx, nextidx;
                if (!rr_nextint(&r, &depthidx) || depthidx < 0 || depthidx >= count || !store[depthidx] ||
                    !rr_nextint(&r, &ipidx) || ipidx < 0 || ipidx >= count || !store[ipidx] ||
                    !rr_nextint(&r, &nextidx) || nextidx < 0 || nextidx >= count || !store[nextidx]) {
                    free(rawstr); goto fail;
                }
                ctx->context.depth = RPL_CALLDEPTH - (int)store[depthidx]->integer;
                ctx->context.ip = (int)store[ipidx]->integer;
                ctx->context.next = store[nextidx];
                break;
            }
            case RPL_BUILTIN: {
                if (!objdata_ok || objdata < 0 || objdata >= count || !store[objdata] || store[objdata]->type != RPL_SYMBOL) { free(rawstr); goto fail; }
                long hintidx, argctidx;
                if (!rr_nextint(&r, &hintidx) || hintidx < 0 || hintidx >= count || !store[hintidx] ||
                    !rr_nextint(&r, &argctidx) || argctidx < 0 || argctidx >= count || !store[argctidx]) {
                    free(rawstr); goto fail;
                }
                store[i] = rpl_new_builtin(&rt->gc, store[objdata]->symbol.parts[0],
                                           store[hintidx]->string.data, (int)store[argctidx]->integer);
                break;
            }
            default:
                free(rawstr);
                goto fail;
        }
        free(rawstr);
    }

    /* Trailing text after the last object (mirrors `print(text[cursor:])`). */
    if (r.cursor < r.len)
        printf("%.*s", (int)(r.len - r.cursor), r.text + r.cursor);

    if (last_was_dispatch)
        stack_push(rt, store[last_binobj]);
    else
        stack_push(rt, store[count - 1]);
    free(store);
    return 1;

fail:
    fprintf(stderr, "rpl_loadrom: malformed ROM at object %ld (cursor %zu)\n", i, r.cursor);
    free(store);
    rpl_ded(rt, "This was a foolhardy proposal");
    return 0;
}

rpl_thunk rpl_evalrom(rpl_runtime *rt, rpl_obj *self) {
    (void)self;
    if (!rpl_loadrom(rt))
        return rpl_obj_eval(rt, rt->dedsym);
    return rpl_obj_eval(rt, stack_pop(rt));
}

int rpl_romboot(rpl_runtime *rt, const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "rpl_romboot: cannot open %s\n", filename);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xrealloc(NULL, (size_t)(size > 0 ? size : 1));
    size_t got = size > 0 ? fread(buf, 1, (size_t)size, f) : 0;
    fclose(f);

    /* Mirrors `ints=rt.rcl(['I*'])` / `rt.sto(['i**'],ints)` -- a
     * faithfully-reproduced oddity, not a typo fixed here; see rom.h. */
    char *ipath[1] = { "I*" };
    rpl_obj *ints = rpl_rcl(rt, ipath, 1);

    stack_push(rt, rpl_new_string(&rt->gc, buf, got));
    free(buf);

    if (!rpl_loadrom(rt))
        return 0;

    rpl_obj *result = stack_pop(rt);
    if (!result || result->type != RPL_CONTEXT) {
        fprintf(stderr, "rpl_romboot: ROM's final object is not a Context\n");
        return 0;
    }
    rt->context = result;

    if (ints) {
        char *lowpath[1] = { "i**" };
        rpl_sto(rt, lowpath, 1, ints);
    }
    return 1;
}
