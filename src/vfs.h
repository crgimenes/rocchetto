#ifndef ROC_VFS_H
#define ROC_VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "roc_config.h"

enum {
    VFS_ARENA_CAP = ROC_CFG_VFS_ARENA_CAP,
    VFS_NODES_MAX = ROC_CFG_VFS_NODES_MAX,
    VFS_PATH_MAX = 256,
};

/* One entry of the index. Strings point into the arena. Directory paths are
   stored without the trailing slash (root is "/"). */
typedef struct {
    const char *path;
    const char *date;  /* "YYYY-MM-DD HH:MM" or "-" */
    const char *title; /* may be "" */
    uint32_t size;
    bool dir;
} vfs_node;

typedef struct {
    char arena[VFS_ARENA_CAP];
    size_t arena_len;
    vfs_node nodes[VFS_NODES_MAX];
    size_t nnodes;
} vfs;

void vfs_init(vfs *v);

/* Accumulates raw index bytes (TSV: path \t size \t date \t title \n). */
bool vfs_append(vfs *v, const uint8_t *data, size_t n);

/* Parses the accumulated arena in place. Malformed lines are skipped.
   Returns the number of nodes. */
size_t vfs_parse(vfs *v);

const vfs_node *vfs_lookup(const vfs *v, const char *path);

/* True when node's parent directory is exactly dir ("/" included). */
bool vfs_is_child(const vfs_node *n, const char *dir);

/* Adds an entry that did not come from the index — the shell's own files,
   the user's. The path is copied into the arena; a path already there only
   has its size updated. */
bool vfs_add(vfs *v, const char *path, uint32_t size, bool dir);

/* vfs_add with every directory on the way added first. */
bool vfs_add_path(vfs *v, const char *path, uint32_t size, bool dir);

/* True when anything lives directly under dir. */
bool vfs_has_children(const vfs *v, const char *dir);

/* Drops an entry; false when there is none. The path's bytes stay in the
   arena: a removal is rare and the arena is a megabyte. */
bool vfs_remove(vfs *v, const char *path);

/* The name after the last slash. */
const char *vfs_base(const vfs_node *n);

/* Dot files are hidden: ls, tree and Tab skip them unless asked. */
bool vfs_hidden(const vfs_node *n);

/* Resolves arg against cwd into dst: handles absolute paths, ".", "..",
   repeated slashes; never escapes above "/". Returns false on overflow. */
bool vfs_resolve(const char *cwd, const char *arg, char *dst, size_t cap);

#endif
