#include "tree.h"

#include <string.h>

#include "roc.h"

/* NULL means the tree built into the binary. */
static const tree_file *source_files = NULL;
static size_t source_nfiles = 0;

void tree_set_source(const tree_file *files, size_t n) {
    source_files = files;
    source_nfiles = n;
}

const tree_file *tree_all(size_t *n) {
    if (source_files != NULL) {
        *n = source_nfiles;
        return source_files;
    }
    *n = tree_nfiles;
    return tree_files;
}

bool tree_find_file(const char *path, const uint8_t **data, size_t *len) {
    size_t n = 0;
    const tree_file *files = tree_all(&n);
    size_t i = 0;
    while (i < n) {
        if (strcmp(files[i].path, path) == 0) {
            *data = files[i].data;
            *len = files[i].len;
            return true;
        }
        i++;
    }
    return false;
}

static bool vfs_path_of(const char *tree_path, char *out, size_t cap) {
    /* bin/ is /bin, the programs; the rest is the shell's own, under
       /lib/roc */
    const char *prefix = "/lib/roc/";
    if (strncmp(tree_path, "bin/", 4) == 0) {
        prefix = "/";
    }
    size_t n = strlen(prefix) + strlen(tree_path);
    if (n >= cap) {
        return false;
    }
    memcpy(out, prefix, strlen(prefix));
    memcpy(out + strlen(prefix), tree_path, strlen(tree_path) + 1);
    return true;
}

void tree_mount(roc *m) {
    size_t n = 0;
    const tree_file *files = tree_all(&n);
    (void)vfs_add(&m->fs, "/lib", 0, true);
    (void)vfs_add(&m->fs, "/lib/roc", 0, true);
    /* the user's home, with the one file the shell writes there for now */
    char home[VFS_PATH_MAX];
    (void)vfs_add(&m->fs, "/home", 0, true);
    if (roc_home_path(m, "", home, sizeof(home))) {
        (void)vfs_add(&m->fs, home, 0, true);
    }
    if (roc_home_path(m, ".history", home, sizeof(home))) {
        (void)vfs_add(&m->fs, home, 0, false);
    }
    size_t i = 0;
    while (i < n) {
        char path[VFS_PATH_MAX];
        if (vfs_path_of(files[i].path, path, sizeof(path))) {
            (void)vfs_add_path(&m->fs, path, (uint32_t)files[i].len, false);
        }
        i++;
    }
}

bool tree_path_of_vfs(const char *vfs_path, char *out, size_t cap) {
    const char *rest = NULL;
    if (strncmp(vfs_path, "/bin/", 5) == 0) {
        rest = vfs_path + 1; /* keeps "bin/" */
    } else if (strncmp(vfs_path, "/lib/roc/", 9) == 0) {
        rest = vfs_path + 9;
    }
    if (rest == NULL || strlen(rest) >= cap) {
        return false;
    }
    memcpy(out, rest, strlen(rest) + 1);
    return true;
}
