#ifndef ROC_UFS_H
#define ROC_UFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "roc_config.h"

#include "vfs.h"

/* The user's own files: what was dropped on the page or picked with
   upload. They live in this memory and nowhere else — nothing leaves the
   machine — so the store is bounded like everything here. One arena, the
   files packed in order; removing one slides the rest down. */
enum {
    UFS_DATA_CAP = ROC_CFG_UFS_DATA_CAP,
    UFS_FILE_MAX = ROC_CFG_UFS_FILE_MAX,
    UFS_FILES_MAX = ROC_CFG_UFS_FILES_MAX,
};

typedef struct {
    char path[VFS_PATH_MAX];
    size_t off;
    size_t len;
    bool has_mtime; /* a host with no clock wrote it: false, not 1970 */
    int64_t mtime;  /* seconds since 1970, UTC */
} ufs_file;

typedef struct {
    uint8_t data[UFS_DATA_CAP];
    size_t used;
    ufs_file files[UFS_FILES_MAX];
    size_t nfiles;
    /* the one transfer in flight: its slot is the last, its bytes arrive
       at the end of the arena, and until ufs_end it is not a file yet */
    bool open;
    size_t expect;
} ufs;

typedef enum {
    UFS_OK,
    UFS_TOO_LARGE, /* one file over UFS_FILE_MAX */
    UFS_NO_ROOM,   /* the arena is full */
    UFS_TOO_MANY,  /* UFS_FILES_MAX reached */
    UFS_BUSY,      /* a transfer is already open */
} ufs_status;

void ufs_init(ufs *u);

/* Reserves room for size bytes at path; a file already there is replaced
   when the transfer ends, gone at once. */
ufs_status ufs_begin(ufs *u, const char *path, size_t size);

/* Bytes of the open transfer; false once past the size announced. */
bool ufs_data(ufs *u, const uint8_t *data, size_t n);

/* Closes the transfer as a file of the bytes received. */
void ufs_end(ufs *u);
void ufs_abort(ufs *u);

bool ufs_find(const ufs *u, const char *path, const uint8_t **data, size_t *len);
/* The file at path as the store keeps it; NULL when it is not there. */
const ufs_file *ufs_entry(const ufs *u, const char *path);
bool ufs_set_mtime(ufs *u, const char *path, int64_t mtime);
bool ufs_remove(ufs *u, const char *path);
/* from becomes to, a file at to replaced: how a file written beside its
   name takes its place. False when from is not there or to is too long. */
bool ufs_rename(ufs *u, const char *from, const char *to);
/* The bytes the open transfer may still take: what its reservation has
   left. */
size_t ufs_room(const ufs *u);

#endif
