#include "ufs.h"

#include <string.h>

void ufs_init(ufs *u) {
    u->used = 0;
    u->nfiles = 0;
    u->open = false;
    u->expect = 0;
}

static size_t find_slot(const ufs *u, const char *path) {
    size_t i = 0;
    while (i < u->nfiles) {
        if (strcmp(u->files[i].path, path) == 0) {
            return i;
        }
        i++;
    }
    return u->nfiles;
}

bool ufs_remove(ufs *u, const char *path) {
    size_t i = find_slot(u, path);
    if (i == u->nfiles) {
        return false;
    }
    /* copies of the slot's numbers: the shift below writes over it */
    size_t off = u->files[i].off;
    size_t len = u->files[i].len;
    memmove(u->data + off, u->data + off + len, u->used - (off + len));
    u->used -= len;
    size_t k = i + 1;
    while (k < u->nfiles) {
        u->files[k].off -= len;
        u->files[k - 1] = u->files[k];
        k++;
    }
    u->nfiles--;
    return true;
}

ufs_status ufs_begin(ufs *u, const char *path, size_t size) {
    if (u->open) {
        return UFS_BUSY;
    }
    if (size > UFS_FILE_MAX) {
        return UFS_TOO_LARGE;
    }
    (void)ufs_remove(u, path);
    if (u->nfiles >= UFS_FILES_MAX) {
        return UFS_TOO_MANY;
    }
    if (size > UFS_DATA_CAP - u->used) {
        return UFS_NO_ROOM;
    }
    ufs_file *f = &u->files[u->nfiles];
    memcpy(f->path, path, strlen(path) + 1);
    f->off = u->used;
    f->len = 0;
    f->has_mtime = false;
    f->mtime = 0;
    u->open = true;
    u->expect = size;
    return UFS_OK;
}

bool ufs_data(ufs *u, const uint8_t *data, size_t n) {
    if (!u->open) {
        return false;
    }
    ufs_file *f = &u->files[u->nfiles];
    if (n > u->expect - f->len) {
        return false;
    }
    memcpy(u->data + f->off + f->len, data, n);
    f->len += n;
    return true;
}

void ufs_end(ufs *u) {
    if (!u->open) {
        return;
    }
    u->used += u->files[u->nfiles].len;
    u->nfiles++;
    u->open = false;
}

void ufs_abort(ufs *u) {
    u->open = false;
}

bool ufs_find(const ufs *u, const char *path, const uint8_t **data, size_t *len) {
    size_t i = find_slot(u, path);
    if (i == u->nfiles) {
        return false;
    }
    *data = u->data + u->files[i].off;
    *len = u->files[i].len;
    return true;
}

const ufs_file *ufs_entry(const ufs *u, const char *path) {
    size_t i = find_slot(u, path);
    return i == u->nfiles ? NULL : &u->files[i];
}

bool ufs_set_mtime(ufs *u, const char *path, int64_t mtime) {
    size_t i = find_slot(u, path);
    if (i == u->nfiles) {
        return false;
    }
    u->files[i].has_mtime = true;
    u->files[i].mtime = mtime;
    return true;
}

bool ufs_rename(ufs *u, const char *from, const char *to) {
    if (strlen(to) >= VFS_PATH_MAX) {
        return false;
    }
    size_t i = find_slot(u, from);
    if (i == u->nfiles) {
        return false;
    }
    if (strcmp(from, to) == 0) {
        return true;
    }
    (void)ufs_remove(u, to); /* what was there goes; the slots after it slide down */
    i = find_slot(u, from);
    memcpy(u->files[i].path, to, strlen(to) + 1);
    return true;
}

size_t ufs_room(const ufs *u) {
    if (!u->open) {
        return 0;
    }
    return u->expect - u->files[u->nfiles].len;
}
