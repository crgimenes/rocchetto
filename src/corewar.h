#ifndef ROC_COREWAR_H
#define ROC_COREWAR_H

#include <stdbool.h>

#include "arena.h"
#include "filo.h"

typedef struct roc roc;

/* Core War on the shell: the corewar repository's MARS, arena and pick
   (/bin/corewar), with the shell's files, keys and screens around them.
   `corewar` with no warriors opens the pick; with them, the arena; with -b
   (or a redirect), the score as text. `mars` is the same command, kept
   for the habit of those who type it. */

/* corewar [a.red b.red ...] [-r rounds] [-F pos] [-b] */
void roc_cmd_corewar(roc *m, const char *rest);
void roc_cmd_mars(roc *m, const char *rest);

/* The pick's fight, over the pick: "" when it opened, else why. */
const char *corewar_fight(roc *m);

/* The pick's builtins: the arena's list and the shell's cw-library,
   cw-fight, cw-edit and cw-manual. */
void corewar_register(filo_ctx *ctx);

#endif
