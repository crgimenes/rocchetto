#include "vfs.h"

#include <string.h>

void vfs_init(vfs *v) {
    v->arena_len = 0;
    v->nnodes = 0;
}

bool vfs_append(vfs *v, const uint8_t *data, size_t n) {
    if (n > VFS_ARENA_CAP - 1 - v->arena_len) {
        return false;
    }
    memcpy(v->arena + v->arena_len, data, n);
    v->arena_len += n;
    return true;
}

static uint32_t parse_u32(const char *s) {
    uint32_t n = 0;
    size_t i = 0;
    while (s[i] >= '0' && s[i] <= '9' && i < 10) {
        n = (n * 10) + (uint32_t)(s[i] - '0');
        i++;
    }
    return n;
}

/* Splits line into up to 4 TSV fields in place. Returns field count. */
static size_t split_tsv(char *line, char *fields[4]) {
    size_t nf = 0;
    char *p = line;
    fields[nf] = p;
    nf++;
    while (*p != '\0' && nf < 4) {
        if (*p == '\t') {
            *p = '\0';
            fields[nf] = p + 1;
            nf++;
        }
        p++;
    }
    return nf;
}

size_t vfs_parse(vfs *v) {
    v->arena[v->arena_len] = '\0';
    v->nnodes = 0;
    char *p = v->arena;
    while (*p != '\0' && v->nnodes < VFS_NODES_MAX) {
        char *line = p;
        while (*p != '\0' && *p != '\n') {
            p++;
        }
        if (*p == '\n') {
            *p = '\0';
            p++;
        }
        char *fields[4] = {0};
        size_t nf = split_tsv(line, fields);
        if (nf < 4 || fields[0][0] != '/') {
            continue;
        }
        vfs_node *n = &v->nodes[v->nnodes];
        n->path = fields[0];
        n->size = parse_u32(fields[1]);
        n->date = fields[2][0] != '\0' ? fields[2] : "-";
        n->title = fields[3];
        n->dir = false;
        size_t len = strlen(fields[0]);
        if (len > 1 && fields[0][len - 1] == '/') {
            fields[0][len - 1] = '\0';
            n->dir = true;
        }
        if (strcmp(n->path, "/") == 0) {
            n->dir = true;
        }
        v->nnodes++;
    }
    return v->nnodes;
}

const vfs_node *vfs_lookup(const vfs *v, const char *path) {
    if (strcmp(path, "/") == 0) {
        static const vfs_node root = {"/", "-", "", 0, true};
        return &root;
    }
    size_t i = 0;
    while (i < v->nnodes) {
        if (strcmp(v->nodes[i].path, path) == 0) {
            return &v->nodes[i];
        }
        i++;
    }
    return NULL;
}

bool vfs_is_child(const vfs_node *n, const char *dir) {
    size_t dlen = strlen(dir);
    if (strncmp(n->path, dir, dlen) != 0) {
        return false;
    }
    const char *rest = n->path + dlen;
    if (strcmp(dir, "/") != 0) {
        if (rest[0] != '/') {
            return false;
        }
        rest++;
    }
    if (rest[0] == '\0') {
        return false; /* the dir itself */
    }
    return strchr(rest, '/') == NULL;
}

bool vfs_resolve(const char *cwd, const char *arg, char *dst, size_t cap) {
    if (cap < 2) {
        return false;
    }
    char tmp[VFS_PATH_MAX];
    if (arg[0] == '/') {
        if (strlen(arg) >= sizeof(tmp)) {
            return false;
        }
        strcpy(tmp, arg);
    } else {
        size_t need = strlen(cwd) + 1 + strlen(arg) + 1;
        if (need > sizeof(tmp)) {
            return false;
        }
        strcpy(tmp, cwd);
        strcat(tmp, "/");
        strcat(tmp, arg);
    }
    /* normalize component by component */
    size_t out = 1;
    dst[0] = '/';
    dst[1] = '\0';
    const char *p = tmp;
    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *start = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        size_t clen = (size_t)(p - start);
        if (clen == 1 && start[0] == '.') {
            continue;
        }
        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            while (out > 1 && dst[out - 1] != '/') {
                out--;
            }
            if (out > 1) {
                out--; /* drop the slash too */
            }
            dst[out] = '\0';
            continue;
        }
        if (out + 1 + clen + 1 > cap) {
            return false;
        }
        if (out > 1) {
            dst[out] = '/';
            out++;
        }
        memcpy(dst + out, start, clen);
        out += clen;
        dst[out] = '\0';
    }
    if (out == 1) {
        dst[0] = '/';
        dst[1] = '\0';
    }
    return true;
}

bool vfs_add(vfs *v, const char *path, uint32_t size, bool dir) {
    size_t i = 0;
    while (i < v->nnodes) {
        if (strcmp(v->nodes[i].path, path) == 0) {
            if (!v->nodes[i].dir) {
                v->nodes[i].size = size;
            }
            return true;
        }
        i++;
    }
    if (strcmp(path, "/") == 0) {
        return true;
    }
    size_t n = strlen(path);
    if (v->nnodes >= VFS_NODES_MAX || n + 1 > VFS_ARENA_CAP - v->arena_len) {
        return false;
    }
    char *copy = v->arena + v->arena_len;
    memcpy(copy, path, n + 1);
    v->arena_len += n + 1;
    vfs_node *node = &v->nodes[v->nnodes];
    node->path = copy;
    node->size = size;
    node->date = "-";
    node->title = "";
    node->dir = dir;
    v->nnodes++;
    return true;
}

bool vfs_add_path(vfs *v, const char *path, uint32_t size, bool dir) {
    char tmp[VFS_PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof(tmp)) {
        return false;
    }
    memcpy(tmp, path, n + 1);
    size_t k = 1;
    while (tmp[k] != '\0') {
        if (tmp[k] == '/') {
            tmp[k] = '\0';
            if (!vfs_add(v, tmp, 0, true)) {
                return false;
            }
            tmp[k] = '/';
        }
        k++;
    }
    return vfs_add(v, path, size, dir);
}

bool vfs_has_children(const vfs *v, const char *dir) {
    size_t i = 0;
    while (i < v->nnodes) {
        if (vfs_is_child(&v->nodes[i], dir)) {
            return true;
        }
        i++;
    }
    return false;
}

bool vfs_remove(vfs *v, const char *path) {
    size_t i = 0;
    while (i < v->nnodes) {
        if (strcmp(v->nodes[i].path, path) == 0) {
            memmove(&v->nodes[i], &v->nodes[i + 1], (v->nnodes - i - 1) * sizeof(v->nodes[0]));
            v->nnodes--;
            return true;
        }
        i++;
    }
    return false;
}

const char *vfs_base(const vfs_node *n) {
    const char *base = strrchr(n->path, '/');
    return base != NULL ? base + 1 : n->path;
}

bool vfs_hidden(const vfs_node *n) {
    return vfs_base(n)[0] == '.';
}
