#include "fileh.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "roc.h"
#include "script_data.h"

static script_exec *run_of(filo_ctx *ctx) {
    return ctx->host.user;
}

static roc *roc_of(filo_ctx *ctx) {
    return run_of(ctx)->m;
}

static filo_value nil_value(void) {
    filo_value v = {0};
    v.kind = FILO_LIST;
    return v;
}

/* A failure a utility reports and goes on from: its reason as a string. */
static int reason(filo_ctx *ctx, const char *why, filo_value *out) {
    size_t n = strlen(why);
    uint8_t *mem = filo_alloc(ctx, n > 0 ? n : 1);
    if (mem == NULL) {
        return filo_fail(ctx, "file: out of memory");
    }
    memcpy(mem, why, n);
    *out = filo_string(mem, (uint32_t)n);
    return FILO_OK;
}

static unsigned long long handle_of(const script_exec *r, size_t slot) {
    return ((unsigned long long)r->run_id * SC_FILES_MAX) + slot + 1ULL;
}

/* The file a handle names: this run's, open; NULL (failed) otherwise. */
static script_file *file_of(filo_ctx *ctx, const filo_value *v, const char *what) {
    script_exec *r = run_of(ctx);
    if (v->kind != FILO_NUMBER || v->u.num < 1 || v->u.num > 9e15 ||
        v->u.num != (double)(unsigned long long)v->u.num) {
        (void)filo_fail2(ctx, what, " expects a file handle");
        return NULL;
    }
    unsigned long long k = (unsigned long long)v->u.num - 1ULL;
    size_t slot = (size_t)(k % SC_FILES_MAX);
    if (k / SC_FILES_MAX != r->run_id || slot >= r->nfiles || r->files[slot].kind == SC_FILE_FREE) {
        (void)filo_fail2(ctx, what, ": not a file this run has open");
        return NULL;
    }
    return &r->files[slot];
}

/* Bytes of a file served from memory, found again on each read: a file of
   the home may move as others are written and removed. */
static bool mem_bytes(const roc *m, const script_file *f, const uint8_t **data, size_t *len) {
    if (f->mem != NULL) {
        *data = f->mem; /* the shell's tree: in flash, never moves */
        *len = (size_t)f->size;
        return true;
    }
    return roc_find_file(m, f->path, data, len);
}

/* Up to n bytes of a read handle from where it is, into dst; how many, 0
   at the end, -1 on an error. */
static long long take(roc *m, script_file *f, uint8_t *dst, size_t n) {
    if (f->kind == SC_FILE_MEM) {
        const uint8_t *data = NULL;
        size_t len = 0;
        if (!mem_bytes(m, f, &data, &len)) {
            return -1; /* gone while open */
        }
        if (f->pos >= len) {
            return 0;
        }
        size_t k = len - (size_t)f->pos < n ? len - (size_t)f->pos : n;
        memcpy(dst, data + f->pos, k);
        f->pos += k;
        return (long long)k;
    }
    /* host: pos is the next byte to fetch; what file-line read ahead, in
       buf[buf_at..buf_len), comes before it */
    size_t got = 0;
    if (f->buf_at < f->buf_len) {
        got = f->buf_len - f->buf_at < n ? f->buf_len - f->buf_at : n;
        memcpy(dst, f->buf + f->buf_at, got);
        f->buf_at += got;
    }
    while (got < n) {
        long long k = m->host.fh_read(m->host.ctx, f->host, f->pos, dst + got, n - got);
        if (k < 0) {
            return -1;
        }
        if (k == 0) {
            break;
        }
        got += (size_t)k;
        f->pos += (uint64_t)k;
    }
    return (long long)got;
}

/* The slot a new handle goes in; NULL (failed) when all are taken. */
static script_file *free_slot(filo_ctx *ctx, size_t *slot) {
    script_exec *r = run_of(ctx);
    for (size_t i = 0; i < r->nfiles; i++) {
        if (r->files[i].kind == SC_FILE_FREE) {
            *slot = i;
            memset(&r->files[i], 0, offsetof(script_file, buf));
            return &r->files[i];
        }
    }
    (void)filo_fail(ctx, "file-open: too many files open at once (file-close one)");
    return NULL;
}

static int want_path(filo_ctx *ctx, const filo_value *v, char *out, size_t cap) {
    char arg[VFS_PATH_MAX];
    if (v->kind != FILO_STRING || v->u.str.len >= sizeof(arg) ||
        memchr(v->u.str.ptr, 0, v->u.str.len) != NULL) {
        return filo_fail(ctx, "file-open expects a path");
    }
    memcpy(arg, v->u.str.ptr, v->u.str.len);
    arg[v->u.str.len] = '\0';
    if (!roc_resolve_arg(roc_of(ctx), arg, out, cap)) {
        return filo_fail(ctx, "file-open: File name too long");
    }
    return FILO_OK;
}

static int open_read(filo_ctx *ctx, script_file *f, filo_value *out) {
    roc *m = roc_of(ctx);
    if (m->host.fh_open != NULL) {
        int r = m->host.fh_open(m->host.ctx, f->path, ROC_FH_READ, &f->host, &f->size);
        if (r == ROC_HOST_CANCELLED) {
            return filo_fail(ctx, "file-open: Interrupted");
        }
        if (r == ROC_HOST_NO) {
            const vfs_node *node = roc_lookup(m, f->path);
            return reason(
                ctx, node != NULL && node->dir ? "Is a directory" : "No such file or directory",
                out);
        }
        if (r == ROC_HOST_YES) {
            f->kind = SC_FILE_HOST_READ;
            return FILO_OK;
        }
    }
    const uint8_t *data = NULL;
    size_t len = 0;
    if (!roc_find_file(m, f->path, &data, &len)) {
        const vfs_node *node = roc_lookup(m, f->path);
        if (node == NULL) {
            return reason(ctx, "No such file or directory", out);
        }
        return reason(ctx, node->dir ? "Is a directory" : "On the site, not here: cp it home first",
                      out);
    }
    const uint8_t *in_home = NULL;
    size_t home_len = 0;
    f->kind = SC_FILE_MEM;
    f->size = len;
    /* the home's may move as others are written: found again by name;
       the tree's stays where it is */
    f->mem = ufs_find(&m->uf, f->path, &in_home, &home_len) ? NULL : data;
    return FILO_OK;
}

static int open_write(filo_ctx *ctx, script_file *f, bool append, filo_value *out) {
    roc *m = roc_of(ctx);
    const char *why = roc_write_allowed(m, f->path);
    if (why != NULL) {
        return reason(ctx, why, out);
    }
    if (m->host.fh_open != NULL) {
        uint64_t size = 0;
        int r = m->host.fh_open(m->host.ctx, f->path, append ? ROC_FH_APPEND : ROC_FH_WRITE,
                                &f->host, &size);
        if (r == ROC_HOST_NO) {
            return reason(ctx, "Permission denied", out);
        }
        if (r == ROC_HOST_YES) {
            f->kind = SC_FILE_HOST_WRITE;
            f->written = append ? size : 0;
            return FILO_OK;
        }
    }
    /* in memory: written beside its name, which keeps the old file until
       the close; the one transfer the store has open at a time */
    if (m->uf.open) {
        return reason(ctx, "Device busy: one file is written at a time here", out);
    }
    if (snprintf(f->tmp, sizeof(f->tmp), "%s.~w", f->path) >= (int)sizeof(f->tmp)) {
        return reason(ctx, "File name too long", out);
    }
    size_t free = UFS_DATA_CAP - m->uf.used;
    size_t room = free < UFS_FILE_MAX ? free : UFS_FILE_MAX;
    static const char *const whys[] = {
        "", "File too large", "Disc quota exceeded", "File table overflow", "Device busy",
    };
    ufs_status st = ufs_begin(&m->uf, f->tmp, room);
    if (st != UFS_OK) {
        return reason(ctx, whys[st], out);
    }
    /* the old bytes first, found once the begin has compacted the store:
       they lie before the reservation, never in it */
    const uint8_t *old = NULL;
    size_t old_len = 0;
    if (append && ufs_find(&m->uf, f->path, &old, &old_len)) {
        if (!ufs_data(&m->uf, old, old_len)) {
            ufs_abort(&m->uf);
            return reason(ctx, "File too large", out);
        }
        f->written = old_len;
    }
    f->kind = SC_FILE_MEM_WRITE;
    return FILO_OK;
}

/* What a file read buys: as many steps a byte as the input gives, so a
   utility walks a file named on its line as far as one piped to it. */
static void grant(filo_ctx *ctx, size_t bytes) {
    uint64_t more = (uint64_t)SC_STEPS_PER_BYTE * bytes;
    filo_grant_steps(ctx, more > UINT32_MAX ? UINT32_MAX : (uint32_t)more);
}

/* (file-open p) to read, (file-open p "w") to write (the old file stays
   until the close), (file-open p "a") to add at the end: a handle (a
   number), or a string saying why not (no such file, a directory, not
   writable): a utility reports it and goes on. At most SC_FILES_MAX at
   once; what a run leaves open closes with it. */
static int b_file_open(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1 && n != 2) {
        return filo_fail(ctx, "file-open expects a path and maybe a mode (\"r\", \"w\", \"a\")");
    }
    char mode = 'r';
    if (n == 2) {
        if (a[1].kind != FILO_STRING || a[1].u.str.len != 1 ||
            strchr("rwa", a[1].u.str.ptr[0]) == NULL) {
            return filo_fail(ctx, "file-open: the mode is \"r\", \"w\" or \"a\"");
        }
        mode = (char)a[1].u.str.ptr[0];
    }
    char path[VFS_PATH_MAX] = {0};
    if (want_path(ctx, &a[0], path, sizeof(path)) != FILO_OK) {
        return FILO_ERR;
    }
    size_t slot = 0;
    script_file *f = free_slot(ctx, &slot);
    if (f == NULL) {
        return FILO_ERR;
    }
    memcpy(f->path, path, sizeof(path));
    out->kind = FILO_NUMBER;
    int rc = mode == 'r' ? open_read(ctx, f, out) : open_write(ctx, f, mode == 'a', out);
    if (rc != FILO_OK || f->kind == SC_FILE_FREE) {
        f->kind = SC_FILE_FREE;
        return rc; /* out holds the reason */
    }
    *out = filo_num((double)handle_of(run_of(ctx), slot));
    return FILO_OK;
}

/* (file-read h) and (file-read h n): the next n bytes (4096) of a file
   opened to read, fewer near the end, nil at the end. */
static int b_file_read(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1 && n != 2) {
        return filo_fail(ctx, "file-read expects a handle and maybe a count");
    }
    script_file *f = file_of(ctx, &a[0], "file-read");
    if (f == NULL) {
        return FILO_ERR;
    }
    if (f->kind != SC_FILE_MEM && f->kind != SC_FILE_HOST_READ) {
        return filo_fail(ctx, "file-read: the file is open to write");
    }
    double want = 4096;
    if (n == 2 && (a[1].kind != FILO_NUMBER || a[1].u.num < 1 || a[1].u.num > 16777216 ||
                   a[1].u.num != (double)(uint32_t)a[1].u.num)) {
        return filo_fail(ctx, "file-read: the count is 1 to 16777216");
    }
    if (n == 2) {
        want = a[1].u.num;
    }
    uint8_t *mem = filo_alloc(ctx, (size_t)want);
    if (mem == NULL) {
        return filo_fail(ctx, "file-read: out of memory");
    }
    long long k = take(roc_of(ctx), f, mem, (size_t)want);
    if (k < 0) {
        return filo_fail2(ctx, "file-read: Input/output error: ", f->path);
    }
    grant(ctx, (size_t)k);
    *out = k == 0 ? nil_value() : filo_string(mem, (uint32_t)k);
    return FILO_OK;
}

/* (file-line h): the next line with its \n, the last one without when the
   file does not end in one, nil at the end. A line of any length: pieces
   are read until its end. */
static int b_file_line(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    roc *m = roc_of(ctx);
    if (n != 1) {
        return filo_fail(ctx, "file-line expects a handle");
    }
    script_file *f = file_of(ctx, &a[0], "file-line");
    if (f == NULL) {
        return FILO_ERR;
    }
    if (f->kind == SC_FILE_MEM) {
        const uint8_t *data = NULL;
        size_t len = 0;
        if (!mem_bytes(m, f, &data, &len)) {
            return filo_fail2(ctx, "file-line: Input/output error: ", f->path);
        }
        if (f->pos >= len) {
            *out = nil_value();
            return FILO_OK;
        }
        const uint8_t *p = data + f->pos;
        const uint8_t *nl = memchr(p, '\n', len - (size_t)f->pos);
        size_t k = nl != NULL ? (size_t)(nl - p) + 1 : len - (size_t)f->pos;
        uint8_t *mem = filo_alloc(ctx, k);
        if (mem == NULL) {
            return filo_fail(ctx, "file-line: out of memory");
        }
        memcpy(mem, p, k);
        f->pos += k;
        grant(ctx, k);
        *out = filo_string(mem, (uint32_t)k);
        return FILO_OK;
    }
    if (f->kind != SC_FILE_HOST_READ) {
        return filo_fail(ctx, "file-line: the file is open to write");
    }
    /* the line gathers in a piece of memory that grows by doubling; what
       the last read took past its end stays in buf for the next one */
    size_t cap = 256;
    size_t len = 0;
    uint8_t *line = filo_alloc(ctx, cap);
    if (line == NULL) {
        return filo_fail(ctx, "file-line: out of memory");
    }
    for (;;) {
        if (f->buf_at == f->buf_len) {
            long long k = m->host.fh_read(m->host.ctx, f->host, f->pos, f->buf, sizeof(f->buf));
            if (k < 0) {
                return filo_fail2(ctx, "file-line: Input/output error: ", f->path);
            }
            f->buf_at = 0;
            f->buf_len = (size_t)k;
            if (k == 0) {
                break;
            }
            f->pos += (uint64_t)k;
        }
        const uint8_t *p = f->buf + f->buf_at;
        size_t avail = f->buf_len - f->buf_at;
        const uint8_t *nl = memchr(p, '\n', avail);
        size_t k = nl != NULL ? (size_t)(nl - p) + 1 : avail;
        if (len + k > cap) {
            while (len + k > cap) {
                cap *= 2;
            }
            uint8_t *bigger = filo_alloc(ctx, cap);
            if (bigger == NULL) {
                return filo_fail(ctx, "file-line: out of memory (a line too long)");
            }
            memcpy(bigger, line, len);
            line = bigger;
        }
        memcpy(line + len, p, k);
        len += k;
        f->buf_at += k;
        if (nl != NULL) {
            break;
        }
    }
    grant(ctx, len);
    *out = len == 0 ? nil_value() : filo_string(line, (uint32_t)len);
    return FILO_OK;
}

/* (file-seek h off): a file open to read goes on from byte off. */
static int b_file_seek(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 2 || a[1].kind != FILO_NUMBER || a[1].u.num < 0 || a[1].u.num > 9e15 ||
        a[1].u.num != (double)(unsigned long long)a[1].u.num) {
        return filo_fail(ctx, "file-seek expects a handle and a byte offset");
    }
    script_file *f = file_of(ctx, &a[0], "file-seek");
    if (f == NULL) {
        return FILO_ERR;
    }
    if (f->kind != SC_FILE_MEM && f->kind != SC_FILE_HOST_READ) {
        return filo_fail(ctx, "file-seek: only a file open to read moves");
    }
    f->pos = (uint64_t)a[1].u.num;
    f->buf_at = 0;
    f->buf_len = 0;
    *out = filo_bool(true);
    return FILO_OK;
}

/* (file-write h s ...): the strings (and numbers, as out-write spells them)
   at the end of a file open to write: #t, or a string saying why not (no
   room, the storage refused). */
static int b_file_write(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    roc *m = roc_of(ctx);
    if (n < 1) {
        return filo_fail(ctx, "file-write expects a handle and what to write");
    }
    script_file *f = file_of(ctx, &a[0], "file-write");
    if (f == NULL) {
        return FILO_ERR;
    }
    if (f->kind != SC_FILE_HOST_WRITE && f->kind != SC_FILE_MEM_WRITE) {
        return filo_fail(ctx, "file-write: the file is open to read");
    }
    for (uint32_t i = 1; i < n; i++) {
        const uint8_t *data = NULL;
        size_t len = 0;
        /* cppcheck-suppress variableScope ; data points into it past the if */
        char digits[66];
        if (a[i].kind == FILO_STRING) {
            data = a[i].u.str.ptr;
            len = a[i].u.str.len;
        } else if (a[i].kind == FILO_NUMBER && a[i].u.num == (double)(long long)a[i].u.num &&
                   a[i].u.num <= 9007199254740992.0 && a[i].u.num >= -9007199254740992.0) {
            len = script_whole_text(a[i].u.num, 10, digits);
            data = (const uint8_t *)digits;
        } else {
            return filo_fail(ctx, "file-write expects strings or whole numbers");
        }
        bool ok = false;
        if (f->kind == SC_FILE_HOST_WRITE) {
            ok = m->host.fh_write(m->host.ctx, f->host, data, len);
        } else {
            if (len <= ufs_room(&m->uf)) {
                ok = ufs_data(&m->uf, data, len);
            }
        }
        if (!ok) {
            return reason(
                ctx, f->kind == SC_FILE_MEM_WRITE ? "File too large" : "Input/output error", out);
        }
        f->written += len;
    }
    *out = filo_bool(true);
    return FILO_OK;
}

/* Closes f: a write is kept (commit) or dropped, the old file as it was.
   NULL, or why it could not be kept. */
static const char *close_file(roc *m, script_file *f, bool commit) {
    const char *why = NULL;
    switch (f->kind) {
    case SC_FILE_HOST_READ:
        (void)m->host.fh_close(m->host.ctx, f->host, false);
        break;
    case SC_FILE_HOST_WRITE:
        if (!m->host.fh_close(m->host.ctx, f->host, commit) && commit) {
            why = "Input/output error";
        } else if (commit) {
            (void)vfs_add_path(&m->fs, f->path,
                               f->written > UINT32_MAX ? UINT32_MAX : (uint32_t)f->written, false);
        }
        break;
    case SC_FILE_MEM_WRITE:
        if (!commit) {
            ufs_abort(&m->uf);
            break;
        }
        ufs_end(&m->uf);
        if (!ufs_rename(&m->uf, f->tmp, f->path)) {
            why = "Input/output error";
            break;
        }
        roc_ufs_stamp(m, f->path);
        (void)vfs_add_path(&m->fs, f->path, (uint32_t)f->written, false);
        roc_home_changed(m);
        break;
    case SC_FILE_MEM:
    case SC_FILE_FREE:
        break;
    }
    f->kind = SC_FILE_FREE;
    return why;
}

/* (file-close h): #t, or a string saying why what was written could not be
   kept (the old file then stays). */
static int b_file_close(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "file-close expects a handle");
    }
    script_file *f = file_of(ctx, &a[0], "file-close");
    if (f == NULL) {
        return FILO_ERR;
    }
    const char *why = close_file(roc_of(ctx), f, true);
    if (why != NULL) {
        return reason(ctx, why, out);
    }
    *out = filo_bool(true);
    return FILO_OK;
}

void script_files_close(roc *m, script_exec *r, bool commit) {
    for (size_t i = 0; i < r->nfiles; i++) {
        if (r->files[i].kind != SC_FILE_FREE) {
            (void)close_file(m, &r->files[i], commit);
        }
    }
}

const script_builtin script_file_builtins[] = {
    {"file-open", b_file_open}, {"file-read", b_file_read},   {"file-line", b_file_line},
    {"file-seek", b_file_seek}, {"file-write", b_file_write}, {"file-close", b_file_close},
};
const size_t script_file_count = sizeof(script_file_builtins) / sizeof(script_file_builtins[0]);
