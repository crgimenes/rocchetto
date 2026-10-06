/* A build whose host offers its programs nothing past roc's. */
#include "roc.h"

bool roc_host_extend(void *ctx, filo_ctx *f) {
    (void)ctx;
    (void)f;
    return true;
}
