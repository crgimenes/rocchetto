#ifndef ROC_FILEH_H
#define ROC_FILEH_H

/* Files a script reads and writes a piece at a time (file-open and the
   rest), on the host's storage or in memory, so a utility walks a file
   larger than any buffer without holding it whole. */

#include <stdbool.h>

#include "script.h"
#include "script_data.h"

extern const script_builtin script_file_builtins[];
extern const size_t script_file_count;

/* What run r left open, closed: its writes kept when it ended well
   (commit), dropped otherwise, each old file as it was. */
void script_files_close(struct roc *m, script_exec *r, bool commit);

#endif
