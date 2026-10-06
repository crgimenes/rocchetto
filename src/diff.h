/* diff: two files line by line, as POSIX diff shows them, in the normal
   format (2c2, < and > lines, ---) or with -u the unified one, three lines
   of context. - is the input a | hands over. $? is 0 when they are the
   same, 1 when they differ, 2 when they cannot be compared. */
#ifndef ROC_DIFF_H
#define ROC_DIFF_H

typedef struct roc roc;

/* rest: the words after "diff", as sh_split reads them. */
void roc_cmd_diff(roc *m, const char *rest);

#endif
