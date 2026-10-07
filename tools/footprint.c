/* What a build costs in RAM, by part. Links the whole core, so it also
   proves the configuration it was built with compiles and fits together.
   usage: footprint [-h]  (it takes no arguments; output is a table) */

#include <stdio.h>
#include <string.h>

#include "roc.h"

typedef struct {
    const char *name;
    size_t bytes;
} part;

int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("usage: footprint\n"
               "Prints how many bytes each part of the session state takes in\n"
               "this build, largest first, and the total. Build it with -D\n"
               "overrides from roc_config.h to measure another target:\n"
               "  make footprint   the defaults (web and desktop)\n"
               "  make small       the board-sized configuration\n");
        return 0;
    }

    const part parts[] = {
        {"ufs (user's files)", sizeof(ufs)},
        {"script (user's Filo)", sizeof(script_state)},
        {"pager (article being read)", sizeof(pager)},
        {"vfs (site index)", sizeof(vfs)},
#if ROC_APP_SCREENS
        {"screen (screens' Filo)", sizeof(screen_state)},
#endif
#if ROC_APP_EDIT
        {"edit (text buffer)", sizeof(edit_state)},
#endif
        {"compositor", sizeof(compositor)},
        {"term (output)", sizeof(term)},
        {"capture (redirect)", sizeof(roc_capture)},
        {"home blob", HOME_BLOB_CAP},
        {"history", (size_t)ROC_HIST_MAX * ROC_LINE_MAX},
    };
    size_t n = sizeof(parts) / sizeof(parts[0]);

    /* largest first: the table is read to decide what to cut next */
    size_t order[sizeof(parts) / sizeof(parts[0])];
    size_t i = 0;
    while (i < n) {
        order[i] = i;
        i++;
    }
    i = 0;
    while (i < n) {
        size_t j = i + 1;
        while (j < n) {
            if (parts[order[j]].bytes > parts[order[i]].bytes) {
                size_t t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
            j++;
        }
        i++;
    }

    printf("terminal ceiling  %dx%d\n", TERM_COLS_MAX, TERM_ROWS_MAX);
    printf("apps              screens %d, edit %d, corewar %d, tools %d\n\n", ROC_APP_SCREENS,
           ROC_APP_EDIT, ROC_APP_COREWAR, ROC_APP_TOOLS);
    printf("the session (one visitor's roc):\n");
    size_t named = 0;
    i = 0;
    while (i < n) {
        const part *p = &parts[order[i]];
        printf("  %-26s %9zu\n", p->name, p->bytes);
        named += p->bytes;
        i++;
    }
    /* signed on purpose: a part counted twice — an editor that carries its
       own text buffer, say — shows up as a negative rest instead of as a
       gigantic one */
    printf("  %-26s %9lld\n", "(the rest)", (long long)sizeof(roc) - (long long)named);
    printf("  %-26s %9zu\n", "sizeof(roc)", sizeof(roc));

    /* Not every byte of a build lives in the session. The Core War arena
       keeps its core and its assembler in module statics — one fight at a
       time, far too big for a stack — and they cost the same RAM. The
       Makefile prints the linker's own figure after this table, which is
       the number that counts them all. */
    size_t outside = 0;
#if ROC_APP_COREWAR
    outside = sizeof(mars) + sizeof(mars_warrior) * CW_FIGHTERS_MAX * 2;
#endif
    printf("\noutside the session (module statics):\n");
    printf("  %-26s %9zu\n", "corewar arena + warriors", outside);
    printf("\n  %-26s %9zu  (%.2f MiB)\n", "TOTAL", sizeof(roc) + outside,
           (double)(sizeof(roc) + outside) / (1024.0 * 1024.0));
    return 0;
}
