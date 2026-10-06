#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../src/home.h"

/* The kept home is editable by hand and can arrive cut short: whatever the
   bytes, unpack must neither crash nor let a path out of the home. Every
   restored file is packed again, which exercises the other half. */

static ufs U;
static vfs V;
static uint8_t out[HOME_BLOB_CAP];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    ufs_init(&U);
    vfs_init(&V);
    (void)vfs_add_path(&V, "/home/guest", 0, true);
    size_t n = home_unpack(&U, &V, "/home/guest", data, size);
    size_t i = 0;
    while (i < U.nfiles) {
        if (strncmp(U.files[i].path, "/home/guest/", 12) != 0 ||
            strstr(U.files[i].path, "/../") != NULL) {
            __builtin_trap();
        }
        i++;
    }
    if (n < U.nfiles) { /* a path twice in the blob replaces: counted twice, stored once */
        __builtin_trap();
    }
    home_report rep;
    (void)home_pack(&U, "/home/guest", out, sizeof(out), &rep);
    return 0;
}
