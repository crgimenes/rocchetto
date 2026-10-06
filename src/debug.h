/* filo debug on the shell: an entry point of a unit stepped by a person,
   as the desktop's filo debug steps it (cmd/filo in the Go repository), in
   the edt's colours: the source on the left, the unit's instructions or its
   bytes on the right, the calls below. The run is the shell's own VM held
   between two instructions (filo_bc_begin, filo_bc_frames); going back is
   starting again and running to where the last movement began, since the
   machine is deterministic. */
#ifndef ROC_DEBUG_H
#define ROC_DEBUG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct roc roc;

/* Opens the debugger on the unit (len bytes, copied) at the entry named
   entry, or when entry is NULL at "main", else the first. The source of an
   entry NAME is dir/NAME.filo. False, said, when it cannot: label names
   the file in what it says. */
bool debug_enter(roc *m, const uint8_t *unit, size_t len, const char *entry, const char *dir,
                 const char *label);

#endif
