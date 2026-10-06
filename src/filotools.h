/* filo's tools on the shell's files, as the desktop's filo has them: build,
   bundle, dump, show, check, size and decompile, over the tree and the
   home. The listing, the report and the decompiler are clang_filo's
   (fbc_dump.c, fbc_decompile.c), so what they write here is what they
   write on the desktop. */
#ifndef FILOTOOLS_H
#define FILOTOOLS_H

#include <stdbool.h>

typedef struct roc roc;

/* Does "filo arg" when arg starts with one of the tools (or -h): true when
   it did, errors said; false when arg is a file to run, for the caller. */
bool roc_filo_tool(roc *m, const char *arg);

#endif
