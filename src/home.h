#ifndef ROC_HOME_H
#define ROC_HOME_H

#include <stddef.h>
#include <stdint.h>

#include "roc_config.h"

#include "ufs.h"
#include "vfs.h"

/* The user's home as one blob, so a host can keep it between visits (the
   browser's localStorage, a file). Text, a header with a version, then
   one record per directory and per file:

       msh-home 2\n
       d\t<path>\n
       f\t<path>\t<len>\t<mtime>\n
       <len bytes>\n

   mtime is seconds since 1970 (UTC), negative before it, or "-" when the
   file has none. Paths
   are relative to the home, so the blob does not care what the home is
   called today. The newline after the bytes is a separator, not part of
   the file. Version 1 (files only: <path>\t<len>\n<bytes>\n) is still
   read. What persists is what fits: HOME_FILE_MAX per file, HOME_BLOB_CAP
   in all, directories first, then files in the order the store has them
   (creation order); one that does not fit is skipped, the rest still go. */
enum {
    HOME_BLOB_CAP = ROC_CFG_HOME_BLOB_CAP,
    HOME_FILE_MAX = 16 * 1024,
    HOME_VERSION = 2,
};

typedef struct {
    size_t files;   /* file records written */
    size_t dirs;    /* directory records written */
    size_t skipped; /* too large, or no room left */
    char first_skipped[VFS_PATH_MAX];
} home_report;

/* Writes the blob; returns its length (always fits cap when cap >=
   HOME_BLOB_CAP). */
size_t home_pack(const ufs *u, const vfs *v, const char *home, uint8_t *buf, size_t cap,
                 home_report *rep);

/* Reads a blob into the store and the index, under home. A trust
   boundary: it is editable by hand and can arrive truncated, so a record
   that does not add up, or a path that escapes the home, is dropped and
   the rest still goes. Returns the files restored; dirs (may be NULL) gets
   the directories. */
size_t home_unpack(ufs *u, vfs *v, const char *home, const uint8_t *blob, size_t len, size_t *dirs);

#endif
