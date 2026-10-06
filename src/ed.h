#ifndef ROC_ED_H
#define ROC_ED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tbuf.h"
#include "vfs.h"

/* The line editor, in the shape ed has had since 1969: addresses, one
   letter, and text that ends with a lone dot. It exists for the two moments
   a full-screen editor cannot help — a build with no screens at all, and a
   session where something was edited badly enough that the shell will not
   come up. Being line-oriented it needs no canvas, no cursor addressing and
   no screen system, so it works over a serial line, on a terminal that lies
   about its size, and in a log. */

typedef struct roc roc;

typedef struct {
    bool on;
    /* a, i and c read text until a lone "." — while this is set every line
       typed is content, not a command */
    bool inserting;
    size_t insert_at; /* line the text goes before */
    size_t dot;       /* the current line, counted from zero */
    char path[VFS_PATH_MAX];
    tbuf tb;
} ed_state;

/* Opens path, or an empty buffer when it is "". Prints the byte count, as
   ed does. */
void ed_begin(roc *m, const char *path);

/* True while it owns the typed line. */
bool ed_active(const roc *m);

/* One line: a command, or text when inserting. The line arrives untrimmed. */
void ed_line(roc *m, char *line);

/* What to draw before the line, or NULL when ed is not up. */
const char *ed_prompt(const roc *m);

#endif
