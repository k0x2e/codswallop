#include "boot.h"
#include "internals.h"
#include "rom.h"
#include "types.h"

#include <stdio.h>
#include <string.h>

int rpl_boot(rpl_runtime *rt, const char *personality, const char *argstr) {
    rpl_register_types(rt);

    /* ourRT.sto([INTERNALSDIR], ourRT.firstdir(ourRT.lastobj)) */
    char *dirpath[1] = { "I*" };
    rpl_sto(rt, dirpath, 1, rpl_firstdir(rt, rt->lastobj));

    /* stoprocs(ourRT, INTERNALSDIR) */
    rpl_stoprocs(rt, "I*");

    /* ourRT.sto([INTERNALSDIR,'semicolon'], ourRT.Return)
     * ourRT.sto([INTERNALSDIR,'lastobj'],   ourRT.lastobj)
     * ourRT.sto([INTERNALSDIR,'nulltag'],   ourRT.nulltag) */
    char *semipath[2] = { "I*", "semicolon" };
    rpl_sto(rt, semipath, 2, rt->ret_internal);
    char *lastpath[2] = { "I*", "lastobj" };
    rpl_sto(rt, lastpath, 2, rt->lastobj);
    char *nullpath[2] = { "I*", "nulltag" };
    rpl_sto(rt, nullpath, 2, rt->nulltag);

    /* ourRT.Stack.push(typestr(argv[1])) / typestr("") */
    rpl_list_push(&rt->gc, rt->stack,
                  rpl_new_string(&rt->gc, argstr, strlen(argstr)));

    /* romboot(ourRT, f'personality/{personality}.rom') */
    char rompath[512];
    snprintf(rompath, sizeof(rompath), "personality/%s.rom", personality);
    return rpl_romboot(rt, rompath);
}
