#ifndef ROC_EDIT_H
#define ROC_EDIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "filo.h"
#include "tbuf.h"
#include "vfs.h"

/* The text an app works on: it lives in C (tbuf), the app's face is a
   Filo program (/bin/edt.fbb, the editor), and these builtins are the
   bridge, registered in every screen's context. */
typedef struct {
    tbuf tb;
    char path[VFS_PATH_MAX];     /* where a save goes; "" for a new file */
    char ret[32];                /* the screen to load on closing; "" leaves the shell */
    char app[32];                /* the app over the text: "edt" */
    char app_file[VFS_PATH_MAX]; /* the program it is: "/bin/edt.fbb", "~/mine.fbb" */
} edit_state;

typedef struct roc roc;

void edit_register(filo_ctx *ctx);

/* Loads the file at arg (relative to cwd, "~" the home; a new file when
   there is none yet) and enters the editor: edt, after DEC's, whose GOLD
   key is our Esc. Refusals are printed. */
bool edit_open(roc *m, const char *arg);
/* The same from a screen, which the editor loads again when it closes:
   the screen keeps no state of its own, so it comes back as it was. */
bool edit_open_from(roc *m, const char *arg, const char *ret);
/* The same for any app: the program in file (a .fbb, named app), with the
   file at arg as the text it works on. */
bool edit_open_app(roc *m, const char *app, const char *file, const char *arg, const char *ret);
/* The same with the bytes in hand: a site file the host just sent. */
void edit_open_bytes(roc *m, const char *path, const uint8_t *data, size_t len);

#endif
