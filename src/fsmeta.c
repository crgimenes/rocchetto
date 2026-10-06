#include <stdio.h>
#include <string.h>

#include "roc.h"
#include "tree.h"

/* Days from 1970-01-01 to y-m-d, proleptic Gregorian (Howard Hinnant's). */
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2 ? 1 : 0;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - (era * 400);
    int64_t doy = (((153 * (m + (m > 2 ? -3 : 9))) + 2) / 5) + d - 1;
    int64_t doe = (yoe * 365) + (yoe / 4) - (yoe / 100) + doy;
    return (era * 146097) + doe - 719468;
}

static bool digits(const char *s, size_t n, int64_t *out) {
    int64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = (v * 10) + (s[i] - '0');
    }
    *out = v;
    return true;
}

/* The index's "YYYY-MM-DD HH:MM", read as UTC to the minute. */
static bool index_time(const char *when, int64_t *out) {
    int64_t y = 0;
    int64_t mo = 0;
    int64_t d = 0;
    int64_t h = 0;
    int64_t mi = 0;
    if (when == NULL || strlen(when) != 16 || when[4] != '-' || when[7] != '-' || when[10] != ' ' ||
        when[13] != ':' || !digits(when, 4, &y) || !digits(when + 5, 2, &mo) ||
        !digits(when + 8, 2, &d) || !digits(when + 11, 2, &h) || !digits(when + 14, 2, &mi) ||
        mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59) {
        return false;
    }
    *out = (days_from_civil(y, mo, d) * 86400) + (h * 3600) + (mi * 60);
    return true;
}

static bool under_home(const roc *m, const char *path) {
    char home[VFS_PATH_MAX];
    if (!roc_home_path(m, "", home, sizeof(home))) {
        return false;
    }
    size_t n = strlen(home);
    if (strncmp(path, home, n) != 0) {
        return false;
    }
    if (path[n] == '\0') {
        return true;
    }
    return path[n] == '/';
}

bool roc_stat_path(const roc *m, const char *path, bool follow, roc_stat *st, roc_origin *from) {
    memset(st, 0, sizeof(*st));
    *from = ROC_FROM_HOME;
    if (m->host.file_stat != NULL) {
        int r = m->host.file_stat(m->host.ctx, path, follow, st);
        if (r != ROC_HOST_NOT_MINE) {
            return r == ROC_HOST_YES; /* the storage as it is: the index may be behind it */
        }
        memset(st, 0, sizeof(*st));
    }
    const uint8_t *data = NULL;
    size_t len = 0;
    const ufs_file *mine = ufs_entry(&m->uf, path);
    if (mine != NULL) {
        st->has_size = true;
        st->size = mine->len;
        st->has_mtime = mine->has_mtime;
        st->mtime = mine->mtime;
        return true;
    }
    char tree[TREE_PATH_MAX];
    if (tree_path_of_vfs(path, tree, sizeof(tree)) && tree_find_file(tree, &data, &len)) {
        st->has_size = true;
        st->size = len;
        *from = ROC_FROM_BOARD;
        return true;
    }
    const vfs_node *node = vfs_lookup(&m->fs, path);
    if (node == NULL) {
        return false;
    }
    st->dir = node->dir;
    if (!node->dir) {
        st->has_size = true;
        st->size = node->size;
    }
    st->has_mtime = index_time(node->date, &st->mtime);
    *from = under_home(m, path) ? ROC_FROM_HOME : ROC_FROM_SITE;
    return true;
}

bool roc_dir_list(const roc *m, const char *dir, roc_dir_each each, void *user) {
    if (m->host.dir_list != NULL) {
        int r = m->host.dir_list(m->host.ctx, dir, each, user);
        if (r != ROC_HOST_NOT_MINE) {
            return r == ROC_HOST_YES;
        }
    }
    for (size_t i = 0; i < m->fs.nnodes; i++) {
        const vfs_node *e = &m->fs.nodes[i];
        if (vfs_is_child(e, dir)) {
            each(user, vfs_base(e), e->dir, false);
        }
    }
    return true;
}

/* The index's entry for path brought to what the host's storage says now:
   added when it appeared, gone when it went, its size and kind as they
   are. The index of a session is made when it opens; another window, the
   Files app or iCloud change the disk under it. */
static void sync_one(roc *m, const char *path, const roc_stat *st) {
    const vfs_node *n = vfs_lookup(&m->fs, path);
    uint32_t size = st->size > UINT32_MAX ? UINT32_MAX : (uint32_t)st->size;
    if (n != NULL && n->dir != st->dir) {
        (void)vfs_remove(&m->fs, path);
        n = NULL;
    }
    if (n == NULL || (!st->dir && n->size != size)) {
        (void)vfs_add_path(&m->fs, path, st->dir ? 0 : size, st->dir);
    }
}

const vfs_node *roc_lookup(roc *m, const char *path) {
    if (m->host.file_stat != NULL) {
        roc_stat st;
        int r = m->host.file_stat(m->host.ctx, path, true, &st);
        if (r == ROC_HOST_YES) {
            sync_one(m, path, &st);
        } else if (r == ROC_HOST_NO && vfs_lookup(&m->fs, path) != NULL &&
                   !vfs_has_children(&m->fs, path) && !roc_made_up(m, path)) {
            (void)vfs_remove(&m->fs, path);
        }
    }
    return vfs_lookup(&m->fs, path);
}

/* FNV-1a: what a listing saw, kept small; two paths sharing a hash only
   keep a gone entry one listing longer. */
static uint64_t path_hash(const char *p) {
    uint64_t h = 14695981039346656037ULL;
    while (*p != '\0') {
        h = (h ^ (uint8_t)*p++) * 1099511628211ULL;
    }
    return h;
}

typedef struct {
    roc *m;
    const char *dir;
    uint64_t seen[ROC_SYNC_SEEN];
    size_t nseen;
    bool overflow;
} syncing;

static void sync_entry(void *user, const char *name, bool dir, bool link) {
    syncing *s = user;
    char path[VFS_PATH_MAX];
    int n =
        snprintf(path, sizeof(path), "%s%s%s", s->dir, strcmp(s->dir, "/") == 0 ? "" : "/", name);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        return;
    }
    (void)link;
    roc_stat st;
    if (s->m->host.file_stat == NULL ||
        s->m->host.file_stat(s->m->host.ctx, path, true, &st) != ROC_HOST_YES) {
        memset(&st, 0, sizeof(st)); /* a dangling link: what the listing said */
        st.dir = dir;
    }
    sync_one(s->m, path, &st);
    if (s->nseen < ROC_SYNC_SEEN) {
        s->seen[s->nseen++] = path_hash(path);
    } else {
        s->overflow = true; /* too many to tell what went: only additions this time */
    }
}

void roc_sync_dir(roc *m, const char *dir) {
    if (m->host.dir_list == NULL) {
        return;
    }
    static syncing s; /* one shell thread */
    s.m = m;
    s.dir = dir;
    s.nseen = 0;
    s.overflow = false;
    if (m->host.dir_list(m->host.ctx, dir, sync_entry, &s) != ROC_HOST_YES || s.overflow) {
        return;
    }
    size_t i = 0;
    while (i < m->fs.nnodes) {
        const vfs_node *e = &m->fs.nodes[i];
        bool there = true; /* what is not this directory's, or cannot go, stays */
        if (vfs_is_child(e, dir) && !vfs_has_children(&m->fs, e->path)) {
            there = roc_made_up(m, e->path);
        }
        uint64_t h = there ? 0 : path_hash(e->path);
        for (size_t k = 0; k < s.nseen && !there; k++) {
            there = s.seen[k] == h;
        }
        if (there) {
            i++;
            continue;
        }
        char gone[VFS_PATH_MAX];
        (void)snprintf(gone, sizeof(gone), "%s", e->path);
        (void)vfs_remove(&m->fs, gone); /* the next entry slides into slot i */
    }
}
