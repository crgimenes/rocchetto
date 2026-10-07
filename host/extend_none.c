/* A build with nothing past roc's: no builtins for its programs, and no
   layer (front screen, doors, commands of its own). */
#include "roc.h"

bool roc_host_extend(void *ctx, filo_ctx *f) {
    (void)ctx;
    (void)f;
    return true;
}

const roc_layer roc_layer_spec = {0};
