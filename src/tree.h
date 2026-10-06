#ifndef ROC_TREE_H
#define ROC_TREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The shell's own files, carried inside the binary by the compiler: the
   scripts of /bin, the screens, the manuals, the art. It is a read-only
   filesystem, not a feature of any one layer — the shell reads /bin from
   it with no board in the build at all. */

enum {
    TREE_PATH_MAX = 64, /* the longest path inside the tree */
};

typedef struct {
    const char *path;
    const uint8_t *data;
    size_t len;
} tree_file;

extern const tree_file tree_files[];
extern const size_t tree_nfiles;

/* Every file of the tree the shell reads from now. */
const tree_file *tree_all(size_t *n);

/* A file by its path in the tree ("bin/help.filo"). */
bool tree_find_file(const char *path, const uint8_t **data, size_t *len);

/* Replaces the tree the shell reads from. The BBS never calls this: it
   carries its files inside the binary. The composing tool does, to read
   them from a directory and reload them as they are saved. NULL puts the
   built-in tree back. */
void tree_set_source(const tree_file *files, size_t n);

typedef struct roc roc;

/* Adds the tree to the VFS: bin/ is /bin, everything else /lib/roc. */
void tree_mount(roc *m);

/* A VFS path back to a tree path; false when it is not one of ours. */
bool tree_path_of_vfs(const char *vfs_path, char *out, size_t cap);

#endif
