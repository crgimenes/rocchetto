#include "script.h"

#include <string.h>

#include "fileh.h"
#include "filo_math.h"
#include "filo_nolibc.h"
#include "filo_strings.h"
#include "roc.h"
#include "screen.h"
#include "script_data.h"
#include "sh.h"

static script_exec *run_of(filo_ctx *ctx) {
    return ctx->host.user;
}

static roc *roc_of(filo_ctx *ctx) {
    return run_of(ctx)->m;
}

/* nil: the empty list, what is-nil answers true to. */
static filo_value nil_value(void) {
    filo_value v = {0};
    v.kind = FILO_LIST;
    return v;
}

static int arg_str(filo_ctx *ctx, const filo_value *v, const char *what, char *dst, size_t cap) {
    if (v->kind != FILO_STRING) {
        return filo_fail2(ctx, what, " expects a string");
    }
    if (v->u.str.len >= cap) {
        return filo_fail2(ctx, what, ": too long");
    }
    memcpy(dst, v->u.str.ptr, v->u.str.len);
    dst[v->u.str.len] = '\0';
    return FILO_OK;
}

static int put_value(filo_ctx *ctx, roc *m, const filo_value *v);

/* Text out, each argument on the line, a newline at the end. */
static int b_echo(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    roc *m = roc_of(ctx);
    uint32_t i = 0;
    while (i < n) {
        if (i > 0) {
            term_puts(&m->t, " ");
        }
        if (put_value(ctx, m, &a[i]) != FILO_OK) {
            return FILO_ERR;
        }
        i++;
    }
    term_puts(&m->t, "\r\n");
    *out = filo_bool(true);
    return FILO_OK;
}

/* less, cat, edit and upload wait on the host once the script is done:
   only the command the shell runs can leave one, not a run it called. */
static int at_top(filo_ctx *ctx, const char *what) {
    if (run_of(ctx)->outer != NULL) {
        return filo_fail2(ctx, what, ": not in a command another one ran (run)");
    }
    return FILO_OK;
}

static int schedule(filo_ctx *ctx, const filo_value *a, uint32_t n, script_action act,
                    const char *what) {
    if (at_top(ctx, what) != FILO_OK) {
        return FILO_ERR;
    }
    if (n != 1) {
        return filo_fail2(ctx, what, " expects a file");
    }
    script_state *s = &roc_of(ctx)->sc;
    if (arg_str(ctx, &a[0], what, s->path, sizeof(s->path)) != FILO_OK) {
        return FILO_ERR;
    }
    s->action = act;
    return FILO_OK;
}

static int b_less(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    *out = filo_bool(true);
    return schedule(ctx, a, n, SC_ACT_LESS, "less");
}

static int b_cat(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    *out = filo_bool(true);
    return schedule(ctx, a, n, SC_ACT_CAT, "cat");
}

/* (upload) asks the person for files; (upload "~/notes") says where they
   go. The picker is the host's, so it is the one action left for after. */
static int b_upload(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    *out = filo_bool(true);
    script_state *s = &roc_of(ctx)->sc;
    if (at_top(ctx, "upload") != FILO_OK) {
        return FILO_ERR;
    }
    if (n == 0) {
        s->path[0] = '\0';
        s->action = SC_ACT_UPLOAD;
        return FILO_OK;
    }
    return schedule(ctx, a, n, SC_ACT_UPLOAD, "upload");
}

/* (cp a b) and (mv a b): the shell's, with the same words when they refuse. */
static int two_paths(filo_ctx *ctx, const filo_value *a, uint32_t n, const char *what,
                     bool (*cmd)(roc *, const char *, const char *), filo_value *out) {
    char src[VFS_PATH_MAX];
    char dst[VFS_PATH_MAX];
    if (n != 2) {
        return filo_fail2(ctx, what, " expects a source and a destination");
    }
    if (arg_str(ctx, &a[0], what, src, sizeof(src)) != FILO_OK ||
        arg_str(ctx, &a[1], what, dst, sizeof(dst)) != FILO_OK) {
        return FILO_ERR;
    }
    *out = filo_bool(cmd(roc_of(ctx), src, dst));
    return FILO_OK;
}

static int b_cp(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return two_paths(ctx, a, n, "cp", roc_cmd_cp, out);
}

static int b_mv(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return two_paths(ctx, a, n, "mv", roc_cmd_mv, out);
}

static int one_path(filo_ctx *ctx, const filo_value *a, uint32_t n, const char *what,
                    bool (*cmd)(roc *, const char *), filo_value *out) {
    char path[VFS_PATH_MAX];
    if (n != 1) {
        return filo_fail2(ctx, what, " expects a path");
    }
    if (arg_str(ctx, &a[0], what, path, sizeof(path)) != FILO_OK) {
        return FILO_ERR;
    }
    *out = filo_bool(cmd(roc_of(ctx), path));
    return FILO_OK;
}

static int b_mkdir(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return one_path(ctx, a, n, "mkdir", roc_cmd_mkdir, out);
}

static int b_rmdir(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return one_path(ctx, a, n, "rmdir", roc_cmd_rmdir, out);
}

/* (pbcopy text) puts the string on the host's clipboard; true when it took. */
static int b_pbcopy(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1 || a[0].kind != FILO_STRING) {
        return filo_fail(ctx, "pbcopy expects a string");
    }
    *out = filo_bool(roc_cmd_pbcopy(roc_of(ctx), a[0].u.str.ptr, a[0].u.str.len));
    return FILO_OK;
}

/* (pbpaste) is the clipboard as a string, "" when empty or absent. */
static int b_pbpaste(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    roc *m = roc_of(ctx);
    if (n != 0) {
        return filo_fail(ctx, "pbpaste takes nothing");
    }
    size_t len = 0;
    const uint8_t *data =
        m->host.clipboard_get != NULL ? m->host.clipboard_get(m->host.ctx, &len) : NULL;
    if (data == NULL) {
        len = 0;
    }
    uint8_t *mem = filo_alloc(ctx, len > 0 ? len : 1);
    if (mem == NULL) {
        return filo_fail(ctx, "pbpaste: out of memory");
    }
    if (len > 0) {
        memcpy(mem, data, len);
    }
    *out = filo_string(mem, (uint32_t)len);
    return FILO_OK;
}

/* (read-file "~/x") is the file as a string: the shell's own and the
   user's, which are here; the site's are not, and say so. */
/* (is-file path): whether read-file would read it, a file here (the home
   or the shell's own), not one of the site still to fetch. */
static int b_is_file(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    roc *m = roc_of(ctx);
    char arg[VFS_PATH_MAX];
    if (n != 1 || arg_str(ctx, &a[0], "is-file", arg, sizeof(arg)) != FILO_OK) {
        return n != 1 ? filo_fail(ctx, "is-file expects a path") : FILO_ERR;
    }
    char path[VFS_PATH_MAX];
    const uint8_t *data = NULL;
    size_t len = 0;
    bool found = false;
    if (roc_resolve_arg(m, arg, path, sizeof(path))) {
        found = roc_find_file(m, path, &data, &len);
    }
    *out = filo_bool(found);
    return FILO_OK;
}

enum { WHOLE_SHORT = 3 }; /* opened, but not read whole: memory, or an I/O error */

/* A file only the host has (the site's in the browser, fetched while the
   shell waits), whole, as a string in out: ROC_HOST_YES, NO, CANCELLED,
   WHOLE_SHORT, or NOT_MINE when the host does not serve it. */
static int read_host_whole(filo_ctx *ctx, roc *m, const char *path, filo_value *out) {
    void *h = NULL;
    uint64_t size = 0;
    if (m->host.fh_open == NULL) {
        return ROC_HOST_NOT_MINE;
    }
    int r = m->host.fh_open(m->host.ctx, path, ROC_FH_READ, &h, &size);
    if (r != ROC_HOST_YES) {
        return r;
    }
    /* a size not known yet (the file arrives as it is read): the room grows */
    bool unknown = size == ROC_FH_SIZE_UNKNOWN;
    size_t cap = 4096;
    if (!unknown) {
        cap = size < UINT32_MAX ? (size_t)size : 0;
    }
    uint8_t *mem = unknown || size < UINT32_MAX ? filo_alloc(ctx, cap > 0 ? cap : 1) : NULL;
    size_t got = 0;
    bool failed = mem == NULL;
    while (!failed && (unknown || got < cap)) {
        if (unknown && got == cap) {
            uint8_t *bigger = cap < UINT32_MAX / 2 ? filo_alloc(ctx, cap * 2) : NULL;
            if (bigger == NULL) {
                failed = true;
                break;
            }
            memcpy(bigger, mem, got);
            mem = bigger;
            cap *= 2;
        }
        long long k = m->host.fh_read(m->host.ctx, h, got, mem + got, cap - got);
        if (k < 0) {
            failed = true;
        }
        if (k <= 0) {
            break;
        }
        got += (size_t)k;
    }
    (void)m->host.fh_close(m->host.ctx, h, false);
    if (failed || (!unknown && got != size)) {
        return WHOLE_SHORT;
    }
    *out = filo_string(mem, (uint32_t)got);
    return ROC_HOST_YES;
}

static int b_read_file(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    roc *m = roc_of(ctx);
    char arg[VFS_PATH_MAX];
    if (n != 1 || arg_str(ctx, &a[0], "read-file", arg, sizeof(arg)) != FILO_OK) {
        return n != 1 ? filo_fail(ctx, "read-file expects a path") : FILO_ERR;
    }
    char path[VFS_PATH_MAX];
    if (!roc_resolve_arg(m, arg, path, sizeof(path))) {
        return filo_fail2(ctx, "read-file: bad path: ", arg);
    }
    const uint8_t *data = NULL;
    size_t len = 0;
    if (!roc_find_file(m, path, &data, &len)) {
        int r = read_host_whole(ctx, m, path, out);
        if (r != ROC_HOST_NOT_MINE) {
            if (r == ROC_HOST_CANCELLED) {
                return filo_fail(ctx, "read-file: Interrupted");
            }
            if (r == WHOLE_SHORT) {
                return filo_fail2(ctx, "read-file: out of memory or I/O error: ", arg);
            }
            return r == ROC_HOST_YES
                       ? FILO_OK
                       : filo_fail2(ctx, "read-file: No such file or directory: ", arg);
        }
        const vfs_node *node = roc_lookup(m, path);
        if (node == NULL) {
            return filo_fail2(ctx, "read-file: No such file or directory: ", arg);
        }
        return filo_fail2(
            ctx, node->dir ? "read-file: Is a directory: " : "read-file: Operation not supported: ",
            arg);
    }
    uint8_t *mem = filo_alloc(ctx, len > 0 ? len : 1);
    if (mem == NULL) {
        return filo_fail(ctx, "read-file: out of memory");
    }
    memcpy(mem, data, len);
    *out = filo_string(mem, (uint32_t)len);
    return FILO_OK;
}

/* (write-file "~/x" text) puts the string in the home, whole. */
static int b_write_file(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    roc *m = roc_of(ctx);
    char arg[VFS_PATH_MAX];
    if (n != 2 || arg_str(ctx, &a[0], "write-file", arg, sizeof(arg)) != FILO_OK) {
        return n != 2 ? filo_fail(ctx, "write-file expects a path and a string") : FILO_ERR;
    }
    if (a[1].kind != FILO_STRING) {
        return filo_fail(ctx, "write-file expects a string");
    }
    char path[VFS_PATH_MAX];
    if (!roc_resolve_arg(m, arg, path, sizeof(path))) {
        return filo_fail2(ctx, "write-file: bad path: ", arg);
    }
    *out = filo_bool(roc_write_file(m, "write-file", path, a[1].u.str.ptr, a[1].u.str.len));
    return FILO_OK;
}

static int b_download(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return one_path(ctx, a, n, "download", roc_cmd_download, out);
}

static int b_home_export(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "home-export takes no argument");
    }
    *out = filo_bool(roc_home_export(roc_of(ctx)));
    return FILO_OK;
}

static int b_home_import(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return one_path(ctx, a, n, "home-import", roc_home_import, out);
}

/* (home-kept) is how many bytes the blob holds now, -1 where nothing is kept. */
static int b_home_kept(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "home-kept takes no argument");
    }
    *out = filo_num((double)roc_home_kept(roc_of(ctx)));
    return FILO_OK;
}

/* (home-keep): this tab's home is the kept one again, over another tab's;
   true when it was kept. */
static int b_home_keep(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "home-keep takes no argument");
    }
    *out = filo_bool(roc_home_keep(roc_of(ctx)));
    return FILO_OK;
}

/* (home-reload): once the script is done, the kept home is asked for and
   takes the place of this one, every file of it. */
static int b_home_reload(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "home-reload takes no argument");
    }
    if (at_top(ctx, "home-reload") != FILO_OK) {
        return FILO_ERR;
    }
    roc_of(ctx)->sc.action = SC_ACT_HOME_RELOAD;
    *out = filo_bool(true);
    return FILO_OK;
}

/* (edit "file") opens the editor once the script is done: a screen cannot
   be entered from inside a running program. */
static int b_edit(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    *out = filo_bool(true);
    return schedule(ctx, a, n, SC_ACT_EDIT, "edit");
}

static int b_rm(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    char path[VFS_PATH_MAX];
    if (n != 1 || arg_str(ctx, &a[0], "rm", path, sizeof(path)) != FILO_OK) {
        return n != 1 ? filo_fail(ctx, "rm expects a file") : FILO_ERR;
    }
    *out = filo_bool(roc_cmd_rm(roc_of(ctx), path));
    return FILO_OK;
}

static int b_cd(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    char path[VFS_PATH_MAX] = "";
    if (n > 1 || (n == 1 && arg_str(ctx, &a[0], "cd", path, sizeof(path)) != FILO_OK)) {
        return n > 1 ? filo_fail(ctx, "cd expects at most a directory") : FILO_ERR;
    }
    roc_cmd_cd(roc_of(ctx), path);
    *out = filo_bool(true);
    return FILO_OK;
}

static int b_pwd(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "pwd takes no argument");
    }
    *out = filo_cstring(roc_of(ctx)->cwd);
    return FILO_OK;
}

static int b_ver(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)ctx;
    (void)a;
    (void)n;
    *out = filo_cstring(ROC_VERSION);
    return FILO_OK;
}

/* (sgr 1 31) is the escape that turns bold red on, (sgr 0) turns it off:
   the way a script colours what it echoes, without the language knowing
   about escape bytes. */
static int b_sgr(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    char buf[64];
    size_t k = 0;
    buf[k++] = 0x1b;
    buf[k++] = '[';
    uint32_t i = 0;
    while (i < n) {
        if (a[i].kind != FILO_NUMBER || a[i].u.num < 0 || a[i].u.num > 255 || k + 5 > sizeof(buf)) {
            return filo_fail(ctx, "sgr expects attribute numbers 0..255");
        }
        uint32_t v = (uint32_t)a[i].u.num;
        if (i > 0) {
            buf[k++] = ';';
        }
        if (v >= 100) {
            buf[k++] = (char)('0' + (v / 100));
        }
        if (v >= 10) {
            buf[k++] = (char)('0' + ((v / 10) % 10));
        }
        buf[k++] = (char)('0' + (v % 10));
        i++;
    }
    buf[k++] = 'm';
    uint8_t *mem = filo_alloc(ctx, k);
    if (mem == NULL) {
        return filo_fail(ctx, "sgr: out of memory");
    }
    memcpy(mem, buf, k);
    *out = filo_string(mem, (uint32_t)k);
    return FILO_OK;
}

/* One value out, a string as its bytes, anything else as echo spells it. */
static int put_value(filo_ctx *ctx, roc *m, const filo_value *v) {
    if (v->kind == FILO_STRING) {
        term_write(&m->t, v->u.str.ptr, v->u.str.len);
        return FILO_OK;
    }
    char buf[256];
    size_t k = 0;
    if (filo_value_repr(ctx, v, buf, sizeof(buf), &k) != FILO_OK) {
        return FILO_ERR;
    }
    term_write(&m->t, (const uint8_t *)buf, k);
    return FILO_OK;
}

/* Text out with nothing added: no spaces between the pieces, no newline.
   For control sequences and for prompts that wait on the same line.
   Numbers and the rest come out as echo spells them: (write (+ 1 1))
   prints 2. */
static int b_write(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    roc *m = roc_of(ctx);
    uint32_t i = 0;
    while (i < n) {
        if (put_value(ctx, m, &a[i]) != FILO_OK) {
            return FILO_ERR;
        }
        i++;
    }
    *out = filo_bool(true);
    return FILO_OK;
}

/* Each value as data into sink: a string as its bytes, a whole number as
   its digits, another number as echo spells it; anything else is a mistake
   (a nil STDIN among them). Nothing between them, nothing after. */
static int put_data(filo_ctx *ctx, const filo_value *a, uint32_t n,
                    void (*sink)(roc *, const uint8_t *, size_t), const char *what,
                    filo_value *out) {
    roc *m = roc_of(ctx);
    for (uint32_t i = 0; i < n; i++) {
        if (a[i].kind == FILO_STRING) {
            sink(m, a[i].u.str.ptr, a[i].u.str.len);
            continue;
        }
        if (a[i].kind != FILO_NUMBER) {
            return filo_fail2(ctx, what, " expects strings or numbers");
        }
        double x = a[i].u.num;
        if (x == (double)(long long)x && x <= 9007199254740992.0 && x >= -9007199254740992.0) {
            char digits[66]; /* a whole number as its digits, never as an exponent */
            sink(m, (const uint8_t *)digits, script_whole_text(x, 10, digits));
            continue;
        }
        char buf[256];
        size_t k = 0;
        if (filo_value_repr(ctx, &a[i], buf, sizeof(buf), &k) != FILO_OK) {
            return FILO_ERR;
        }
        sink(m, (const uint8_t *)buf, k);
    }
    *out = filo_bool(true);
    return FILO_OK;
}

/* (out-write s ...) is the command's output as data: what a > or a | gets,
   byte for byte; (write) is for the terminal, which drops escapes in a
   capture. (err-write s ...) goes where errors go (2>, 2>&1). */
static int b_stdout(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return put_data(ctx, a, n, roc_out, "out-write", out);
}

static int b_stderr(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return put_data(ctx, a, n, roc_errout, "err-write", out);
}

/* (exit-status 1) makes 1 the command's exit status ($?, && and ||) once
   the script ends; it does not end it. (exit-status) is the one set so
   far, or the shell's $? before the script when none is. */
static int b_status(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    const roc *m = roc_of(ctx);
    script_exec *s = run_of(ctx);
    if (n == 0) {
        *out = filo_num((double)(s->status_set ? s->status : m->status));
        return FILO_OK;
    }
    if (n != 1 || a[0].kind != FILO_NUMBER || a[0].u.num < 0 || a[0].u.num > 255 ||
        a[0].u.num != (double)(int)a[0].u.num) {
        return filo_fail(ctx, "exit-status expects a whole number 0..255");
    }
    s->status = (int)a[0].u.num;
    s->status_set = true;
    *out = filo_num(a[0].u.num);
    return FILO_OK;
}

/* The command's input (a | or a <): first what a spooled | left in the
   host's spool, then what is in memory. */
static uint64_t in_total(const roc *m) {
    if (!m->piped || m->in == NULL) {
        return 0;
    }
    return (m->in_spool != NULL ? m->in_spool_len : 0) + m->in_len;
}

/* n bytes of the input from pos into dst; false on an I/O error. */
static bool in_peek(const roc *m, uint64_t pos, uint8_t *dst, size_t n) {
    uint64_t spooled = m->in_spool != NULL ? m->in_spool_len : 0;
    while (n > 0 && pos < spooled) {
        size_t want = spooled - pos < n ? (size_t)(spooled - pos) : n;
        long long k = m->host.spool_read(m->host.ctx, m->in_spool, pos, dst, want);
        if (k <= 0) {
            return false;
        }
        pos += (uint64_t)k;
        dst += k;
        n -= (size_t)k;
    }
    if (n > 0) {
        memcpy(dst, m->in + (pos - spooled), n);
    }
    return true;
}

/* The input from where the last read left it, into the run's memory: as
   large as the input is, unlike STDIN. */
static int in_take(filo_ctx *ctx, size_t n, filo_value *out) {
    const roc *m = roc_of(ctx);
    script_exec *s = run_of(ctx);
    uint64_t total = in_total(m);
    uint64_t left = total - (s->in_pos < total ? s->in_pos : total);
    if (n > left) {
        n = (size_t)left;
    }
    uint8_t *mem = filo_alloc(ctx, n > 0 ? n : 1);
    if (mem == NULL) {
        return filo_fail(ctx, "in-read: out of memory");
    }
    if (n > 0 && !in_peek(m, s->in_pos, mem, n)) {
        return filo_fail(ctx, "in-read: Input/output error");
    }
    s->in_pos += n;
    *out = filo_string(mem, (uint32_t)n);
    return FILO_OK;
}

static size_t in_left(filo_ctx *ctx) {
    const roc *m = roc_of(ctx);
    uint64_t pos = run_of(ctx)->in_pos;
    uint64_t total = in_total(m);
    return pos >= total ? 0 : (size_t)(total - pos);
}

/* How long the line at pos is, with its \n (the rest when none ends it). */
static size_t in_line_len(const roc *m, uint64_t pos, size_t left) {
    uint64_t spooled = m->in_spool != NULL ? m->in_spool_len : 0;
    size_t k = 0;
    static uint8_t piece[ROC_CFG_SPOOL_PIECE];
    while (pos + k < spooled) {
        size_t want =
            spooled - (pos + k) < sizeof(piece) ? (size_t)(spooled - (pos + k)) : sizeof(piece);
        if (!in_peek(m, pos + k, piece, want)) {
            return left;
        }
        const uint8_t *nl = memchr(piece, '\n', want);
        if (nl != NULL) {
            return k + (size_t)(nl - piece) + 1;
        }
        k += want;
    }
    const uint8_t *p = m->in + (pos + k - spooled);
    const uint8_t *nl = memchr(p, '\n', left - k);
    return nl != NULL ? k + (size_t)(nl - p) + 1 : left;
}

/* (in-read) is the rest of the input, "" when nothing is left; (in-read n)
   at most n bytes of it, nil at the end (an empty input is at its end). */
static int b_in_read(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n == 0) {
        return in_take(ctx, in_left(ctx), out);
    }
    if (n != 1 || a[0].kind != FILO_NUMBER || a[0].u.num < 1 ||
        a[0].u.num != (double)(uint32_t)a[0].u.num) {
        return filo_fail(ctx, "in-read expects nothing, or a count of bytes 1 or more");
    }
    if (in_left(ctx) == 0) {
        *out = nil_value();
        return FILO_OK;
    }
    return in_take(ctx, (size_t)a[0].u.num, out);
}

/* (in-line) is the next line of the input with its \n, the last one
   without when the input does not end in one; nil at the end. */
static int b_in_line(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    const roc *m = roc_of(ctx);
    if (n != 0) {
        return filo_fail(ctx, "in-line takes nothing");
    }
    size_t left = in_left(ctx);
    if (left == 0) {
        *out = nil_value();
        return FILO_OK;
    }
    return in_take(ctx, in_line_len(m, run_of(ctx)->in_pos, left), out);
}

/* (run args), (run args input) and (run args input env): the command args
   names (its words, a list of strings: a function, the shell's own, a
   script of /bin) run as the shell runs one, input its stdin (none without
   it, or nil), env a list of (name value) set and exported for it alone,
   and the result (status out err): its exit status, what it wrote to
   stdout and to stderr. Each word stays one word, whatever it holds (the
   line is sh_join's); a line longer than the shell reads (SH_LINE_MAX) is
   refused, as an exec past ARG_MAX is, and xargs batches under it. */
static int b_run(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    roc *m = roc_of(ctx);
    if (n < 1 || n > 3 || a[0].kind != FILO_LIST || a[0].u.seq.len == 0 ||
        (n >= 2 && a[1].kind != FILO_STRING && !(a[1].kind == FILO_LIST && a[1].u.seq.len == 0)) ||
        (n == 3 && a[2].kind != FILO_LIST)) {
        return filo_fail(ctx,
                         "run expects a list of words, maybe an input (or nil) and an env list");
    }
    static char line[SH_LINE_MAX + 1];
    static char words[SH_LINE_MAX + 1];
    static char *argv[SH_WORDS_MAX];
    size_t k = 0;
    /* NAME='value' before the command, each value quoted as a word is */
    filo_seq env = {NULL, 0};
    if (n == 3 && filo_arg_list(ctx, &a[2], &env) != FILO_OK) {
        return FILO_ERR;
    }
    for (uint32_t i = 0; i < env.len; i++) {
        const filo_value *e = &env.items[i];
        if (e->kind != FILO_TUPLE || e->u.seq.len != 2 || e->u.seq.items[0].kind != FILO_STRING ||
            e->u.seq.items[1].kind != FILO_STRING) {
            return filo_fail(ctx, "run: the env is a list of (name value)");
        }
        filo_str name = e->u.seq.items[0].u.str;
        filo_str value = e->u.seq.items[1].u.str;
        if (!sh_is_name((const char *)name.ptr, name.len) ||
            name.len + value.len + 1 > sizeof(words) || memchr(value.ptr, 0, value.len) != NULL) {
            return filo_fail(ctx, "run: an env name is a shell name, its value a string");
        }
        memcpy(words, value.ptr, value.len);
        words[value.len] = '\0';
        char *one[1] = {words};
        char quoted[SH_LINE_MAX + 1];
        if (!sh_join(one, 1, quoted, sizeof(quoted)) ||
            k + name.len + 1 + strlen(quoted) + 3 > SH_LINE_MAX) {
            return filo_fail(ctx, "run: Argument list too long");
        }
        memcpy(line + k, name.ptr, name.len);
        k += name.len;
        line[k++] = '=';
        const char *q = value.len > 0 ? quoted : "''";
        memcpy(line + k, q, strlen(q));
        k += strlen(q);
        line[k++] = ' ';
    }
    /* the words, NUL-ended one after another, for sh_join */
    size_t used = 0;
    filo_seq list;
    if (filo_arg_list(ctx, &a[0], &list) != FILO_OK) {
        return FILO_ERR;
    }
    uint32_t argc = list.len;
    if (argc > SH_WORDS_MAX) {
        return filo_fail(ctx, "run: Argument list too long");
    }
    for (uint32_t i = 0; i < argc; i++) {
        const filo_value *w = &list.items[i];
        if (w->kind != FILO_STRING || memchr(w->u.str.ptr, 0, w->u.str.len) != NULL) {
            return filo_fail(ctx, "run: each word is a string with no NUL in it");
        }
        if (used + w->u.str.len + 1 > sizeof(words)) {
            return filo_fail(ctx, "run: Argument list too long");
        }
        argv[i] = words + used;
        memcpy(words + used, w->u.str.ptr, w->u.str.len);
        used += w->u.str.len;
        words[used++] = '\0';
    }
    if (!sh_join(argv, (int)argc, line + k, sizeof(line) - k)) {
        return filo_fail(ctx, "run: Argument list too long");
    }
    k += strlen(line + k);
    size_t need = roc_call_keep(m);
    uint8_t *keep = filo_alloc(ctx, need > 0 ? need : 1);
    if (keep == NULL) {
        return filo_fail(ctx, "run: out of memory");
    }
    static roc_called res;
    char why[96];
    const uint8_t *in = n >= 2 && a[1].kind == FILO_STRING ? a[1].u.str.ptr : NULL;
    size_t in_len = n >= 2 && a[1].kind == FILO_STRING ? a[1].u.str.len : 0;
    if (!roc_call(m, line, k, in, in_len, keep, need, &res, why, sizeof(why))) {
        return filo_fail2(ctx, "run: ", why);
    }
    uint8_t *o = filo_alloc(ctx, res.out_len > 0 ? res.out_len : 1);
    uint8_t *e = filo_alloc(ctx, res.err_len > 0 ? res.err_len : 1);
    if (o == NULL || e == NULL) {
        return filo_fail(ctx, "run: out of memory");
    }
    memcpy(o, res.out, res.out_len);
    memcpy(e, res.err, res.err_len);
    filo_value parts[3] = {
        filo_num((double)res.status),
        filo_string(o, (uint32_t)res.out_len),
        filo_string(e, (uint32_t)res.err_len),
    };
    return filo_tuple(ctx, parts, 3, out);
}

/* (csi "2J") is ESC [ 2J: any control sequence, spelled without the escape
   byte the language's strings cannot hold. */
static int b_csi(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1 || a[0].kind != FILO_STRING || a[0].u.str.len > 32) {
        return filo_fail(ctx, "csi expects a short string");
    }
    size_t k = a[0].u.str.len + 2;
    uint8_t *mem = filo_alloc(ctx, k);
    if (mem == NULL) {
        return filo_fail(ctx, "csi: out of memory");
    }
    mem[0] = 0x1b;
    mem[1] = '[';
    memcpy(mem + 2, a[0].u.str.ptr, a[0].u.str.len);
    *out = filo_string(mem, (uint32_t)k);
    return FILO_OK;
}

/* (dir-entries "/pub") is the directory as data: a list of (name dir?)
   tuples in the order ls shows them. What tree, and anything that walks
   the tree, is built on. */
static int b_dir_entries(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    roc *m = roc_of(ctx);
    char arg[VFS_PATH_MAX] = "";
    if (n > 2 || (n >= 1 && arg_str(ctx, &a[0], "dir-entries", arg, sizeof(arg)) != FILO_OK)) {
        return n > 2 ? filo_fail(
                           ctx, "dir-entries expects a directory and, optionally, #t for dot files")
                     : FILO_ERR;
    }
    bool all = false;
    if (n == 2 && a[1].kind == FILO_BOOL) {
        all = a[1].u.b;
    }
    char dir[VFS_PATH_MAX];
    if (!roc_resolve_arg(m, arg[0] != '\0' ? arg : ".", dir, sizeof(dir))) {
        return filo_fail2(ctx, "dir-entries: bad path: ", arg);
    }
    const vfs_node *node = roc_lookup(m, dir);
    if (node == NULL || !node->dir) {
        return filo_fail2(ctx, "dir-entries: no such directory: ", dir);
    }
    roc_sync_dir(m, dir);
    /* as many as the directory has: they live in the run's memory */
    filo_value *items = filo_alloc(ctx, (m->fs.nnodes > 0 ? m->fs.nnodes : 1) * sizeof(filo_value));
    if (items == NULL) {
        return filo_fail(ctx, "dir-entries: out of memory");
    }
    uint32_t k = 0;
    size_t i = 0;
    while (i < m->fs.nnodes) {
        const vfs_node *e = &m->fs.nodes[i];
        i++;
        if (!vfs_is_child(e, dir) || (!all && vfs_hidden(e))) {
            continue;
        }
        const char *base = vfs_base(e);
        filo_value parts[2];
        parts[0] = filo_cstring(base);
        parts[1] = filo_bool(e->dir);
        if (filo_tuple(ctx, parts, 2, &items[k]) != FILO_OK) {
            return FILO_ERR;
        }
        k++;
    }
    return filo_list(ctx, items, k, out);
}

/* The shell's builtins, as every command sees them. */
static const struct {
    const char *name;
    filo_builtin fn;
} builtins[] = {
    {"echo", b_echo},
    {"less", b_less},
    {"cat", b_cat},
    {"cd", b_cd},
    {"pwd", b_pwd},
    {"ver", b_ver},
    {"sgr", b_sgr},
    {"write", b_write},
    {"out-write", b_stdout},
    {"err-write", b_stderr},
    {"exit-status", b_status},
    {"in-read", b_in_read},
    {"in-line", b_in_line},
    {"run", b_run},
    {"csi", b_csi},
    {"dir-entries", b_dir_entries},
    {"upload", b_upload},
    {"rm", b_rm},
    {"mkdir", b_mkdir},
    {"rmdir", b_rmdir},
    {"cp", b_cp},
    {"mv", b_mv},
    {"pbcopy", b_pbcopy},
    {"pbpaste", b_pbpaste},
    {"read-file", b_read_file},
    {"is-file", b_is_file},
    {"write-file", b_write_file},
    {"download", b_download},
    {"home-export", b_home_export},
    {"home-import", b_home_import},
    {"home-kept", b_home_kept},
    {"home-keep", b_home_keep},
    {"home-reload", b_home_reload},
    {"edt", b_edit},
    {"edit", b_edit},
};

/* Every builtin a script sees into ctx: false when one did not register
   (it would be missing without a word). */
static bool register_all(const roc *m, filo_ctx *ctx) {
    bool ok = filo_strings_register(ctx, &filo_nolibc_strings) == FILO_OK;
    /* floor, round, to-int and the rest that need no libm: the ones that
       would (sqrt, sin) stay out, undefined rather than failing when called */
    static const filo_math_fns no_libm = {0};
    if (filo_math_register(ctx, &no_libm) != FILO_OK) {
        ok = false;
    }
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        if (filo_register_builtin(ctx, builtins[i].name, builtins[i].fn) != FILO_OK) {
            ok = false;
        }
    }
    for (size_t i = 0; i < script_data_count; i++) {
        if (filo_register_builtin(ctx, script_data_builtins[i].name, script_data_builtins[i].fn) !=
            FILO_OK) {
            ok = false;
        }
    }
    for (size_t i = 0; i < script_file_count; i++) {
        if (filo_register_builtin(ctx, script_file_builtins[i].name, script_file_builtins[i].fn) !=
            FILO_OK) {
            ok = false;
        }
    }
    if (m->host.filo_extend != NULL && !m->host.filo_extend(m->host.ctx, ctx)) {
        ok = false;
    }
    return ok;
}

static const char *machine(void) {
#ifdef __wasm32__
    return "wasm32";
#elifdef __aarch64__
    return "arm64";
#elifdef __x86_64__
    return "x86_64";
#elifdef __XTENSA__
    return "xtensa";
#elifdef __riscv
    return "riscv";
#else
    return "unknown";
#endif
}

static void globals(const roc *m, filo_ctx *ctx) {
    (void)filo_set_global(ctx, "USER", filo_cstring(m->user));
    (void)filo_set_global(ctx, "HOSTNAME",
                          filo_cstring(m->host.host_name != NULL ? m->host.host_name : ""));
    (void)filo_set_global(ctx, "MACHINE", filo_cstring(machine()));
    (void)filo_set_global(ctx, "VERSION", filo_cstring(ROC_VERSION));
    (void)filo_set_global(ctx, "ARGS", filo_cstring(""));
    (void)filo_set_global(ctx, "STDIN", filo_cstring(""));
}

static void run_init(script_exec *r, roc *m, filo_ctx *ctx, script_exec *outer, sh_regex *re,
                     uint32_t nre, script_file *files, uint32_t nfiles) {
    r->m = m;
    r->ctx = ctx;
    r->outer = outer;
    r->status_set = false;
    r->status = 0;
    r->in_pos = 0;
    r->run_id = ++m->sc.runs;
    r->re_used = 0;
    r->nre = nre;
    r->re = re;
    r->nfiles = nfiles;
    r->files = files;
    for (uint32_t i = 0; i < nfiles; i++) {
        files[i].kind = SC_FILE_FREE;
    }
}

/* Filo asks at every step: the host only every SC_STOP_EVERY of them. */
/* cppcheck-suppress constParameterCallback ; filo_host's should_stop takes a void * */
static bool should_stop(void *user) {
    const script_exec *r = user;
    roc *m = r->m;
    if (m->host.interrupted == NULL || ++m->stop_polls < SC_STOP_EVERY) {
        return m->stopped;
    }
    m->stop_polls = 0;
    if (m->host.interrupted(m->host.ctx)) {
        m->stopped = true;
    }
    return m->stopped;
}

static void script_reset(roc *m) {
    script_state *s = &m->sc;
    filo_host host = filo_nolibc_host;
    host.user = &s->top;
    host.should_stop = should_stop;
    filo_init(&s->ctx, &host, s->persistent, sizeof(s->persistent), s->run, sizeof(s->run));
    s->broken = false;
    if (!register_all(m, &s->ctx)) {
        s->broken = true;
    }
    globals(m, &s->ctx);
    run_init(&s->top, m, &s->ctx, NULL, s->re, SC_REGEX_MAX, s->files, SC_FILES_MAX);
    s->ready = true;
}

/* The argument line, its words as sh reads them, as a list of strings in
   ARGS. */
static bool set_args(const roc *m, filo_ctx *ctx, const char *args) {
    static sh_line sl;
    static filo_value items[SH_WORDS_MAX];
    char why[64];
    if (!sh_split(args, &sl, why, sizeof(why))) {
        (void)filo_fail(ctx, why);
        return false;
    }
    uint32_t n = 0;
    while ((int)n < sl.argc) {
        items[n] = filo_cstring(sl.argv[n]);
        n++;
    }
    filo_value list = {0};
    if (filo_list(ctx, items, n, &list) != FILO_OK) {
        return false;
    }
    /* what a | hands the program, "" without one. STDIN is a copy in the
       context's lasting memory: an input too large for it is nil there
       (a script that uses it fails, never with half of it), and read
       whole with (in-read) */
    filo_value in = filo_cstring("");
    if (m->piped && m->in != NULL) {
        /* what the program left free, less room for the globals its run
           writes, which go there when it ends */
        size_t free = ctx->persistent.cap - ctx->persistent.used;
        size_t spare = SC_STDIN_SPARE;
        size_t room = free > spare * 2 ? free - spare : free / 2;
        in = m->in_len > room || m->in_spool != NULL ? nil_value()
                                                     : filo_string(m->in, (uint32_t)m->in_len);
    }
    if (filo_set_global(ctx, "STDIN", in) != FILO_OK) {
        return false;
    }
    return filo_set_global(ctx, "ARGS", list) == FILO_OK;
}

static void take_action(roc *m);

/* The steps a command may take: SC_STEPS, and SC_STEPS_PER_BYTE more for
   each byte of its input. */
static filo_limits limits_for(const roc *m) {
    uint64_t steps = SC_STEPS;
    if (m->piped && m->in != NULL) {
        uint64_t spooled = m->in_spool != NULL ? m->in_spool_len : 0;
        steps += (uint64_t)SC_STEPS_PER_BYTE * (m->in_len + spooled);
    }
    filo_limits l = {steps > UINT32_MAX ? UINT32_MAX : (uint32_t)steps, 0};
    return l;
}

/* An error ends the command with 1, or with the failure status the script
   set before it (exit-status 2, then a runtime error: 2). */
static void run_failed(roc *m, const script_exec *r, const char *name) {
    if (m->stopped) {
        /* a Ctrl-C: the script, and the line it is in, end as sh's do */
        if (r->outer == NULL) {
            m->stopped = false;
            roc_err(m, name, "", "Interrupted");
            m->status = 130;
            m->list.active = false;
        }
        return;
    }
    roc_err(m, name, "", filo_error(r->ctx)); /* an error, as 2> sends them */
    if (r->status_set && r->status > 0) {
        m->status = r->status;
    }
}

/* The script ended: the status it set, if it set one; the action it left,
   at the top. */
static void run_done(roc *m, const script_exec *r) {
    if (r->outer == NULL) {
        take_action(m);
    }
    if (r->status_set) {
        m->status = r->status;
    }
}

/* Compiles and runs one source, then takes the action it left. Every
   command starts from a fresh context, as a process does: the arenas only
   grow, and a session that kept each command's program would run out after
   a few of them (on a board, two). What the REPL defined is gone too. */
/* A context missing a builtin runs nothing: a script would find a name
   gone with no word of why. */
static bool usable(roc *m, const char *name) {
    if (m->sc.broken) {
        roc_err(m, name, "", "the shell's Filo is missing builtins (FILO_BUILTINS_MAX)");
        return false;
    }
    return true;
}

static size_t round16(size_t n) {
    return (n + 15U) & ~(size_t)15U;
}

/* The context a run happens in: the session's own for the command the shell
   runs; for one run while another goes on (run, or a $(...) whose command
   is a script), half of what the running one has left of its memory, which
   goes back with it: no static memory per level, and a run too deep stops
   for want of room instead of overwriting its caller. NULL, said, when it
   cannot start. */
static script_exec *open_run(roc *m, const char *name) {
    script_state *s = &m->sc;
    if (s->cur == NULL) {
        script_reset(m);
        if (!usable(m, name)) {
            return NULL;
        }
        s->action = SC_ACT_NONE;
        return &s->top;
    }
    filo_ctx *outer = s->cur->ctx;
    size_t budget = (outer->run.cap - outer->run.used) / 2;
    size_t head = round16(sizeof(filo_ctx)) + round16(sizeof(script_exec)) +
                  round16(SC_NESTED_REGEX * sizeof(sh_regex)) +
                  round16(SC_NESTED_FILES * sizeof(script_file));
    uint8_t *mem = budget >= head + SC_NESTED_MIN ? filo_alloc(outer, budget) : NULL;
    if (mem == NULL) {
        roc_err(m, name, "", "called too deep: no memory left to run it");
        return NULL;
    }
    filo_ctx *ctx = (filo_ctx *)(void *)mem;
    script_exec *r = (script_exec *)(void *)(mem + round16(sizeof(filo_ctx)));
    sh_regex *re =
        (sh_regex *)(void *)(mem + round16(sizeof(filo_ctx)) + round16(sizeof(script_exec)));
    script_file *files =
        (script_file *)(void *)((uint8_t *)re + round16(SC_NESTED_REGEX * sizeof(sh_regex)));
    uint8_t *rest = mem + head;
    size_t rest_len = budget - head;
    size_t lasting = round16(rest_len / 8) - 16U; /* globals and what they hold */
    filo_host host = filo_nolibc_host;
    host.user = r;
    host.should_stop = should_stop;
    filo_init(ctx, &host, rest, lasting, rest + lasting, rest_len - lasting);
    run_init(r, m, ctx, s->cur, re, SC_NESTED_REGEX, files, SC_NESTED_FILES);
    if (!register_all(m, ctx)) {
        roc_err(m, name, "", "the shell's Filo is missing builtins (FILO_BUILTINS_MAX)");
        return NULL;
    }
    globals(m, ctx);
    return r;
}

/* While r runs it is the innermost; a called run's cd is its own, undone
   when it returns, as a utility cannot move the shell that ran it. */
static void enter_run(roc *m, script_exec *r, char *cwd) {
    if (r->outer != NULL) {
        memcpy(cwd, m->cwd, sizeof(m->cwd));
    }
    m->sc.cur = r;
}

static void leave_run(roc *m, const script_exec *r, const char *cwd) {
    m->sc.cur = r->outer;
    if (r->outer != NULL) {
        memcpy(m->cwd, cwd, sizeof(m->cwd));
    }
}

static void run_source(roc *m, const char *name, const uint8_t *src, size_t len, const char *args) {
    script_exec *r = open_run(m, name);
    if (r == NULL) {
        return;
    }
    char cwd[sizeof(m->cwd)] = {0};
    enter_run(m, r, cwd);
    filo_prog prog;
    filo_limits limits = limits_for(m);
    filo_value v = {0};
    /* the program first: what STDIN may take is what its IR leaves free */
    bool ok = filo_compile(r->ctx, src, len, &prog) == FILO_OK;
    if (ok) {
        ok = set_args(m, r->ctx, args);
    }
    if (ok) {
        ok = filo_run(r->ctx, &prog, &limits, &v) == FILO_OK;
    }
    leave_run(m, r, cwd);
    script_files_close(m, r, ok);
    if (!ok) {
        run_failed(m, r, name);
        return;
    }
    run_done(m, r);
}

/* The same, for a program: its member name, a command (the entry "main":
   loaded — it says what it needs, and this shell refuses it when it lacks
   any — and run) or an app (the entry "draw": opened on the file named
   first, in the context apps run in). */
/* A program is named by its file: /home/guest/mine.fbb is "mine". name
   holds VFS_PATH_MAX bytes. */
/* The program's name: its file's, less an old .fbb when it has one. */
static void program_name(const char *path, char *name) {
    const char *base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;
    size_t bl = strlen(base);
    if (bl > 4 && strcmp(base + bl - 4, ".fbb") == 0) {
        bl -= 4;
    }
    memcpy(name, base, bl);
    name[bl] = '\0';
}

bool roc_is_program(const uint8_t *data, size_t len) {
    if (len < 5 || memcmp(data,
                          "\x7f"
                          "FBC",
                          4) != 0) {
        return false;
    }
    if (data[4] == 1) {
        return true; /* a unit */
    }
    return data[4] == 2; /* a bundle */
}

static void run_program(roc *m, const char *name, const char *path, const uint8_t *fbb, size_t len,
                        const char *args) {
    script_exec *r = open_run(m, name);
    if (r == NULL) {
        return;
    }
    const uint8_t *unit = NULL;
    size_t unit_len = 0;
    if (!roc_program_unit(r->ctx, fbb, len, name, &unit, &unit_len)) {
        run_failed(m, r, name);
        return;
    }
    /* main is a command, draw an app; with neither, the first entry is the
       command, as the desktop's filo run takes it (filo build x.filo makes
       the entry x) */
    char entry[64] = "main";
    if (!filo_bc_declares(unit, unit_len, "main") && !filo_bc_declares(unit, unit_len, "draw")) {
        uint32_t count = 0;
        filo_str first = {NULL, 0};
        if (!filo_bc_entry_at(unit, unit_len, 0, &count, &first) || first.len >= sizeof(entry)) {
            (void)filo_fail(r->ctx, "a program with no entry to run");
            run_failed(m, r, name);
            return;
        }
        memcpy(entry, first.ptr, first.len);
        entry[first.len] = '\0';
    } else if (!filo_bc_declares(unit, unit_len, "main")) {
        if (r->outer != NULL) {
            roc_err(m, name, "", "an app takes the screen: not in a command another one ran");
            return;
        }
#if ROC_APP_EDIT
        static sh_line sl; /* the first word: an app opens on one file */
        char why[64];
        if (!sh_split(args, &sl, why, sizeof(why))) {
            roc_err(m, name, "", why);
            return;
        }
        (void)edit_open_app(m, name, path, sl.argc > 0 ? sl.argv[0] : "", "");
#else
        (void)path;
        roc_err(m, name, args, "No apps in this build");
#endif
        return;
    }
    const filo_unit *u = NULL;
    filo_limits limits = limits_for(m);
    filo_value v = {0};
    char cwd[sizeof(m->cwd)] = {0};
    enter_run(m, r, cwd);
    bool ok = filo_bc_load(r->ctx, unit, unit_len, &u) == FILO_OK;
    if (ok) {
        ok = set_args(m, r->ctx, args);
    }
    if (ok) {
        ok = filo_bc_run(r->ctx, u, entry, &limits, &v) == FILO_OK;
    }
    leave_run(m, r, cwd);
    script_files_close(m, r, ok);
    if (!ok) {
        run_failed(m, r, name);
        return;
    }
    run_done(m, r);
}

/* The one action a script may leave for after it is done. */
static void take_action(roc *m) {
    script_state *s = &m->sc;
    if (s->action == SC_ACT_LESS) {
        roc_cmd_less(m, s->path);
    } else if (s->action == SC_ACT_CAT) {
        roc_cmd_cat(m, s->path);
    } else if (s->action == SC_ACT_UPLOAD) {
        roc_cmd_upload(m, s->path);
    } else if (s->action == SC_ACT_EDIT) {
#if ROC_APP_EDIT
        (void)edit_open(m, s->path);
#else
        roc_err(m, "edit", s->path, "No editor in this build");
#endif
    } else if (s->action == SC_ACT_HOME_RELOAD) {
        (void)roc_home_reload(m);
    }
    s->action = SC_ACT_NONE;
}

/* filo FILE: a program of ours or a source, by what the file holds; its
   whole name, nothing guessed. */
void script_run_path(roc *m, const char *path, const char *args) {
    if (!m->indexed) {
        term_puts(&m->t, "rocchetto: no index\r\n");
        return;
    }
    char full[VFS_PATH_MAX];
    if (path[0] == '\0') {
        term_puts(&m->t, "usage: filo <file> [args]\r\n");
        return;
    }
    if (!roc_resolve_arg(m, path, full, sizeof(full))) {
        return;
    }
    const vfs_node *node = roc_lookup(m, full);
    if (node == NULL || node->dir) {
        roc_err(m, "filo", full, node == NULL ? "No such file or directory" : "Is a directory");
        m->status = node == NULL ? 127 : 126;
        return;
    }
    const uint8_t *src = NULL;
    size_t len = 0;
    if (!roc_find_file(m, full, &src, &len)) {
        (void)script_load_begin(m, SC_LOAD_RUN, full,
                                args); /* the host's or the site's: once here */
        return;
    }
    if (roc_is_program(src, len)) {
        char name[VFS_PATH_MAX];
        program_name(full, name);
        run_program(m, name, full, src, len, args);
        return;
    }
    run_source(m, full, src, len, args);
}

/* A command named by its path (./ls, /bin/ls, ~/bin/x): it runs when it is
   a program of ours, as exec runs a binary; anything else is refused (a
   source runs with filo FILE). */
void script_exec_path(roc *m, const char *path, const char *args) {
    char full[VFS_PATH_MAX];
    if (!roc_resolve_arg(m, path, full, sizeof(full))) {
        return;
    }
    const uint8_t *data = NULL;
    size_t len = 0;
    if (!roc_find_file(m, full, &data, &len)) {
        const vfs_node *node = roc_lookup(m, full);
        if (node == NULL || node->dir) {
            roc_err(m, path, "", node == NULL ? "No such file or directory" : "Is a directory");
            m->status = node == NULL ? 127 : 126;
            return;
        }
        (void)script_load_begin(m, SC_LOAD_PROGRAM, full, args); /* checked once it is here */
        return;
    }
    if (!roc_is_program(data, len)) {
        roc_err(m, path, "", "cannot execute: not a program (a script runs with filo FILE)");
        m->status = 126;
        return;
    }
    char name[VFS_PATH_MAX];
    program_name(full, name);
    run_program(m, name, full, data, len, args);
}

bool roc_program_unit(filo_ctx *ctx, const uint8_t *data, size_t len, const char *name,
                      const uint8_t **unit, size_t *unit_len) {
    if (len >= 5 && memcmp(data,
                           "\x7f"
                           "FBC\x01",
                           5) == 0) { /* a unit, as filo build writes one */
        *unit = data;
        *unit_len = len;
        return true;
    }
    if (filo_bundle_find(ctx, data, len, name, unit, unit_len) == FILO_OK) {
        return true;
    }
    uint32_t count = 0;
    filo_str member = {NULL, 0};
    if (filo_bundle_at(ctx, data, len, 0, &count, &member, unit, unit_len) == FILO_OK &&
        count == 1) {
        return true;
    }
    (void)filo_fail2(ctx, "not a program named ", name);
    return false;
}

bool script_load_begin(roc *m, script_load kind, const char *path, const char *args) {
    script_state *s = &m->sc;
    if (strlen(args) >= sizeof(s->src_args) || strlen(path) >= sizeof(s->src_name)) {
        roc_err(m, "rocchetto", path, "Argument list too long");
        return false;
    }
    memcpy(s->src_name, path, strlen(path) + 1);
    memcpy(s->src_args, args, strlen(args) + 1);
    s->src_len = 0;
    s->src_overflow = false;
    s->load = kind;
    roc_reader_begin_script(m, path);
    return true;
}

void script_collect(roc *m, const uint8_t *data, size_t n) {
    script_state *s = &m->sc;
    if (n > sizeof(s->src) - s->src_len) {
        s->src_overflow = true;
        return;
    }
    memcpy(s->src + s->src_len, data, n);
    s->src_len += n;
}

/* A program of the site, fetched: a command runs from the bytes, which last
   as long as it does; an app reads its program again by its path each time
   it opens, and the site's is nowhere to be read, so it is copied first. */
static void run_site_program(roc *m) {
    script_state *s = &m->sc;
    char name[VFS_PATH_MAX];
    program_name(s->src_name, name);
    if (!roc_is_program(s->src, s->src_len)) {
        roc_err(m, s->src_name, "", "cannot execute: not a program (a script runs with filo FILE)");
        m->status = 126;
        return;
    }
    const uint8_t *unit = NULL;
    size_t unit_len = 0;
    script_reset(m);
    if (roc_program_unit(&s->ctx, s->src, s->src_len, name, &unit, &unit_len) &&
        !filo_bc_declares(unit, unit_len, "main") && filo_bc_declares(unit, unit_len, "draw")) {
        roc_err(m, name, s->src_name,
                "Operation not supported: an app runs from a copy, cp it home");
        return;
    }
    run_program(m, name, s->src_name, s->src, s->src_len, s->src_args);
}

void script_collected(roc *m) {
    const script_state *s = &m->sc;
    if (s->src_overflow) {
        roc_err(m, "rocchetto", s->src_name, "File too large");
        return;
    }
    if (s->load == SC_LOAD_CP) {
        (void)roc_write_file(m, "cp", s->src_args, s->src, s->src_len);
        return;
    }
    if (s->load == SC_LOAD_EDIT) {
#if ROC_APP_EDIT
        edit_open_bytes(m, s->src_name, s->src, s->src_len);
#else
        roc_err(m, "edit", s->src_name, "No editor in this build");
#endif
        return;
    }
    if (s->load == SC_LOAD_PROGRAM || roc_is_program(s->src, s->src_len)) {
        run_site_program(m);
        return;
    }
    run_source(m, s->src_name, s->src, s->src_len, s->src_args);
}

/* ---- the REPL ---- */

void script_repl_begin(roc *m) {
    script_state *s = &m->sc;
    if (!s->ready) {
        script_reset(m);
    }
    s->repl = true;
    s->repl_len = 0;
    term_puts(&m->t, "Filo. A line is read and shown; (def x 1) keeps x. "
                     "exit or Ctrl-D leaves.\r\n");
}

const char *script_repl_prompt(const roc *m) {
    if (!m->sc.repl) {
        return NULL;
    }
    return m->sc.repl_len > 0 ? "....> " : "filo> ";
}

/* Parens still open, outside strings and comments; negative means a stray
   close, which the compiler will name. */
static int open_parens(const uint8_t *src, size_t n) {
    int depth = 0;
    bool in_str = false;
    size_t i = 0;
    while (i < n) {
        uint8_t c = src[i];
        i++;
        if (in_str) {
            if (c == '\\' && i < n) {
                i++;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == ';') {
            while (i < n && src[i] != '\n') {
                i++;
            }
        } else if (c == '"') {
            in_str = true;
        } else if (c == '(') {
            depth++;
        } else if (c == ')') {
            depth--;
        }
    }
    return depth;
}

void script_repl_line(roc *m, const char *line) {
    script_state *s = &m->sc;
    size_t n = strlen(line);
    if (s->repl_len == 0 && (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0)) {
        s->repl = false;
        return;
    }
    if (n + 1 > sizeof(s->repl_buf) - s->repl_len) {
        term_puts(&m->t, "filo: line too long\r\n");
        s->repl_len = 0;
        return;
    }
    memcpy(s->repl_buf + s->repl_len, line, n);
    s->repl_len += n;
    s->repl_buf[s->repl_len] = '\n';
    s->repl_len++;
    if (open_parens(s->repl_buf, s->repl_len) > 0) {
        return; /* more to come */
    }
    size_t len = s->repl_len;
    s->repl_len = 0;
    s->action = SC_ACT_NONE;
    filo_prog prog;
    filo_limits limits = {SC_STEPS, 0};
    filo_value v = {0};
    s->cur = &s->top; /* a run called from the REPL nests under it */
    bool ok = filo_compile(&s->ctx, s->repl_buf, len, &prog) == FILO_OK;
    if (ok) {
        ok = filo_run(&s->ctx, &prog, &limits, &v) == FILO_OK;
    }
    s->cur = NULL;
    if (!ok) {
        term_puts(&m->t, "filo: ");
        term_puts(&m->t, filo_error(&s->ctx));
        term_puts(&m->t, "\r\n");
        return;
    }
    static char shown[SC_REPL_MAX];
    size_t k = 0;
    if (filo_value_repr(&s->ctx, &v, shown, sizeof(shown), &k) == FILO_OK) {
        term_write(&m->t, (const uint8_t *)shown, k);
        term_puts(&m->t, "\r\n");
    }
    take_action(m);
}

/* A command by name: the first program of that name in PATH. */
bool script_run(roc *m, const char *name, const char *args) {
    char path[VFS_PATH_MAX];
    if (roc_path_find(m, name, path, sizeof(path)) != 'p') {
        return false; /* none, or a shell script (the line runs those) */
    }
    const uint8_t *data = NULL;
    size_t len = 0;
    if (roc_find_file(m, path, &data, &len)) {
        run_program(m, name, path, data, len, args);
    } else {
        (void)script_load_begin(m, SC_LOAD_PROGRAM, path, args); /* on the host's disk */
    }
    return true;
}

filo_ctx *script_context(roc *m) {
    script_reset(m);
    return &m->sc.ctx;
}

bool script_build(roc *m, const uint8_t *src, size_t len, uint8_t *dst, size_t cap,
                  size_t *out_len) {
    script_state *s = &m->sc;
    script_reset(m);
    filo_prog prog;
    if (filo_compile(&s->ctx, src, len, &prog) != FILO_OK) {
        return false;
    }
    filo_bc_entry entry = {"main", &prog};
    return filo_bc_build(&s->ctx, &entry, 1, dst, cap, out_len) == FILO_OK;
}
