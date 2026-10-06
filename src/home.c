#include "home.h"

#include <string.h>

static const char header_v1[] = "msh-home 1\n";
static const char header_v2[] = "msh-home 2\n";

static size_t put(uint8_t *buf, size_t cap, size_t at, const void *src, size_t n) {
    if (n > cap - at) {
        return at;
    }
    memcpy(buf + at, src, n);
    return at + n;
}

static size_t put_num(uint8_t *buf, size_t cap, size_t at, uint64_t n) {
    char digits[24];
    size_t k = sizeof(digits);
    do {
        k--;
        digits[k] = (char)('0' + (n % 10));
        n /= 10;
    } while (n > 0);
    return put(buf, cap, at, digits + k, sizeof(digits) - k);
}

/* True when path is home/<something>; rel gets the something. */
static bool under(const char *path, const char *home, const char **rel) {
    size_t n = strlen(home);
    if (strncmp(path, home, n) != 0 || path[n] != '/' || path[n + 1] == '\0') {
        return false;
    }
    *rel = path + n + 1;
    return true;
}

static void skip(home_report *rep, const char *rel) {
    if (rep->skipped == 0) {
        size_t n = strlen(rel);
        if (n >= sizeof(rep->first_skipped)) {
            n = sizeof(rep->first_skipped) - 1;
        }
        memcpy(rep->first_skipped, rel, n);
        rep->first_skipped[n] = '\0';
    }
    rep->skipped++;
}

size_t home_pack(const ufs *u, const vfs *v, const char *home, uint8_t *buf, size_t cap,
                 home_report *rep) {
    rep->files = 0;
    rep->dirs = 0;
    rep->skipped = 0;
    rep->first_skipped[0] = '\0';
    size_t at = put(buf, cap, 0, header_v2, sizeof(header_v2) - 1);
    for (size_t i = 0; i < v->nnodes; i++) {
        const char *rel = NULL;
        if (!v->nodes[i].dir || !under(v->nodes[i].path, home, &rel)) {
            continue;
        }
        size_t rlen = strlen(rel);
        if (rlen + 3 > cap - at) {
            skip(rep, rel);
            continue;
        }
        at = put(buf, cap, at, "d\t", 2);
        at = put(buf, cap, at, rel, rlen);
        at = put(buf, cap, at, "\n", 1);
        rep->dirs++;
    }
    for (size_t i = 0; i < u->nfiles; i++) {
        const ufs_file *f = &u->files[i];
        const char *rel = NULL;
        if (!under(f->path, home, &rel)) {
            continue;
        }
        size_t rlen = strlen(rel);
        size_t need = 2 + rlen + 1 + 10 + 1 + 20 + 1 + f->len + 1;
        if (f->len > HOME_FILE_MAX || need > cap - at) {
            skip(rep, rel);
            continue;
        }
        at = put(buf, cap, at, "f\t", 2);
        at = put(buf, cap, at, rel, rlen);
        at = put(buf, cap, at, "\t", 1);
        at = put_num(buf, cap, at, f->len);
        at = put(buf, cap, at, "\t", 1);
        if (!f->has_mtime) {
            at = put(buf, cap, at, "-", 1);
        } else if (f->mtime < 0) {
            at = put(buf, cap, at, "-", 1);
            at = put_num(buf, cap, at, (uint64_t)0 - (uint64_t)f->mtime);
        } else {
            at = put_num(buf, cap, at, (uint64_t)f->mtime);
        }
        at = put(buf, cap, at, "\n", 1);
        at = put(buf, cap, at, u->data + f->off, f->len);
        at = put(buf, cap, at, "\n", 1);
        rep->files++;
    }
    return at;
}

/* A relative path the home would accept: components that are not "", "."
   or "..", no control bytes, no tab. */
static bool safe_rel(const char *rel, size_t n) {
    if (n == 0 || n >= VFS_PATH_MAX - 32 || rel[0] == '/') {
        return false;
    }
    size_t i = 0;
    size_t start = 0;
    while (i <= n) {
        if (i == n || rel[i] == '/') {
            size_t clen = i - start;
            if (clen == 0 || (clen == 1 && rel[start] == '.') ||
                (clen == 2 && rel[start] == '.' && rel[start + 1] == '.')) {
                return false;
            }
            start = i + 1;
        } else if ((unsigned char)rel[i] < 0x20 || rel[i] == 0x7f) {
            return false;
        }
        i++;
    }
    return true;
}

/* home/rel into path when rel is one the home accepts. */
static bool home_path_of(const char *home, const uint8_t *rel, size_t rlen, char *path) {
    size_t homelen = strlen(home);
    if (rlen >= VFS_PATH_MAX || homelen + 1 + rlen >= VFS_PATH_MAX) {
        return false;
    }
    char r[VFS_PATH_MAX];
    memcpy(r, rel, rlen);
    r[rlen] = '\0';
    if (!safe_rel(r, rlen)) {
        return false;
    }
    memcpy(path, home, homelen);
    path[homelen] = '/';
    memcpy(path + homelen + 1, r, rlen + 1);
    return true;
}

/* Digits from at to end as a number; false when there are none, another
   byte, or more than max. */
static bool number(const uint8_t *b, size_t at, size_t end, uint64_t max, uint64_t *out) {
    uint64_t n = 0;
    if (at == end) {
        return false;
    }
    for (size_t i = at; i < end; i++) {
        if (b[i] < '0' || b[i] > '9' || n > max / 10) {
            return false;
        }
        n = (n * 10) + (uint64_t)(b[i] - '0');
    }
    *out = n;
    return n <= max;
}

static size_t find_byte(const uint8_t *b, size_t at, size_t end, uint8_t c) {
    while (at < end && b[at] != c) {
        at++;
    }
    return at;
}

/* The file's bytes into the store and the index; false when it does not go
   (a directory there, the store full). */
static bool restore_file(ufs *u, vfs *v, const char *path, const uint8_t *data, size_t n,
                         bool has_mtime, int64_t mtime) {
    const vfs_node *have = vfs_lookup(v, path);
    if ((have != NULL && have->dir) || ufs_begin(u, path, n) != UFS_OK) {
        return false;
    }
    (void)ufs_data(u, data, n);
    ufs_end(u);
    if (has_mtime) {
        (void)ufs_set_mtime(u, path, mtime);
    }
    (void)vfs_add_path(v, path, (uint32_t)n, false);
    return true;
}

static size_t unpack_v1(ufs *u, vfs *v, const char *home, const uint8_t *blob, size_t len) {
    size_t restored = 0;
    size_t at = sizeof(header_v1) - 1;
    while (at < len) {
        /* the record line: path TAB digits NL */
        size_t eol = find_byte(blob, at, len, '\n');
        if (eol == len) {
            break; /* truncated line */
        }
        size_t tab = find_byte(blob, at, eol, '\t');
        uint64_t n = 0;
        if (tab == eol || tab == at || !number(blob, tab + 1, eol, UFS_FILE_MAX, &n)) {
            break; /* not a record line: nothing after it can be trusted */
        }
        size_t body = eol + 1;
        if (n > len - body || (body + n < len && blob[body + n] != '\n')) {
            break; /* the length does not add up with what is left */
        }
        char path[VFS_PATH_MAX];
        if (home_path_of(home, blob + at, tab - at, path) &&
            restore_file(u, v, path, blob + body, (size_t)n, false, 0)) {
            restored++;
        }
        at = body + (size_t)n + 1;
    }
    return restored;
}

static size_t unpack_v2(ufs *u, vfs *v, const char *home, const uint8_t *blob, size_t len,
                        size_t *dirs) {
    size_t restored = 0;
    size_t at = sizeof(header_v2) - 1;
    while (at < len) {
        size_t eol = find_byte(blob, at, len, '\n');
        if (eol == len || eol - at < 3 || blob[at + 1] != '\t') {
            break; /* truncated, or not a record: nothing after it can be trusted */
        }
        char path[VFS_PATH_MAX];
        if (blob[at] == 'd') {
            if (home_path_of(home, blob + at + 2, eol - at - 2, path)) {
                const vfs_node *have = vfs_lookup(v, path);
                if ((have == NULL || have->dir) && vfs_add_path(v, path, 0, true)) {
                    (*dirs)++;
                }
            }
            at = eol + 1;
            continue;
        }
        if (blob[at] != 'f') {
            break;
        }
        /* f TAB path TAB len TAB (mtime | -) NL bytes NL */
        size_t t1 = find_byte(blob, at + 2, eol, '\t');
        size_t t2 = t1 == eol ? eol : find_byte(blob, t1 + 1, eol, '\t');
        uint64_t n = 0;
        if (t2 == eol || t1 == at + 2 || !number(blob, t1 + 1, t2, UFS_FILE_MAX, &n)) {
            break;
        }
        uint64_t mag = 0;
        bool before = blob[t2 + 1] == '-'; /* "-" alone: no time; "-n": before 1970 */
        bool has_mtime = true;
        if (before && eol - t2 <= 2) {
            has_mtime = false;
        }
        size_t from = before ? t2 + 2 : t2 + 1;
        if (has_mtime && !number(blob, from, eol, (uint64_t)INT64_MAX, &mag)) {
            break;
        }
        int64_t mtime = before ? -(int64_t)mag : (int64_t)mag;
        size_t body = eol + 1;
        if (n > len - body || (body + n < len && blob[body + n] != '\n')) {
            break;
        }
        if (home_path_of(home, blob + at + 2, t1 - at - 2, path) &&
            restore_file(u, v, path, blob + body, (size_t)n, has_mtime, mtime)) {
            restored++;
        }
        at = body + (size_t)n + 1;
    }
    return restored;
}

size_t home_unpack(ufs *u, vfs *v, const char *home, const uint8_t *blob, size_t len,
                   size_t *dirs) {
    size_t h1 = sizeof(header_v1) - 1;
    size_t h2 = sizeof(header_v2) - 1;
    size_t none = 0;
    if (dirs == NULL) {
        dirs = &none;
    }
    *dirs = 0;
    if (len >= h2 && memcmp(blob, header_v2, h2) == 0) {
        return unpack_v2(u, v, home, blob, len, dirs);
    }
    if (len >= h1 && memcmp(blob, header_v1, h1) == 0) {
        return unpack_v1(u, v, home, blob, len);
    }
    return 0; /* another version, or not a home at all: an empty home */
}
