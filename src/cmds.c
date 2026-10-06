#include <stdio.h>
#include <string.h>

#include "printf.h"
#include "roc.h"
#include "sh.h"

#if ROC_APP_TOOLS
#include "diff.h"
#include "filotools.h"
#endif

/* ---- builtins ---- */

/* The commands that are C. Everything else the shell answers to is a script
   in /bin, which Tab finds by itself; what each one does is in
   bin/roc_help.md. Aliases (more, ?, logout, edit) stay in the dispatcher. */
static const char *const commands[] = {
#if ROC_APP_BOARD
    "articles",
#endif
    "cd",       "cat",     "less",
#if ROC_APP_COREWAR
    "mars",     "corewar",
#endif
#if ROC_APP_LIVE
    "live",
#endif
#if ROC_APP_TOOLS
    "diff",
#endif
    "menu",     "echo",    "filo",  "exit", "set",  "unset", "test",    "true",   "false",
    "printf",   "read",    "sleep", "type", "wait", "alias", "unalias", "pbcopy", "pbpaste",
};

size_t roc_command_count(void) {
    return sizeof(commands) / sizeof(commands[0]);
}

const char *roc_command_name(size_t i) {
    return commands[i];
}

bool roc_home_path(const roc *m, const char *file, char *out, size_t cap) {
    size_t n = strlen("/home/") + strlen(m->user) + (file[0] != '\0' ? 1 + strlen(file) : 0);
    if (n >= cap) {
        return false;
    }
    memcpy(out, "/home/", 6);
    memcpy(out + 6, m->user, strlen(m->user) + 1);
    if (file[0] != '\0') {
        out[6 + strlen(m->user)] = '/';
        memcpy(out + 7 + strlen(m->user), file, strlen(file) + 1);
    }
    return true;
}

static void say_here(roc *m, const char *msg);

size_t roc_err_text(char *buf, size_t cap, const char *cmd, const char *arg, const char *msg) {
    const char *parts[] = {"rocchetto: ", cmd, ": ", arg, arg[0] != '\0' ? ": " : "", msg};
    size_t k = 0;
    size_t i = 0;
    while (i < sizeof(parts) / sizeof(parts[0])) {
        size_t n = strlen(parts[i]);
        if (n > cap - 1 - k) {
            n = cap - 1 - k;
        }
        memcpy(buf + k, parts[i], n);
        k += n;
        i++;
    }
    buf[k] = '\0';
    return k;
}

static void err_line(roc *m, const char *buf, size_t n);

void roc_err(roc *m, const char *cmd, const char *arg, const char *msg) {
    m->status = 1;
    char buf[VFS_PATH_MAX + 96];
    size_t n = roc_err_text(buf, sizeof(buf), cmd, arg, msg);
    err_line(m, buf, n);
}

/* Into a capture as they are, cut at its end (and said to be). */
static void raw_into(roc *m, roc_capture *c, const uint8_t *data, size_t n) {
    while (n > sizeof(c->buf) - c->len) {
        size_t room = sizeof(c->buf) - c->len;
        memcpy(c->buf + c->len, data, room);
        c->len += room;
        data += room;
        n -= room;
        if (!roc_capture_spill(m, c)) {
            c->overflow = true;
            return;
        }
    }
    memcpy(c->buf + c->len, data, n);
    c->len += n;
}

/* To the terminal: a \n not already after a \r becomes \r\n. */
static void term_data(roc *m, const uint8_t *data, size_t n) {
    size_t from = 0;
    for (size_t i = 0; i < n; i++) {
        if (data[i] != '\n' || (i > 0 && data[i - 1] == '\r')) {
            continue;
        }
        term_write(&m->t, data + from, i - from);
        term_puts(&m->t, "\r\n");
        from = i + 1;
    }
    term_write(&m->t, data + from, n - from);
}

bool roc_out_terminal(const roc *m) {
    if (m->cap.on && !m->cap.paused) {
        return false;
    }
    if (roc_output_captured(m)) {
        return false;
    }
    return true;
}

void roc_out(roc *m, const uint8_t *data, size_t n) {
    if (m->cap.on && !m->cap.paused) {
        if (!m->cap.discard) {
            raw_into(m, &m->cap, data, n);
        }
        return;
    }
    if (!m->cap.on && roc_output_captured(m)) { /* a block's > or | */
        raw_into(m, &m->bcap, data, n);
        return;
    }
    term_data(m, data, n);
}

static void write_out(roc *m, const char *path, bool append, const uint8_t *data, size_t n);

void roc_errout(roc *m, const uint8_t *data, size_t n) {
    roc_errcap *e = &m->errcap;
    if (e->on && e->to_out) { /* 2>&1: as output, captured if it is */
        roc_out(m, data, n);
        return;
    }
    if (e->on) { /* 2> file: kept for the file, or 2> /dev/null: gone */
        if (e->discard) {
            return;
        }
        while (n > 0) {
            size_t room = sizeof(e->buf) - e->len;
            if (room == 0 && !e->keep && !m->uf.open) { /* the store writes one file at once */
                /* full: what it holds goes to the file now, the rest after it */
                write_out(m, e->path, e->append, e->buf, e->len);
                e->append = true;
                e->len = 0;
                room = sizeof(e->buf);
            }
            if (room == 0) {
                e->overflow = true; /* a run's own, kept for its caller: no file to go to */
                return;
            }
            size_t k = n < room ? n : room;
            memcpy(e->buf + e->len, data, k);
            e->len += k;
            data += k;
            n -= k;
        }
        return;
    }
    if (m->nberrs > 0 && m->list.active) { /* the 2> of the block it is in */
        const roc_block_err *b = &m->berrs[m->nberrs - 1];
        if (b->to_out) { /* where the block writes, not the | of the command in it */
            bool on = m->cap.on;
            m->cap.on = false;
            roc_out(m, data, n);
            m->cap.on = on;
            return;
        }
        if (b->discard) {
            return;
        }
        while (n > 0) {
            size_t room = sizeof(m->berr_buf) - m->berr_len;
            if (room == 0 && m->berr_len > b->start && !m->uf.open) {
                /* full: this block's part goes to its file now (emptied when
                   the block began); the blocks around it keep theirs */
                write_out(m, b->path, true, m->berr_buf + b->start, m->berr_len - b->start);
                m->berr_len = b->start;
                room = sizeof(m->berr_buf) - m->berr_len;
            }
            if (room == 0) {
                m->berr_overflow = true; /* the blocks around took it all */
                return;
            }
            size_t k = n < room ? n : room;
            memcpy(m->berr_buf + m->berr_len, data, k);
            m->berr_len += k;
            data += k;
            n -= k;
        }
        return;
    }
    bool paused = m->cap.paused;
    m->cap.paused = true; /* a refusal is for the eyes, not the file */
    term_data(m, data, n);
    m->cap.paused = paused;
}

/* A line for the errors, where they go now. */
static void err_line(roc *m, const char *buf, size_t n) {
    roc_errout(m, (const uint8_t *)buf, n);
    roc_errout(m, (const uint8_t *)"\n", 1);
}

/* What >&2 kept: its lines, each to where the refusals go now. */
static void err_text(roc *m, const uint8_t *text, size_t n) {
    size_t from = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && text[i] != '\n') {
            continue;
        }
        if (i > from || i < n) {
            char one[SH_LINE_MAX]; /* err_line's terminal wants a NUL; a longer line is cut */
            size_t k = i - from < sizeof(one) - 1 ? i - from : sizeof(one) - 1;
            memcpy(one, text + from, k);
            one[k] = '\0';
            err_line(m, one, k);
        }
        from = i + 1;
    }
}

/* A file a redirection writes: made, or emptied unless it is appended to,
   before the command runs, which is what sh does and the one check that a
   file can go there. False, said, when it cannot. */
static bool open_out(roc *m, const char *path, bool append) {
    roc_stat st;
    roc_origin from = ROC_FROM_HOME;
    if (append && roc_stat_path(m, path, true, &st, &from) && !st.dir) {
        return true; /* >> adds to what is there, the host's file or the store's */
    }
    return roc_write_file(m, "rocchetto", path, (const uint8_t *)"", 0);
}

static void spill_abandon(roc *m, roc_capture *c);

/* The host's file at path, open to write (append: at its end), in *h:
   ROC_HOST_YES, NO (it refused) or NOT_MINE (the store's). */
static int host_out(roc *m, const char *path, bool append, void **h) {
    uint64_t size = 0;
    *h = NULL;
    if (m->host.fh_open == NULL) {
        return ROC_HOST_NOT_MINE;
    }
    return m->host.fh_open(m->host.ctx, path, append ? ROC_FH_APPEND : ROC_FH_WRITE, h, &size);
}

/* A command's | that outgrew buf: buf after what the spool holds. */
static bool spool_more(roc *m, roc_capture *c) {
    if (m->host.spool_new == NULL) {
        return false;
    }
    if (c->spool == NULL) {
        c->spool = m->host.spool_new(m->host.ctx);
        c->spool_len = 0;
        if (c->spool == NULL) {
            return false;
        }
    }
    if (!m->host.spool_write(m->host.ctx, c->spool, c->buf, c->len)) {
        return false;
    }
    c->spool_len += c->len;
    c->len = 0;
    return true;
}

static void spool_free(roc *m, void **s) {
    if (*s != NULL) {
        m->host.spool_free(m->host.ctx, *s);
        *s = NULL;
    }
}

void roc_spool_drop(roc *m) {
    spool_free(m, &m->in_spool);
    m->in_spool_len = 0;
}

bool roc_input_spooled(roc *m, const char *cmd) {
    if (!m->piped || m->in_spool == NULL) {
        return false;
    }
    roc_err(m, cmd, "|",
            "Input too large to hold whole here: a Filo utility reads it (grep, head, sort...)");
    m->status = 1;
    return true;
}

/* cat of a spooled |: the spool a piece at a time, then the rest. */
static void cat_spooled(roc *m) {
    static uint8_t piece[ROC_CFG_SPOOL_PIECE];
    uint64_t off = 0;
    while (off < m->in_spool_len) {
        long long k = m->host.spool_read(m->host.ctx, m->in_spool, off, piece, sizeof(piece));
        if (k <= 0) {
            roc_err(m, "cat", "|", "Input/output error");
            return;
        }
        roc_show_bytes(m, false, "-", piece, (size_t)k);
        off += (uint64_t)k;
    }
    roc_show_bytes(m, false, "-", m->in, m->in_len);
}

bool roc_capture_spill(roc *m, roc_capture *c) {
    const char *path = c->path;
    bool append = c->append;
    if (c == &m->cap) {
        if (c->on && c->pipe && !c->paused) {
            return spool_more(m, c);
        }
        if (!c->on || c->pipe || c->keep || c->to_err || c->discard || c->paused) {
            return false;
        }
    } else {
        /* a block's: only when it is the one level, and a file */
        const roc_block_out *b = &m->bouts[0];
        if (c != &m->bcap || m->nbouts != 1 || b->pipe || b->to_err || b->discard ||
            b->start != 0) {
            return false;
        }
        path = b->path;
        append = b->append;
        memcpy(c->path, path, strlen(path) + 1);
    }
    if (!c->spilled) {
        /* the first piece: the file opened for the rest of the command */
        int r = host_out(m, path, append, &c->spill_h);
        if (r == ROC_HOST_NO) {
            return false;
        }
        c->spill_host = r == ROC_HOST_YES;
        if (!c->spill_host) {
            if (m->uf.open || snprintf(c->spill_tmp, sizeof(c->spill_tmp), "%s.~o", path) >=
                                  (int)sizeof(c->spill_tmp)) {
                return false; /* the store's one transfer is taken */
            }
            size_t free = UFS_DATA_CAP - m->uf.used;
            if (ufs_begin(&m->uf, c->spill_tmp, free < UFS_FILE_MAX ? free : UFS_FILE_MAX) !=
                UFS_OK) {
                return false;
            }
            /* found once the begin has compacted the store: before the
               reservation, never in it */
            const uint8_t *old = NULL;
            size_t old_len = 0;
            if (append && ufs_find(&m->uf, path, &old, &old_len) &&
                !ufs_data(&m->uf, old, old_len)) {
                ufs_abort(&m->uf);
                return false;
            }
        }
        c->spilled = true;
    }
    bool ok = false;
    if (c->spill_host) {
        ok = m->host.fh_write(m->host.ctx, c->spill_h, c->buf, c->len);
    } else {
        ok = ufs_data(&m->uf, c->buf, c->len);
    }
    if (!ok) {
        return false;
    }
    c->len = 0;
    return true;
}

/* The end of a > that spilled: the last piece (len bytes from text), then
   the file whole (the host's renamed over the old one, the store's in its
   place); false, said, when it did not go. */
static bool spill_end(roc *m, roc_capture *c, const uint8_t *text, size_t len) {
    c->spilled = false;
    if (c->spill_host) {
        bool wrote = true;
        if (c->overflow) {
            wrote = false;
        }
        if (wrote && len > 0) {
            wrote = m->host.fh_write(m->host.ctx, c->spill_h, text, len);
        }
        bool closed = m->host.fh_close(m->host.ctx, c->spill_h, wrote);
        if (!wrote || !closed) {
            roc_err(m, "rocchetto", c->path, "Input/output error");
            return false;
        }
        roc_stat st;
        roc_origin from = ROC_FROM_HOME;
        uint64_t size = roc_stat_path(m, c->path, true, &st, &from) ? st.size : 0;
        (void)vfs_add_path(&m->fs, c->path, size > UINT32_MAX ? UINT32_MAX : (uint32_t)size, false);
        return true;
    }
    if (c->overflow || (len > 0 && !ufs_data(&m->uf, text, len))) {
        ufs_abort(&m->uf);
        roc_err(m, "rocchetto", c->path, "File too large");
        return false;
    }
    ufs_end(&m->uf);
    if (!ufs_rename(&m->uf, c->spill_tmp, c->path)) {
        roc_err(m, "rocchetto", c->path, "Input/output error");
        return false;
    }
    roc_ufs_stamp(m, c->path);
    const ufs_file *f = ufs_entry(&m->uf, c->path);
    (void)vfs_add_path(&m->fs, c->path, f != NULL ? (uint32_t)f->len : 0, false);
    roc_home_changed(m);
    return true;
}

bool roc_capture_begin(roc *m, const char *arg, bool append) {
    roc_capture *c = &m->cap;
    if (arg[0] == '\0') {
        roc_err(m, "rocchetto", ">", "syntax error: a file is expected");
        return false;
    }
    if (!roc_resolve_arg(m, arg, c->path, sizeof(c->path))) {
        return false;
    }
    spill_abandon(m, c);
    c->discard = strcmp(c->path, "/dev/null") == 0; /* written nowhere */
    c->keep = false;
    c->to_err = false;
    if (!c->discard && !open_out(m, c->path, append)) {
        return false;
    }
    c->on = true;
    c->paused = false;
    c->append = append;
    c->overflow = false;
    c->pipe = false;
    c->spilled = false;
    c->esc = 0;
    c->len = 0;
    return true;
}

/* 2> file, 2>> file, 2>&1: where the command's refusals go. */
static bool err_begin(roc *m, const sh_line *sl) {
    roc_errcap *e = &m->errcap;
    e->len = 0;
    e->overflow = false;
    e->discard = false;
    e->keep = false;
    e->to_out = sl->err_to_out;
    e->append = sl->err_append;
    if (sl->err != NULL) {
        e->to_out = false;
        if (!roc_resolve_arg(m, sl->err, e->path, sizeof(e->path))) {
            return false;
        }
        e->discard = strcmp(e->path, "/dev/null") == 0;
        if (!e->discard && !open_out(m, e->path, e->append)) {
            return false;
        }
    }
    e->on = true;
    return true;
}

/* < f, or the | before it: a level of bin, what read reads in a block or a
   function, copied (the pipelines inside use pipe_buf again). False, said,
   when the file cannot be. */
static bool read_input(roc *m, const char *arg);

static bool block_in_push(roc *m, const sh_line *sl) {
    if (sl->here != NULL) {
        m->in = (const uint8_t *)sl->here;
        m->in_len = sl->here_len;
    } else if (sl->in != NULL && !read_input(m, sl->in)) {
        return false;
    }
    size_t start = 0;
    if (m->nbins > 0) {
        start = m->bins[m->nbins - 1].start + m->bins[m->nbins - 1].len;
    }
    size_t n = m->in_len;
    if (n > sizeof(m->bin) - start) {
        roc_err(m, "rocchetto", "<", "Input too large: the rest is cut");
        n = sizeof(m->bin) - start;
    }
    if (n > 0) {
        memmove(m->bin + start, m->in, n);
    }
    m->bins[m->nbins].start = start;
    m->bins[m->nbins].len = n;
    m->bins[m->nbins].cursor = 0;
    m->nbins++;
    return true;
}

/* 2> f, 2>> f or 2>&1 after a block, a function or a script: a level of
   berr, the file made or emptied now. False, said, when it cannot be. */
static bool block_err_push(roc *m, const sh_line *sl) {
    roc_block_err *b = &m->berrs[m->nberrs];
    b->to_out = false; /* 2>&1, and no 2> f after it */
    if (sl->err_to_out && sl->err == NULL) {
        b->to_out = true;
    }
    b->append = sl->err_append;
    b->discard = false;
    if (sl->err != NULL) {
        if (!roc_resolve_arg(m, sl->err, b->path, sizeof(b->path))) {
            return false;
        }
        b->discard = strcmp(b->path, "/dev/null") == 0;
        if (!b->discard && !open_out(m, b->path, b->append)) {
            return false;
        }
    }
    if (m->nberrs == 0) {
        m->berr_len = 0;
        m->berr_overflow = false;
    }
    b->start = m->berr_len;
    m->nberrs++;
    return true;
}

/* The block ended, or was left: what its refusals were, to their file. */
static void block_err_pop(roc *m) {
    m->nberrs--;
    const roc_block_err *b = &m->berrs[m->nberrs];
    size_t n = m->berr_len - b->start;
    m->berr_len = b->start;
    if (b->to_out || b->discard) {
        return;
    }
    /* the file was emptied when the block began: what it got goes after */
    write_out(m, b->path, true, m->berr_buf + b->start, n);
    if (m->berr_overflow) {
        m->berr_overflow = false;
        roc_err(m, "rocchetto", b->path, "Too many errors: the rest is cut");
    }
}

/* What a redirection kept, into its file: after what is there for >>
   (the host's file at its end; the store's written beside it, old bytes
   then new, and put in its place). */
static void write_out(roc *m, const char *path, bool append, const uint8_t *data, size_t n) {
    const uint8_t *old = NULL;
    size_t old_len = 0;
    void *h = NULL;
    if (append && host_out(m, path, true, &h) == ROC_HOST_YES) {
        bool ok = m->host.fh_write(m->host.ctx, h, data, n);
        if (!m->host.fh_close(m->host.ctx, h, ok) || !ok) {
            roc_err(m, "rocchetto", path, "Input/output error");
        }
        return;
    }
    if (!append || !ufs_find(&m->uf, path, &old, &old_len) || old_len == 0) {
        (void)roc_write_file(m, "rocchetto", path, data, n);
        return;
    }
    char tmp[VFS_PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.~o", path) >= (int)sizeof(tmp)) {
        roc_err(m, "rocchetto", path, "File name too long");
        return;
    }
    /* a reservation goes after every file: old stays where it is meanwhile */
    ufs_status st =
        old_len > UFS_FILE_MAX - n ? UFS_TOO_LARGE : ufs_begin(&m->uf, tmp, old_len + n);
    if (st != UFS_OK) {
        static const char *const why[] = {
            "", "File too large", "Disc quota exceeded", "File table overflow", "Device busy",
        };
        roc_err(m, "rocchetto", path, why[st]);
        return;
    }
    (void)ufs_data(&m->uf, old, old_len);
    (void)ufs_data(&m->uf, data, n);
    ufs_end(&m->uf);
    (void)ufs_rename(&m->uf, tmp, path);
    roc_ufs_stamp(m, path);
    (void)vfs_add_path(&m->fs, path, (uint32_t)(old_len + n), false);
    roc_home_changed(m);
}

void roc_err_end(roc *m) {
    roc_errcap *e = &m->errcap;
    if (!e->on) {
        return;
    }
    e->on = false;
    if (e->to_out || e->discard || e->keep) {
        return;
    }
    write_out(m, e->path, e->append, e->buf, e->len);
    if (e->overflow) {
        roc_err(m, "rocchetto", e->path, "Too many errors: the rest is cut");
    }
}

/* < file: the command's input, as a | hands it over; /dev/null, none. A
   file of the site not read yet cannot be (the input is here or nowhere). */
static bool read_input(roc *m, const char *arg) {
    char path[VFS_PATH_MAX];
    if (!roc_resolve_arg(m, arg, path, sizeof(path))) {
        return false;
    }
    m->piped = true;
    m->in = m->pipe_buf;
    m->in_len = 0;
    if (strcmp(path, "/dev/null") == 0) {
        return true;
    }
    const vfs_node *node = roc_lookup(m, path);
    if (node != NULL && node->dir) {
        roc_err(m, "rocchetto", path, "Is a directory");
        return false;
    }
    if (roc_find_file(m, path, &m->in, &m->in_len)) {
        return true;
    }
    if (node != NULL) {
        roc_err(m, "rocchetto", path, "On the site, not here: cp it home first");
        return false;
    }
    roc_err(m, "rocchetto", path, "No such file or directory");
    return false;
}

void roc_pipe_begin(roc *m) {
    roc_capture *c = &m->cap;
    spill_abandon(m, c);
    c->discard = false;
    c->keep = false;
    c->to_err = false;
    c->on = true;
    c->paused = false;
    c->append = false;
    c->overflow = false;
    c->pipe = true;
    c->spilled = false;
    c->esc = 0;
    c->len = 0;
}

/* What the commands of a $(...) write, for the line that called it. */
static uint8_t sub_out[ROC_CAP_MAX];
static size_t sub_len;
static bool sub_overflow;
static bool sub_long;                         /* a $(...) that did not end in ROC_CFG_SUB_STEPS */
static int sub_depth;                         /* how many $(...) run, one inside another */
static int sub_base;                          /* the block levels open when the innermost began */
static roc_list sub_saved[ROC_CFG_SUB_DEPTH]; /* the lines that called them, as they were */
/* roc_call's input: what the commands of the line it runs read when no |
   and no < of their own hand them one, at that $(...) level only */
static const uint8_t *call_in;
static size_t call_in_len;
static int call_level = -1;
static bool sub_ran;     /* a $(...) ran while the command was read: its $? is the command's */
static int sub_status;   /* when it has no name (x=$(cmd)), as POSIX says */
static int locals_floor; /* a $(...)'s frames count from 0: the locals of the line around it
                            are not theirs to put back */

void roc_capture_end(roc *m) {
    roc_capture *c = &m->cap;
    c->on = false;
    if (c->discard) {
        return;
    }
    if (c->to_err) {
        c->to_err = false;
        err_text(m, c->buf, c->len);
        return;
    }
    if (c->keep) {
        c->keep = false;
        size_t n = c->len;
        if (n > sizeof(sub_out) - sub_len || c->overflow) {
            n = sizeof(sub_out) - sub_len < n ? sizeof(sub_out) - sub_len : n;
            sub_overflow = true;
        }
        memcpy(sub_out + sub_len, c->buf, n);
        sub_len += n;
        return;
    }
    if (c->pipe) {
        c->pipe = false;
        roc_spool_drop(m); /* this command's own input is done with */
        m->in_spool = c->spool;
        m->in_spool_len = c->spool_len;
        c->spool = NULL;
        c->spool_len = 0;
        memcpy(m->pipe_buf, c->buf, c->len);
        m->in = m->pipe_buf;
        m->in_len = c->len;
        if (c->overflow) {
            roc_err(m, "rocchetto", "|", "Output too large: the rest is cut");
            m->pipe_cut = true;
        }
        return;
    }
    if (c->spilled) {
        (void)spill_end(m, c, c->buf, c->len);
        return;
    }
    if (c->overflow) {
        roc_err(m, "rocchetto", c->path, "File too large");
        return;
    }
    write_out(m, c->path, c->append, c->buf, c->len);
}

/* A block's > f or | b: a level of bcap, the file made or emptied now, as
   sh does before the block runs. */
static bool block_out_push(roc *m, const sh_line *tail) {
    roc_block_out *b = &m->bouts[m->nbouts];
    b->to_err = false;
    b->pipe = false;
    if (tail->out == NULL && tail->out_to_err) {
        b->to_err = true;
    } else if (tail->out == NULL) {
        b->pipe = true;
    }
    b->append = tail->append;
    b->discard = false;
    if (tail->out != NULL) {
        if (!roc_resolve_arg(m, tail->out, b->path, sizeof(b->path))) {
            return false;
        }
        b->discard = strcmp(b->path, "/dev/null") == 0;
        if (!b->discard && !open_out(m, b->path, b->append)) {
            return false;
        }
    }
    if (m->nbouts == 0) {
        m->bcap.len = 0;
        m->bcap.overflow = false;
        m->bcap.spilled = false;
        m->bcap.esc = 0;
    }
    b->start = m->bcap.len;
    m->nbouts++;
    return true;
}

/* A spill left behind (a line dropped mid-way): its file as it was. */
static void spill_abandon(roc *m, roc_capture *c) {
    spool_free(m, &c->spool);
    c->spool_len = 0;
    if (!c->spilled) {
        return;
    }
    c->spilled = false;
    if (c->spill_host) {
        (void)m->host.fh_close(m->host.ctx, c->spill_h, false);
    } else {
        ufs_abort(&m->uf);
    }
}

/* The levels from n on, gone with what they kept: a line dropped. */
static void block_out_drop_to(roc *m, int n) {
    if (n < m->nbouts) {
        m->bcap.len = m->bouts[n].start;
        m->nbouts = n;
        if (n == 0) {
            spill_abandon(m, &m->bcap);
        }
    }
}

/* The block ended: what it wrote to its file, or to the next command of
   the pipeline. True for a file (the next command then reads nothing). */
static bool block_out_pop(roc *m) {
    m->nbouts--;
    const roc_block_out *b = &m->bouts[m->nbouts];
    const uint8_t *text = m->bcap.buf + b->start;
    size_t n = m->bcap.len - b->start;
    m->bcap.len = b->start;
    bool overflow = m->bcap.overflow;
    m->bcap.overflow = false;
    if (b->to_err) {
        err_text(m, text, n);
        return true;
    }
    if (b->pipe) {
        memcpy(m->pipe_buf, text, n);
        m->in = m->pipe_buf;
        m->in_len = n;
        if (overflow) {
            roc_err(m, "rocchetto", "|", "Output too large: the rest is cut");
            m->pipe_cut = true;
        }
        return false;
    }
    if (m->bcap.spilled && m->nbouts == 0) {
        m->bcap.overflow = overflow;
        (void)spill_end(m, &m->bcap, text, n);
        m->bcap.overflow = false;
        return true;
    }
    if (overflow) {
        roc_err(m, "rocchetto", b->path, "File too large");
    } else if (!b->discard) {
        write_out(m, b->path, b->append, text, n);
    }
    return true;
}

/* Files the shell makes up as they are read: .history is the session's
   lines. One static buffer; the shell is one thread. */
static uint8_t made_up[ROC_HIST_MAX * ROC_LINE_MAX];

bool roc_made_up(const roc *m, const char *path) {
    char home[VFS_PATH_MAX];
    if (!roc_home_path(m, ".history", home, sizeof(home))) {
        return false;
    }
    return strcmp(path, home) == 0;
}

static bool show_made_up(roc *m, bool less, const char *path) {
    if (!roc_made_up(m, path)) {
        return false;
    }
    size_t n = roc_history_text(m, made_up, sizeof(made_up));
    roc_show_bytes(m, less, path, made_up, n);
    return true;
}

/* "~" and "~/x" are the home, as every shell reads them. */
bool roc_resolve_arg(roc *m, const char *arg, char *dst, size_t cap) {
    char home[VFS_PATH_MAX];
    const char *typed = arg;
    if (arg[0] == '~' && (arg[1] == '\0' || arg[1] == '/')) {
        if (!roc_home_path(m, arg[1] == '/' ? arg + 2 : "", home, sizeof(home))) {
            roc_err(m, "rocchetto", typed, "File name too long");
            return false;
        }
        arg = home;
    }
    if (!vfs_resolve(m->cwd, arg[0] != '\0' ? arg : "/", dst, cap)) {
        roc_err(m, "rocchetto", typed, "File name too long");
        return false;
    }
    return true;
}

/* The user's copy first, the built-in tree behind it. A board carries its
   own files in flash and whatever the user wrote shadows them, so editing
   /bin/ls.filo changes the ls this session runs — and removing that copy
   brings the factory one back. Nothing the user writes can damage the
   tree, which is the whole safety net: the worst case is the state the
   binary shipped with. */
bool roc_find_file(const roc *m, const char *path, const uint8_t **data, size_t *len) {
    if (ufs_find(&m->uf, path, data, len)) {
        return true;
    }
    if (m->host.file_get != NULL) {
        const uint8_t *p = m->host.file_get(m->host.ctx, path, len);
        if (p != NULL) {
            *data = p;
            return true;
        }
    }
    char tree[TREE_PATH_MAX];
    if (!tree_path_of_vfs(path, tree, sizeof(tree))) {
        return false;
    }
    return tree_find_file(tree, data, len);
}

/* True when the built-in tree has this path: what a write shadows and what
   a removal uncovers. */
static bool in_tree(const char *path, size_t *len) {
    char tree[TREE_PATH_MAX];
    const uint8_t *data = NULL;
    size_t n = 0;
    if (!tree_path_of_vfs(path, tree, sizeof(tree)) || !tree_find_file(tree, &data, &n)) {
        return false;
    }
    if (len != NULL) {
        *len = n;
    }
    return true;
}

#if ROC_APP_BOARD
/* articles [N]: the site's articles, all of /pub as the shell's Articles
   screen lists them (the index's order, newest first): number, date and
   title, one a row; N opens that one in the pager, as "Read #" does. */
static void articles(roc *m, const sh_line *sl) {
    long want = 0;
    if (sl->argc == 2) {
        const char *d = sl->argv[1];
        for (; *d >= '0' && *d <= '9' && want < 100000; d++) {
            want = (want * 10) + (*d - '0');
        }
        if (*d != '\0' || want < 1) {
            want = -1;
        }
    }
    if (sl->argc > 2 || want < 0) {
        roc_err(m, "articles", "", "usage: articles [number]");
        m->status = 2;
        return;
    }
    if (!m->indexed) {
        roc_err(m, "articles", "", "no index");
        m->status = 1;
        return;
    }
    size_t cols = roc_out_terminal(m) ? (size_t)m->t.cols : SIZE_MAX;
    long k = 0;
    for (size_t i = 0; i < m->fs.nnodes; i++) {
        const vfs_node *n = &m->fs.nodes[i];
        if (n->dir || !vfs_is_child(n, "/pub")) {
            continue;
        }
        k++;
        if (want > 0) {
            if (k == want) {
                roc_reader_begin_less(m, n->path);
                return;
            }
            continue;
        }
        const char *base = strrchr(n->path, '/');
        const char *label = n->title[0] != '\0' ? n->title : base + 1;
        char row[VFS_PATH_MAX + 64];
        int w = snprintf(row, sizeof(row), "%3ld  %-10.10s  ", k, n->date);
        size_t at = w > 0 ? (size_t)w : 0;
        size_t width = at;
        /* the title, cut where the terminal ends: a wrapped row breaks the column */
        for (const char *c = label; *c != '\0' && at + 4 < sizeof(row);) {
            uint32_t cp = 0;
            int resync = 0;
            utf8_dec d;
            utf8_dec_init(&d);
            size_t len = 0;
            utf8_result r = UTF8_MORE;
            while (r == UTF8_MORE && c[len] != '\0') {
                r = utf8_dec_feed(&d, (uint8_t)c[len], &cp, &resync);
                len++;
            }
            size_t cw = 1;
            if (r == UTF8_RUNE) {
                cw = (size_t)utf8_width(cp);
            } else if (r == UTF8_ERROR && resync != 0 && len > 1) {
                len--; /* that byte starts the next one */
            }
            if (cols != SIZE_MAX && width + cw >= cols) {
                break;
            }
            memcpy(row + at, c, len);
            at += len;
            width += cw;
            c += len;
        }
        row[at++] = '\n';
        roc_out(m, (const uint8_t *)row, at);
    }
    if (want > 0) {
        roc_err(m, "articles", sl->argv[1], "no such article");
        m->status = 1;
    }
}

#endif

bool roc_command_here(const roc *m, const char *name) {
    /* an app's shell (the fosforo app) has no site, so none of its articles */
    if ((m->flags & ROC_F_PROMPT) != 0) {
        return strcmp(name, "articles") != 0;
    }
    return true;
}

void roc_cmd_cd(roc *m, const char *arg) {
    if (!m->indexed) {
        term_puts(&m->t, "rocchetto: no index\r\n");
        return;
    }
    char path[VFS_PATH_MAX];
    if (arg[0] == '\0') {
        arg = "~"; /* cd alone goes home, as it does everywhere */
    }
    bool back = strcmp(arg, "-") == 0; /* cd -: where the last cd came from, said */
    if (back) {
        arg = sh_var_get(&m->vars, "OLDPWD", 6);
        if (arg == NULL) {
            roc_err(m, "cd", "", "OLDPWD not set");
            return;
        }
    }
    if (!roc_resolve_arg(m, arg, path, sizeof(path))) {
        return;
    }
    const vfs_node *node = roc_lookup(m, path);
    if (node == NULL) {
        roc_err(m, "cd", arg, "No such file or directory");
        return;
    }
    if (!node->dir) {
        roc_err(m, "cd", arg, "Not a directory");
        return;
    }
    char old[SH_VALUE_MAX + 8];
    char why[48];
    (void)snprintf(old, sizeof(old), "OLDPWD=%s", m->cwd);
    (void)sh_var_assign(&m->vars, old, why, sizeof(why));
    strcpy(m->cwd, path);
    if (back) {
        term_puts(&m->t, path);
        term_puts(&m->t, "\r\n");
    }
}

/* Shared front end of cat/less: resolves arg to an existing regular file. */
static bool resolve_file(roc *m, const char *cmd, const char *arg, char *path, size_t cap) {
    if (!m->indexed) {
        term_puts(&m->t, "rocchetto: no index\r\n");
        return false;
    }
    if (arg[0] == '\0') {
        term_puts(&m->t, "usage: ");
        term_puts(&m->t, cmd);
        term_puts(&m->t, " <file>\r\n");
        return false;
    }
    if (!roc_resolve_arg(m, arg, path, cap)) {
        return false;
    }
    const vfs_node *node = roc_lookup(m, path);
    if (node == NULL) {
        roc_err(m, cmd, arg, "No such file or directory");
        return false;
    }
    if (node->dir) {
        roc_err(m, cmd, arg, "Is a directory");
        return false;
    }
    return true;
}

void roc_cmd_cat(roc *m, const char *arg) {
    char path[VFS_PATH_MAX];
    if (!resolve_file(m, "cat", arg, path, sizeof(path))) {
        return;
    }
    const uint8_t *data = NULL;
    size_t len = 0;
    if (roc_find_file(m, path, &data, &len)) {
        roc_show_bytes(m, false, path, data, len); /* here already: no host round trip */
        return;
    }
    if (show_made_up(m, false, path)) {
        return;
    }
    /* reader takes over; prompt returns on EOF */
    roc_reader_begin_cat(m, path);
}

void roc_cmd_less(roc *m, const char *arg) {
    char path[VFS_PATH_MAX];
    if (!resolve_file(m, "less", arg, path, sizeof(path))) {
        return;
    }
    const uint8_t *data = NULL;
    size_t len = 0;
    if (roc_find_file(m, path, &data, &len)) {
        roc_show_bytes(m, true, path, data, len); /* here already: no host round trip */
        return;
    }
    if (show_made_up(m, true, path)) {
        return;
    }
    /* loads in the background; the pager opens on EOF */
    roc_reader_begin_less(m, path);
}

/* ---- the user's files ---- */

static bool under_home(const roc *m, const char *path) {
    char home[VFS_PATH_MAX];
    if (!roc_home_path(m, "", home, sizeof(home))) {
        return false;
    }
    size_t n = strlen(home);
    if (strncmp(path, home, n) != 0) {
        return false;
    }
    if (path[n] == '\0' || path[n] == '/') {
        return true;
    }
    return false;
}

/* Where a file lands: the directory asked for, else where the person is
   when that is their own, else ~/uploads, made on the spot. Only the home
   takes files; the rest of the tree is the site's. */
static bool upload_dest(roc *m, const char *arg, char *dst, size_t cap) {
    if (arg[0] == '\0' && under_home(m, m->cwd)) {
        memcpy(dst, m->cwd, strlen(m->cwd) + 1);
        return true;
    }
    if (arg[0] == '\0') {
        if (!roc_home_path(m, "uploads", dst, cap)) {
            return false;
        }
        (void)vfs_add_path(&m->fs, dst, 0, true);
        return true;
    }
    if (!roc_resolve_arg(m, arg, dst, cap)) {
        return false;
    }
    /* the site's tree is what it says it is: mounted read-only */
    if (!under_home(m, dst)) {
        roc_err(m, "upload", arg, "Read-only file system");
        return false;
    }
    const vfs_node *node = roc_lookup(m, dst);
    if (node == NULL) {
        roc_err(m, "upload", arg, "No such file or directory");
        return false;
    }
    if (!node->dir) {
        roc_err(m, "upload", arg, "Not a directory");
        return false;
    }
    return true;
}

void roc_cmd_upload(roc *m, const char *arg) {
    if (!m->indexed) {
        term_puts(&m->t, "rocchetto: no index\r\n");
        return;
    }
    char dest[VFS_PATH_MAX];
    if (!upload_dest(m, arg, dest, sizeof(dest))) {
        return;
    }
    if (m->host.pick_file == NULL) {
        roc_err(m, "upload", "", "No such device"); /* no picker to open here */
        return;
    }
    m->host.pick_file(m->host.ctx, dest);
}

bool roc_cmd_rm(roc *m, const char *arg) {
    if (!m->indexed) {
        term_puts(&m->t, "rocchetto: no index\r\n");
        return false;
    }
    if (arg[0] == '\0') {
        term_puts(&m->t, "usage: rm <file>\r\n");
        return false;
    }
    char path[VFS_PATH_MAX];
    if (!roc_resolve_arg(m, arg, path, sizeof(path))) {
        return false;
    }
    bool gone = ufs_remove(&m->uf, path);
    if (!gone && m->host.file_del != NULL) {
        gone = m->host.file_del(m->host.ctx, path);
    }
    if (!gone) {
        const vfs_node *node = roc_lookup(m, path);
        if (node != NULL && node->dir) {
            roc_err(m, "rm", arg, "Is a directory");
        } else {
            roc_err(m, "rm", arg,
                    node != NULL ? "Read-only file system" : "No such file or directory");
        }
        return false;
    }
    /* Removing a copy that shadowed the tree uncovers the factory file
       instead of leaving a hole: that is how a bad edit is undone. */
    size_t original = 0;
    if (in_tree(path, &original)) {
        (void)vfs_add(&m->fs, path, (uint32_t)original, false);
    } else {
        (void)vfs_remove(&m->fs, path);
    }
    roc_home_changed(m);
    return true;
}

bool roc_cmd_download(roc *m, const char *arg) {
    if (!m->indexed) {
        term_puts(&m->t, "rocchetto: no index\r\n");
        return false;
    }
    if (arg[0] == '\0') {
        term_puts(&m->t, "usage: download <file>\r\n");
        return false;
    }
    if (m->host.download == NULL) {
        roc_err(m, "download", "", "No such device");
        return false;
    }
    char path[VFS_PATH_MAX];
    if (!roc_resolve_arg(m, arg, path, sizeof(path))) {
        return false;
    }
    const vfs_node *node = roc_lookup(m, path);
    if (node == NULL) {
        roc_err(m, "download", arg, "No such file or directory");
        return false;
    }
    if (node->dir) {
        roc_err(m, "download", arg, "Is a directory");
        return false;
    }
    const uint8_t *data = NULL;
    size_t len = 0;
    if (!roc_find_file(m, path, &data, &len)) {
        roc_err(m, "download", arg,
                "Operation not supported"); /* the site's: the browser has it already */
        return false;
    }
    m->host.download(m->host.ctx, vfs_base(node), data, len);
    return true;
}

bool roc_home_export(roc *m) {
    if (m->host.download == NULL) {
        roc_err(m, "home", "export", "No such device");
        return false;
    }
    char home[VFS_PATH_MAX];
    if (!roc_home_path(m, "", home, sizeof(home))) {
        return false;
    }
    home_report rep;
    size_t n = home_pack(&m->uf, &m->fs, home, m->home_buf, sizeof(m->home_buf), &rep);
    if (rep.skipped > 0) {
        roc_err(m, "home", rep.first_skipped, "File too large; not in the export");
    }
    m->host.download(m->host.ctx, "rocchetto-home.txt", m->home_buf, n);
    return true;
}

bool roc_home_import(roc *m, const char *arg) {
    if (arg[0] == '\0') {
        term_puts(&m->t, "usage: home import <file>\r\n");
        return false;
    }
    char path[VFS_PATH_MAX];
    char home[VFS_PATH_MAX];
    if (!roc_resolve_arg(m, arg, path, sizeof(path)) || !roc_home_path(m, "", home, sizeof(home))) {
        return false;
    }
    const uint8_t *data = NULL;
    size_t len = 0;
    if (!roc_find_file(m, path, &data, &len)) {
        roc_err(m, "home", arg, "No such file or directory");
        return false;
    }
    /* the blob may be a file of the store, which the restore rewrites */
    if (len > sizeof(m->home_buf)) {
        roc_err(m, "home", arg, "File too large");
        return false;
    }
    memcpy(m->home_buf, data, len);
    size_t dirs = 0;
    size_t n = home_unpack(&m->uf, &m->fs, home, m->home_buf, len, &dirs);
    if (n == 0 && dirs == 0) {
        roc_err(m, "home", arg, "Invalid argument"); /* not a home, or one from another version */
        return false;
    }
    term_put_u32(&m->t, (uint32_t)n);
    term_puts(&m->t, n == 1 ? " file back in " : " files back in ");
    term_puts(&m->t, home);
    term_puts(&m->t, ".\r\n");
    roc_home_changed(m);
    return true;
}

long roc_home_kept(roc *m) {
    if (m->host.store_put == NULL) {
        return -1;
    }
    char home[VFS_PATH_MAX];
    if (!roc_home_path(m, "", home, sizeof(home))) {
        return -1;
    }
    home_report rep;
    return (long)home_pack(&m->uf, &m->fs, home, m->home_buf, sizeof(m->home_buf), &rep);
}

void roc_ufs_stamp(roc *m, const char *path) {
    int64_t secs = 0;
    int32_t tz = 0;
    if (m->host.clock != NULL && m->host.clock(m->host.ctx, &secs, &tz)) {
        (void)ufs_set_mtime(&m->uf, path, secs);
    }
}

bool roc_home_conflict(const roc *m) {
    return m->home_conflict;
}

bool roc_home_keep(roc *m) {
    if (m->host.store_put == NULL || m->host.store_claim == NULL) {
        roc_err(m, "home", "keep", "This terminal keeps nothing between visits");
        return false;
    }
    m->host.store_claim(m->host.ctx);
    m->home_conflict = false;
    roc_home_changed(m);
    bool kept = true;
    if (m->home_conflict) {
        kept = false;
    }
    return kept;
}

/* A word about keeping the home: a command's stderr while one runs (the
   change came from it), else above the line being typed (an upload). */
static void home_said(roc *m, const char *msg) {
    if (m->list.active) {
        err_line(m, msg, strlen(msg));
        return;
    }
    say_here(m, msg);
}

/* Every change to the home is a save of the whole home: 64 KB at most,
   on a keystroke's schedule. Said only when something did not go. */
void roc_home_changed(roc *m) {
    if (m->host.store_put == NULL) {
        return;
    }
    char home[VFS_PATH_MAX];
    if (!roc_home_path(m, "", home, sizeof(home))) {
        return;
    }
    if (m->home_conflict) {
        return; /* not over the other tab's: home keep or home reload first */
    }
    home_report rep;
    size_t n = home_pack(&m->uf, &m->fs, home, m->home_buf, sizeof(m->home_buf), &rep);
    char buf[VFS_PATH_MAX + 160];
    uint32_t r = m->host.store_put(m->host.ctx, m->home_buf, n);
    if (r == ROC_STORE_OTHER) {
        m->home_conflict = true;
        roc_err_text(buf, sizeof(buf), "home", "",
                     "another tab kept its home since this one read it; this tab's changes are "
                     "not kept (home says what to do)");
        home_said(m, buf);
        return;
    }
    if (r != 0) {
        roc_err_text(buf, sizeof(buf), "home", "", "No space left on device; not kept");
        home_said(m, buf);
        return;
    }
    if (rep.skipped > 0) {
        roc_err_text(buf, sizeof(buf), "home", rep.first_skipped,
                     "File too large; not kept between visits");
        home_said(m, buf);
    }
}

/* The parent of path must exist and be a directory of the user's. */
static bool parent_ok(roc *m, const char *cmd, const char *arg, const char *path) {
    char parent[VFS_PATH_MAX];
    const char *slash = strrchr(path, '/');
    size_t n = slash != NULL ? (size_t)(slash - path) : 0;
    if (n == 0) {
        n = 1;
    }
    memcpy(parent, path, n);
    parent[n] = '\0';
    if (!under_home(m, path) || strcmp(path, parent) == 0) {
        roc_err(m, cmd, arg, "Read-only file system");
        return false;
    }
    const vfs_node *up = roc_lookup(m, parent);
    if (up == NULL) {
        roc_err(m, cmd, arg, "No such file or directory");
        return false;
    }
    if (!up->dir) {
        roc_err(m, cmd, arg, "Not a directory");
        return false;
    }
    return true;
}

bool roc_cmd_mkdir(roc *m, const char *arg) {
    if (!m->indexed) {
        term_puts(&m->t, "rocchetto: no index\r\n");
        return false;
    }
    if (arg[0] == '\0') {
        term_puts(&m->t, "usage: mkdir <dir>\r\n");
        return false;
    }
    char path[VFS_PATH_MAX];
    if (!roc_resolve_arg(m, arg, path, sizeof(path)) || !parent_ok(m, "mkdir", arg, path)) {
        return false;
    }
    if (roc_lookup(m, path) != NULL) {
        roc_err(m, "mkdir", arg, "File exists");
        return false;
    }
    if (m->host.dir_make != NULL && !m->host.dir_make(m->host.ctx, path)) {
        roc_err(m, "mkdir", arg, "Read-only file system");
        return false;
    }
    if (!vfs_add(&m->fs, path, 0, true)) {
        roc_err(m, "mkdir", arg, "File table overflow");
        return false;
    }
    roc_home_changed(m);
    return true;
}

bool roc_cmd_pbcopy(roc *m, const uint8_t *data, size_t len) {
    if (m->host.clipboard_put == NULL) {
        term_puts(&m->t, "pbcopy: no clipboard on this host\r\n");
        return false;
    }
    if (!m->host.clipboard_put(m->host.ctx, data, len)) {
        term_puts(&m->t, "pbcopy: the clipboard refused\r\n");
        return false;
    }
    return true;
}

bool roc_cmd_pbpaste(roc *m) {
    if (m->host.clipboard_get == NULL) {
        term_puts(&m->t, "pbpaste: no clipboard on this host\r\n");
        return false;
    }
    size_t len = 0;
    const uint8_t *data = m->host.clipboard_get(m->host.ctx, &len);
    if (data == NULL || len == 0) {
        return true; /* an empty clipboard pastes nothing, as pbpaste does */
    }
    /* as a file is shown: a bare LF becomes CR LF on the terminal, and a
       | or $(...) takes the bytes as they are */
    roc_show_bytes(m, false, "-", data, len);
    return true;
}

bool roc_cmd_rmdir(roc *m, const char *arg) {
    if (!m->indexed) {
        term_puts(&m->t, "rocchetto: no index\r\n");
        return false;
    }
    if (arg[0] == '\0') {
        term_puts(&m->t, "usage: rmdir <dir>\r\n");
        return false;
    }
    char path[VFS_PATH_MAX];
    if (!roc_resolve_arg(m, arg, path, sizeof(path))) {
        return false;
    }
    const vfs_node *node = roc_lookup(m, path);
    if (node == NULL) {
        roc_err(m, "rmdir", arg, "No such file or directory");
        return false;
    }
    if (!node->dir) {
        roc_err(m, "rmdir", arg, "Not a directory");
        return false;
    }
    if (!parent_ok(m, "rmdir", arg, path)) {
        return false;
    }
    if (vfs_has_children(&m->fs, path)) {
        roc_err(m, "rmdir", arg, "Directory not empty");
        return false;
    }
    if (m->host.dir_del != NULL && !m->host.dir_del(m->host.ctx, path)) {
        roc_err(m, "rmdir", arg, "Read-only file system");
        return false;
    }
    (void)vfs_remove(&m->fs, path);
    roc_home_changed(m);
    return true;
}

const char *roc_write_allowed(roc *m, const char *path) {
    char parent[VFS_PATH_MAX];
    const char *slash = strrchr(path, '/');
    size_t n = slash != NULL ? (size_t)(slash - path) : 0;
    if (n == 0) {
        n = 1;
    }
    memcpy(parent, path, n);
    parent[n] = '\0';
    if (strcmp(path, parent) == 0) {
        return "Read-only file system";
    }
    /* The home is the user's, and so is anything that shadows the tree:
       writing there makes a copy, it does not touch what is in flash. So
       is /bin, where a program or a script of theirs is a command. */
    if (!under_home(m, path) && !in_tree(path, NULL) && strncmp(path, "/bin/", 5) != 0) {
        return "Read-only file system";
    }
    const vfs_node *up = roc_lookup(m, parent);
    if (up == NULL) {
        return "No such file or directory";
    }
    if (!up->dir) {
        return "Not a directory";
    }
    const vfs_node *have = roc_lookup(m, path);
    if (have != NULL && have->dir) {
        return "Is a directory";
    }
    return NULL;
}

const char *roc_write_check(roc *m, const char *path, const uint8_t *data, size_t len) {
    const char *refused = roc_write_allowed(m, path);
    if (refused != NULL) {
        return refused;
    }
    if (m->host.file_put != NULL) {
        if (!m->host.file_put(m->host.ctx, path, data, len)) {
            return "Input/output error";
        }
        (void)vfs_add(&m->fs, path, (uint32_t)len, false);
        return NULL;
    }
    ufs_status st = ufs_begin(&m->uf, path, len);
    if (st != UFS_OK) {
        static const char *const why[] = {
            "", "File too large", "Disc quota exceeded", "File table overflow", "Device busy",
        };
        return why[st];
    }
    (void)ufs_data(&m->uf, data, len);
    ufs_end(&m->uf);
    roc_ufs_stamp(m, path);
    (void)vfs_add(&m->fs, path, (uint32_t)len, false);
    roc_home_changed(m);
    return NULL;
}

bool roc_write_file(roc *m, const char *cmd, const char *path, const uint8_t *data, size_t len) {
    const char *why = roc_write_check(m, path, data, len);
    if (why != NULL) {
        roc_err(m, cmd, path, why);
        return false;
    }
    return true;
}

/* dst as typed, or dst/<base of src> when dst is a directory. */
static bool target_path(roc *m, const char *cmd, const char *src, const char *dst, char *out,
                        size_t cap) {
    if (!roc_resolve_arg(m, dst, out, cap)) {
        return false;
    }
    const vfs_node *node = roc_lookup(m, out);
    if (node == NULL || !node->dir) {
        return true;
    }
    const char *base = strrchr(src, '/');
    base = base != NULL ? base + 1 : src;
    size_t n = strlen(out);
    if (n + 1 + strlen(base) >= cap) {
        roc_err(m, cmd, dst, "File name too long");
        return false;
    }
    if (n > 1) {
        out[n] = '/';
        n++;
    }
    memcpy(out + n, base, strlen(base) + 1);
    return true;
}

/* A file only the host serves, whole into buf: ROC_HOST_* as fh_open
   answers; len past cap means it did not fit (nothing read). */
static int host_whole(roc *m, const char *path, uint8_t *buf, size_t cap, size_t *len) {
    void *h = NULL;
    uint64_t size = 0;
    if (m->host.fh_open == NULL) {
        return ROC_HOST_NOT_MINE;
    }
    int r = m->host.fh_open(m->host.ctx, path, ROC_FH_READ, &h, &size);
    if (r != ROC_HOST_YES) {
        return r;
    }
    bool unknown = size == ROC_FH_SIZE_UNKNOWN; /* it arrives as it is read */
    size_t want = unknown || size > cap ? cap : (size_t)size;
    size_t got = 0;
    bool failed = false;
    while (got < want || (unknown && got == cap)) {
        uint8_t probe = 0; /* past cap: whether there is more */
        uint8_t *at = got < cap ? buf + got : &probe;
        size_t n = got < cap ? want - got : 1;
        long long k = m->host.fh_read(m->host.ctx, h, got, at, n);
        if (k < 0) {
            failed = true;
        }
        if (k <= 0) {
            break;
        }
        got += (size_t)k;
        if (got > cap) {
            break;
        }
    }
    (void)m->host.fh_close(m->host.ctx, h, false);
    if (!unknown && size > cap) {
        *len = cap + 1;
        return ROC_HOST_YES;
    }
    *len = got;
    if (failed || (!unknown && got != size)) {
        return ROC_HOST_NO;
    }
    return ROC_HOST_YES;
}

bool roc_cmd_cp(roc *m, const char *src, const char *dst) {
    if (!m->indexed) {
        term_puts(&m->t, "rocchetto: no index\r\n");
        return false;
    }
    if (src[0] == '\0' || dst[0] == '\0') {
        term_puts(&m->t, "usage: cp <file> <file|dir>\r\n");
        return false;
    }
    char from[VFS_PATH_MAX];
    char to[VFS_PATH_MAX];
    if (!roc_resolve_arg(m, src, from, sizeof(from)) ||
        !target_path(m, "cp", from, dst, to, sizeof(to))) {
        return false;
    }
    const vfs_node *node = roc_lookup(m, from);
    if (node == NULL) {
        roc_err(m, "cp", src, "No such file or directory");
        return false;
    }
    if (node->dir) {
        roc_err(m, "cp", src, "Is a directory");
        return false;
    }
    if (strcmp(from, to) == 0) {
        roc_err(m, "cp", src, "Invalid argument"); /* onto itself */
        return false;
    }
    const uint8_t *data = NULL;
    size_t len = 0;
    /* the source may live in the store the write compacts: copy it first */
    static uint8_t hold[UFS_FILE_MAX];
    if (!roc_find_file(m, from, &data, &len)) {
        /* the site's: read now where the host waits for it (the browser),
           else fetched and written where asked after this line */
        int r = host_whole(m, from, hold, sizeof(hold), &len);
        if (r == ROC_HOST_NOT_MINE) {
            return script_load_begin(m, SC_LOAD_CP, from, to);
        }
        if (r != ROC_HOST_YES) {
            roc_err(m, "cp", src,
                    r == ROC_HOST_CANCELLED ? "Interrupted" : "No such file or directory");
            return false;
        }
        if (len > sizeof(hold)) {
            roc_err(m, "cp", src, "File too large");
            return false;
        }
        return roc_write_file(m, "cp", to, hold, len);
    }
    if (len > sizeof(hold)) {
        roc_err(m, "cp", src, "File too large");
        return false;
    }
    memcpy(hold, data, len);
    return roc_write_file(m, "cp", to, hold, len);
}

static bool prefixed(const char *path, const char *dir, size_t dlen) {
    if (strncmp(path, dir, dlen) != 0) {
        return false;
    }
    return path[dlen] == '/';
}

/* Renames path and, for a directory, everything under it, in both the
   store and the index. */
static void rename_tree(roc *m, const char *from, const char *to, bool dir) {
    size_t flen = strlen(from);
    size_t tlen = strlen(to);
    size_t i = 0;
    while (i < m->uf.nfiles) {
        char *p = m->uf.files[i].path;
        bool inside = false;
        if (dir) {
            inside = prefixed(p, from, flen);
        }
        if (strcmp(p, from) == 0 || inside) {
            char np[VFS_PATH_MAX];
            size_t rest = strlen(p) - flen;
            if (tlen + rest < sizeof(np)) {
                memcpy(np, to, tlen);
                memcpy(np + tlen, p + flen, rest + 1);
                memcpy(p, np, tlen + rest + 1);
            }
        }
        i++;
    }
    i = 0;
    while (i < m->fs.nnodes) {
        const vfs_node *n = &m->fs.nodes[i];
        bool inside = false;
        if (dir) {
            inside = prefixed(n->path, from, flen);
        }
        if (strcmp(n->path, from) != 0 && !inside) {
            i++;
            continue;
        }
        char np[VFS_PATH_MAX];
        size_t rest = strlen(n->path) - flen;
        if (tlen + rest >= sizeof(np)) {
            i++;
            continue;
        }
        memcpy(np, to, tlen);
        memcpy(np + tlen, n->path + flen, rest + 1);
        uint32_t size = n->size;
        bool isdir = n->dir;
        (void)vfs_remove(&m->fs, n->path); /* shifts the rest down: do not advance */
        (void)vfs_add_path(&m->fs, np, size, isdir);
    }
}

bool roc_cmd_mv(roc *m, const char *src, const char *dst) {
    if (!m->indexed) {
        term_puts(&m->t, "rocchetto: no index\r\n");
        return false;
    }
    if (src[0] == '\0' || dst[0] == '\0') {
        term_puts(&m->t, "usage: mv <file|dir> <file|dir>\r\n");
        return false;
    }
    char from[VFS_PATH_MAX];
    char to[VFS_PATH_MAX];
    if (!roc_resolve_arg(m, src, from, sizeof(from)) ||
        !target_path(m, "mv", from, dst, to, sizeof(to))) {
        return false;
    }
    const vfs_node *node = roc_lookup(m, from);
    if (node == NULL) {
        roc_err(m, "mv", src, "No such file or directory");
        return false;
    }
    bool dir = node->dir;
    const uint8_t *data = NULL;
    size_t len = 0;
    /* what moves has to be the user's: a file in the store (or on the
       host's storage), or a directory of the home; the site's and the
       board's answer as a mount would */
    bool on_disk = m->host.file_move != NULL;
    if (!under_home(m, from) || (!dir && !on_disk && !ufs_find(&m->uf, from, &data, &len))) {
        roc_err(m, "mv", src, "Read-only file system");
        return false;
    }
    if (!parent_ok(m, "mv", src, from) || !parent_ok(m, "mv", dst, to)) {
        return false;
    }
    size_t flen = strlen(from);
    if (strcmp(from, to) == 0 || (dir && strncmp(to, from, flen) == 0 && to[flen] == '/')) {
        roc_err(m, "mv", dst, "Invalid argument"); /* onto or into itself */
        return false;
    }
    const vfs_node *there = roc_lookup(m, to);
    if (there != NULL) {
        if (there->dir || dir) {
            roc_err(m, "mv", dst, there->dir ? "Is a directory" : "Not a directory");
            return false;
        }
        (void)ufs_remove(&m->uf, to);
        (void)vfs_remove(&m->fs, to);
    }
    if (on_disk && !m->host.file_move(m->host.ctx, from, to)) {
        roc_err(m, "mv", src, "Input/output error");
        return false;
    }
    rename_tree(m, from, to, dir);
    roc_home_changed(m);
    return true;
}

/* Said where the person will see it: on the line when at the prompt, as
   the note of the screen on top otherwise. */
static void say_here(roc *m, const char *msg) {
    if (m->mode != ROC_MODE_LINE) {
        roc_flash(m, msg);
        return;
    }
    term_puts(&m->t, "\r\x1b[K");
    term_puts(&m->t, msg);
    term_puts(&m->t, "\r\n");
    roc_line_repaint(m);
}

/* The name as the shell can type it: no slashes, no spaces, no control
   bytes; anything else, UTF-8 included, is the person's to choose. */
static bool clean_name(const char *name, char *out, size_t cap) {
    size_t n = strlen(name);
    if (n == 0 || n >= cap || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)name[i];
        out[i] = (c < 0x20 || c == 0x7f || c == '/' || c == ' ') ? '_' : (char)c;
        i++;
    }
    out[n] = '\0';
    return true;
}

static uint8_t upload_last[VFS_PATH_MAX]; /* the path being received */

static bool upload_refuse(roc *m, const char *name, const char *msg) {
    char buf[VFS_PATH_MAX + 96];
    roc_err_text(buf, sizeof(buf), "upload", name, msg);
    say_here(m, buf);
    return false;
}

bool roc_upload_begin(roc *m, const char *name, const char *dest, size_t size) {
    char base[128];
    char dir[VFS_PATH_MAX];
    if (!m->indexed || !clean_name(name, base, sizeof(base))) {
        return upload_refuse(m, name, "Invalid argument");
    }
    if (!upload_dest(m, dest, dir, sizeof(dir))) {
        return false;
    }
    char path[VFS_PATH_MAX];
    size_t dlen = strlen(dir);
    if (dlen + 1 + strlen(base) >= sizeof(path)) {
        return upload_refuse(m, name, "File name too long");
    }
    memcpy(path, dir, dlen);
    path[dlen] = '/';
    memcpy(path + dlen + 1, base, strlen(base) + 1);
    const vfs_node *have = roc_lookup(m, path);
    if (have != NULL && have->dir) {
        return upload_refuse(m, name, "Is a directory");
    }
    ufs_status st = ufs_begin(&m->uf, path, size);
    if (st != UFS_OK) {
        /* each one the truth: 512K a file, 2M the home, 64 files, one at a time */
        static const char *const why[] = {
            "", "File too large", "Disc quota exceeded", "File table overflow", "Device busy",
        };
        return upload_refuse(m, name, why[st]);
    }
    memcpy(upload_last, path, strlen(path) + 1);
    return true;
}

void roc_upload_data(roc *m, const uint8_t *data, size_t n) {
    if (!ufs_data(&m->uf, data, n)) {
        ufs_abort(&m->uf);
        (void)upload_refuse(m, (const char *)upload_last, "I/O error"); /* more than announced */
    }
}

void roc_upload_end(roc *m) {
    if (!m->uf.open) {
        return;
    }
    size_t len = m->uf.files[m->uf.nfiles].len;
    ufs_end(&m->uf);
    roc_ufs_stamp(m, (const char *)upload_last);
    (void)vfs_add(&m->fs, (const char *)upload_last, (uint32_t)len, false);
    roc_home_changed(m);
    char msg[VFS_PATH_MAX + 32];
    char size[8];
    size_t k = term_fmt_human(size, (uint32_t)len);
    size_t p = strlen((const char *)upload_last);
    memcpy(msg, "uploaded ", 9);
    memcpy(msg + 9, upload_last, p);
    memcpy(msg + 9 + p, " (", 2);
    memcpy(msg + 11 + p, size, k);
    memcpy(msg + 11 + p + k, ")", 2);
    say_here(m, msg);
}

void roc_upload_abort(roc *m) {
    ufs_abort(&m->uf);
}

/* ---- the shell's variables ---- */

/* The ones the session holds, not the table: read, never set. */
static bool readonly(const roc *m, const char *name) {
    static const char *const held[] = {"HOME", "USER", "PWD"};
    for (size_t i = 0; i < sizeof(held) / sizeof(held[0]); i++) {
        if (strcmp(name, held[i]) == 0) {
            return true;
        }
    }
    return sh_var_held(&m->vars, name);
}

/* The function running, and the words its parameters are in: in the list,
   or, for a $(...), in the line that called it. NULL when none runs. */
static const roc_frame *call_frame(const roc *m, const char **words) {
    for (int i = 0; i <= sub_depth; i++) {
        const roc_list *l = i == 0 ? &m->list : &sub_saved[sub_depth - i];
        for (int k = l->nframes - 1; k >= 0; k--) {
            if (l->frames[k].rw == SH_RW_CALL && !l->frames[k].seen_through) {
                *words = l->words + l->frames[k].words;
                return &l->frames[k];
            }
        }
    }
    return NULL;
}

/* $0 $1.. $# $@ $*: the function's parameters, none outside one. $@ has a
   \x1f between them, for the lexer to keep each a word. */
static const char *param(const roc *m, const char *name, size_t len) {
    const char *w = NULL;
    const roc_frame *f = call_frame(m, &w);
    uint32_t n = m->top_nparams; /* outside any call: set --'s */
    if (f == NULL) {
        w = m->top_params;
    } else {
        n = f->nwords - f->word;
        for (uint32_t i = 0; i < f->word; i++) {
            w += strlen(w) + 1; /* shifted out */
        }
    }
    if (name[0] == '#') {
        static char count[12]; /* apart: $@ is read with $# beside it */
        (void)snprintf(count, sizeof(count), "%u", (unsigned)n);
        return count;
    }
    if (name[0] == '@' || name[0] == '*') {
        static char text[SH_SCRIPT_MAX];
        const char *ifs = sh_var_get(&m->vars, "IFS", 3);
        char sep = ' ';
        if (name[0] == '@') {
            sep = '\x1f';
        } else if (ifs != NULL) {
            sep = ifs[0]; /* IFS empty: nothing between */
        }
        size_t k = 0;
        for (uint32_t i = 0; i < n; i++) {
            size_t wl = strlen(w);
            if (k + wl + 2 > sizeof(text)) {
                break;
            }
            if (i > 0 && sep != '\0') {
                text[k++] = sep;
            }
            memcpy(text + k, w, wl);
            k += wl;
            w += wl + 1;
        }
        text[k] = '\0';
        return text;
    }
    uint32_t at = 0;
    for (size_t i = 0; i < len; i++) {
        if (at > 100000) {
            return NULL;
        }
        at = (at * 10) + (uint32_t)(name[i] - '0');
    }
    if (at == 0) { /* the script's name, or the shell's */
        for (int i = 0; i <= sub_depth; i++) {
            const roc_list *l = i == 0 ? &m->list : &sub_saved[sub_depth - i];
            for (int k = l->nframes - 1; k >= 0; k--) {
                if (l->frames[k].script && !l->frames[k].seen_through) {
                    return l->words + l->frames[k].words;
                }
            }
        }
        return "rocchetto";
    }
    if (at > n) {
        return NULL;
    }
    for (uint32_t i = 1; i < at; i++) {
        w += strlen(w) + 1;
    }
    return w;
}

/* $NAME for sh_read: $?, the parameters, the session's HOME, USER and PWD,
   then the table. */
static const char *lookup(void *user, const char *name, size_t len) {
    roc *m = user;
    static char text[VFS_PATH_MAX];
    if (len == 1 && name[0] == '$') {
        return "1"; /* one shell per session: its number, for names like x.$$ */
    }
    if (len == 1 && name[0] == '!') { /* the last cmd &, run in the foreground */
        if (m->last_bg == 0) {
            return NULL;
        }
        (void)snprintf(text, sizeof(text), "%d", m->last_bg + 1);
        return text;
    }
    if (len == 1 && name[0] == '-') { /* the options set: e u x */
        size_t k = 0;
        if (m->opt_e) {
            text[k++] = 'e';
        }
        if (sh_nounset) {
            text[k++] = 'u';
        }
        if (m->opt_x) {
            text[k++] = 'x';
        }
        text[k] = '\0';
        return text;
    }
    if (len > 0 && strchr("#@*0123456789", name[0]) != NULL) {
        return param(m, name, len);
    }
    if (len == 1 && name[0] == '?') {
        (void)snprintf(text, sizeof(text), "%d", m->status);
        return text;
    }
    if (len == 4 && memcmp(name, "HOME", 4) == 0) {
        return roc_home_path(m, "", text, sizeof(text)) ? text : NULL;
    }
    if (len == 4 && memcmp(name, "USER", 4) == 0) {
        return m->user;
    }
    if (len == 3 && memcmp(name, "PWD", 3) == 0) {
        return m->cwd;
    }
    return sh_var_get(&m->vars, name, len);
}

/* ${name:=value}: the variable set as NAME=value would set it. */
static bool set_var(void *user, const char *name, size_t len, const char *value, char *why,
                    size_t cap) {
    roc *m = user;
    char word[SH_NAME_MAX + SH_VALUE_MAX + 1];
    (void)snprintf(word, sizeof(word), "%.*s=%s", (int)len, name, value);
    char plain[SH_NAME_MAX];
    (void)snprintf(plain, sizeof(plain), "%.*s", (int)len, name);
    if (readonly(m, plain)) {
        (void)snprintf(why, cap, "%s: readonly variable", plain);
        return false;
    }
    return sh_var_assign(&m->vars, word, why, cap);
}

static void assign(roc *m, const char *word) {
    const char *eq = strchr(word, '=');
    char name[SH_NAME_MAX];
    size_t n = (size_t)(eq - word) < sizeof(name) ? (size_t)(eq - word) : sizeof(name) - 1;
    memcpy(name, word, n);
    name[n] = '\0';
    if (readonly(m, name)) {
        roc_err(m, name, "", "readonly variable");
        return;
    }
    char why[48];
    if (!sh_var_assign(&m->vars, word, why, sizeof(why))) {
        roc_err(m, name, "", why);
    }
}

/* set alone: every variable, as sh lists them, NAME='value'. */
static void list_vars(roc *m) {
    for (int i = 0; i < m->vars.n; i++) {
        char *value[1] = {m->vars.v[i].value};
        char quoted[(4 * SH_VALUE_MAX) + 3];
        if (!sh_join(value, 1, quoted, sizeof(quoted))) {
            continue;
        }
        term_puts(&m->t, m->vars.v[i].name);
        term_puts(&m->t, "=");
        term_puts(&m->t, quoted);
        term_puts(&m->t, "\r\n");
    }
}

/* cat a b c: each file after the other, as cat does. Only files here (the
   board's, the home's): one of the site's is fetched when it is cat's only
   one, and a line cannot wait for several. */
static void cat_many(roc *m, const sh_line *sl) {
    for (int i = 1; i < sl->argc; i++) {
        char path[VFS_PATH_MAX];
        if (!roc_resolve_arg(m, sl->argv[i], path, sizeof(path))) {
            continue;
        }
        const vfs_node *node = roc_lookup(m, path);
        const uint8_t *data = NULL;
        size_t len = 0;
        if (node != NULL && node->dir) {
            roc_err(m, "cat", sl->argv[i], "Is a directory");
        } else if (roc_find_file(m, path, &data, &len)) {
            roc_show_bytes(m, false, path, data, len);
        } else if (node != NULL) {
            roc_err(m, "cat", sl->argv[i], "On the site, not here: cat it alone, or cp it home");
        } else {
            roc_err(m, "cat", sl->argv[i], "No such file or directory");
        }
    }
}

/* ---- test and [: what if and while ask ---- */

static bool test_number(roc *m, const char *s, long *v) {
    const char *p = s;
    bool neg = *p == '-';
    if (neg || *p == '+') {
        p++;
    }
    long n = 0;
    bool any = false;
    while (*p >= '0' && *p <= '9' && n < 100000000L) { /* 32 bits on wasm */
        n = (n * 10) + (*p - '0');
        p++;
        any = true;
    }
    if (!any || *p != '\0') {
        roc_err(m, "test", s, "Illegal number");
        m->status = 2;
        return false;
    }
    *v = neg ? -n : n;
    return true;
}

/* What a test says: true or false, or an operator it does not know, or an
   error already said. */
enum { T_FALSE, T_TRUE, T_BAD_OP, T_SAID };

static int truth(bool b) {
    if (b) {
        return T_TRUE;
    }
    return T_FALSE;
}

static int test_unary(roc *m, const char *op, const char *arg) {
    if (strcmp(op, "-n") == 0) {
        return truth(arg[0] != '\0');
    }
    if (strcmp(op, "-z") == 0) {
        return truth(arg[0] == '\0');
    }
    if (op[0] != '-' || op[1] == '\0' || op[2] != '\0' || strchr("efdsrwxhL", op[1]) == NULL) {
        return T_BAD_OP;
    }
    char path[VFS_PATH_MAX];
    if (!vfs_resolve(m->cwd, arg, path, sizeof(path))) {
        return T_FALSE;
    }
    const vfs_node *node = roc_lookup(m, path);
    const uint8_t *data = NULL;
    size_t len = 0;
    bool dir = false;
    bool file = false;
    if (node != NULL && node->dir) {
        dir = true;
    } else if (node != NULL) {
        file = true;
    } else {
        file = roc_find_file(m, path, &data, &len);
    }
    if (op[1] == 'd') {
        return truth(dir);
    }
    if (op[1] == 'f') {
        return truth(file);
    }
    if (op[1] == 'h' || op[1] == 'L') {
        return T_FALSE; /* no links here */
    }
    if (op[1] == 's') { /* not empty */
        if (data != NULL) {
            return truth(len > 0);
        }
        if (node == NULL || node->dir) {
            return T_FALSE;
        }
        return truth(node->size > 0);
    }
    if (op[1] == 'w') { /* what a user may write: the home */
        char home[VFS_PATH_MAX];
        if (!roc_home_path(m, "", home, sizeof(home)) || (!dir && !file)) {
            return T_FALSE;
        }
        size_t hn = strlen(home);
        return truth(strncmp(path, home, hn) == 0);
    }
    if (op[1] == 'x') { /* what runs: a directory to enter, a script */
        size_t pn = strlen(path);
        bool script = false;
        if (pn > 5 && strcmp(path + pn - 5, ".filo") == 0) {
            script = true;
        }
        if (pn > 3 && strcmp(path + pn - 3, ".sh") == 0) {
            script = true;
        }
        if (dir) {
            return T_TRUE;
        }
        if (!script) {
            return T_FALSE;
        }
        return truth(file);
    }
    if (dir) {
        return T_TRUE;
    }
    return truth(file);
}

static int test_binary(roc *m, const char *a, const char *op, const char *b) {
    if (strcmp(op, "=") == 0) {
        return truth(strcmp(a, b) == 0);
    }
    if (strcmp(op, "!=") == 0) {
        return truth(strcmp(a, b) != 0);
    }
    static const char *const nums[] = {"-eq", "-ne", "-lt", "-le", "-gt", "-ge"};
    int k = 0;
    while (k < 6 && strcmp(op, nums[k]) != 0) {
        k++;
    }
    if (k == 6) {
        return T_BAD_OP;
    }
    long x = 0;
    long y = 0;
    if (!test_number(m, a, &x) || !test_number(m, b, &y)) {
        return T_SAID;
    }
    long d = x - y;
    const bool r[] = {d == 0, d != 0, d < 0, d <= 0, d > 0, d >= 0};
    return truth(r[k]);
}

static bool is_binary_op(const char *w) {
    static const char *const ops[] = {"=", "!=", "-eq", "-ne", "-lt", "-le", "-gt", "-ge"};
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        if (strcmp(w, ops[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* test's words past four, or with -a -o ( ): ! over -a over -o, as the
   XSI extension reads them. */
typedef struct {
    roc *m;
    char *const *a;
    int n;
    int i;
    bool bad;
} test_parse;

static int t_or(test_parse *t);

static int t_primary(test_parse *t) {
    if (t->i >= t->n) {
        t->bad = true;
        return T_FALSE;
    }
    const char *w = t->a[t->i];
    if (strcmp(w, "!") == 0) {
        t->i++;
        int r = t_primary(t);
        if (r == T_TRUE || r == T_FALSE) {
            return truth(r == T_FALSE);
        }
        return r;
    }
    if (strcmp(w, "(") == 0) {
        t->i++;
        int r = t_or(t);
        if (t->i >= t->n || strcmp(t->a[t->i], ")") != 0) {
            t->bad = true;
            return T_FALSE;
        }
        t->i++;
        return r;
    }
    if (t->i + 2 < t->n && is_binary_op(t->a[t->i + 1])) {
        int r = test_binary(t->m, w, t->a[t->i + 1], t->a[t->i + 2]);
        t->i += 3;
        return r;
    }
    if (t->i + 2 == t->n && is_binary_op(t->a[t->i + 1])) {
        t->bad = true; /* x = with nothing after */
        return T_FALSE;
    }
    if (w[0] == '-' && w[1] != '\0' && w[2] == '\0' && t->i + 1 < t->n && strcmp(w, "-a") != 0 &&
        strcmp(w, "-o") != 0) {
        int r = test_unary(t->m, w, t->a[t->i + 1]);
        t->i += 2;
        return r;
    }
    t->i++;
    return truth(w[0] != '\0');
}

static int t_and(test_parse *t) {
    int r = t_primary(t);
    while (t->i < t->n && strcmp(t->a[t->i], "-a") == 0) {
        t->i++;
        int s = t_primary(t);
        if (r == T_TRUE) {
            r = s == T_TRUE ? T_TRUE : T_FALSE;
        }
    }
    return r;
}

static int t_or(test_parse *t) {
    int r = t_and(t);
    while (t->i < t->n && strcmp(t->a[t->i], "-o") == 0) {
        t->i++;
        int s = t_and(t);
        if (r == T_FALSE) {
            r = s == T_TRUE ? T_TRUE : T_FALSE;
        }
    }
    return r;
}

/* Whether the words want the parser: more than four, or -a -o ( in them. */
static bool test_long(char *const *a, int n) {
    if (n > 4) {
        return true;
    }
    for (int i = 0; i < n; i++) {
        if (strcmp(a[i], "-a") == 0 || strcmp(a[i], "-o") == 0 || strcmp(a[i], "(") == 0) {
            return n > 2; /* -a alone is a word, as is ( */
        }
    }
    return false;
}

/* test EXPR and [ EXPR ]: by the number of words, as POSIX says, up to
   four; past that, or with -a -o ( ), the parser. $? 0 true, 1 false, 2 an
   error. */
static void test_cmd(roc *m, const sh_line *sl) {
    if (sl->argc < 1 || sl->argc > SH_WORDS_MAX) {
        return;
    }
    int argc = sl->argc - 1;
    char *const *a = sl->argv + 1;
    if (strcmp(sl->argv[0], "[") == 0) {
        if (argc == 0 || strcmp(sl->argv[sl->argc - 1], "]") != 0) {
            roc_err(m, "[", "", "missing ]");
            m->status = 2;
            return;
        }
        argc--;
    }
    if (test_long(a, argc)) {
        test_parse t = {m, a, argc, 0, false};
        int r = t_or(&t);
        if (r == T_SAID) {
            return;
        }
        if (t.bad || t.i != t.n || r == T_BAD_OP) {
            roc_err(m, "test", "", "syntax error");
            m->status = 2;
            return;
        }
        m->status = r == T_TRUE ? 0 : 1;
        return;
    }
    /* ! before a question, but "! = x" asks whether ! is x */
    bool negate = false;
    if (argc >= 2 && strcmp(a[0], "!") == 0 && !(argc == 3 && is_binary_op(a[1]))) {
        negate = true;
        a++;
        argc--;
    }
    int r = T_BAD_OP;
    if (argc == 0) {
        r = T_FALSE;
    } else if (argc == 1) {
        r = truth(a[0][0] != '\0');
    } else if (argc == 2) {
        r = test_unary(m, a[0], a[1]);
    } else if (argc == 3) {
        r = test_binary(m, a[0], a[1], a[2]);
    }
    if (r == T_SAID) {
        return; /* and $? 2 */
    }
    if (r == T_BAD_OP) {
        roc_err(m, "test", argc > 1 ? a[1 % argc] : "", "unexpected operator");
        m->status = 2;
        return;
    }
    bool yes = false;
    if (r == T_TRUE) {
        yes = true;
    }
    m->status = 1;
    if (yes != negate) {
        m->status = 0;
    }
}

/* ---- read: a line into variables ---- */

/* Whether c separates the fields read reads: a byte of IFS (space, tab
   and newline when it is not set). white: one of its blanks. */
static bool read_sep(const char *ifs, char c, bool *white) {
    *white = false;
    if (c == ' ' || c == '\t' || c == '\n') {
        *white = true;
    }
    if (c == '\0') {
        return false;
    }
    return strchr(ifs, c) != NULL;
}

/* One line of data from *cur, as read reads it: split at IFS, a field
   for each name and the rest for the last; without raw a \ keeps the next
   byte (from splitting too) and \ newline goes on. 0 for a line, 1 at the
   end of the data (what was there still read). whole: the data is a line
   typed, its newline gone. */
static int read_into(roc *m, const char *names, bool raw, const uint8_t *data, size_t len,
                     size_t *cur, bool whole) {
    static char line[SH_VALUE_MAX - 1]; /* past what a variable holds: cut */
    static bool kept[sizeof(line)];     /* escaped: not a blank that splits */
    size_t n = 0;
    bool nl = whole;
    while (*cur < len) {
        char c = (char)data[*cur];
        (*cur)++;
        if (c == '\n') {
            nl = true;
            break;
        }
        bool esc = false;
        if (!raw && c == '\\' && *cur < len) {
            c = (char)data[*cur];
            (*cur)++;
            if (c == '\n') {
                continue;
            }
            esc = true;
        }
        if (c == '\r' && !esc) {
            continue;
        }
        if (n < sizeof(line)) {
            line[n] = c;
            kept[n] = esc;
            n++;
        }
    }
    const char *ifs = sh_var_get(&m->vars, "IFS", 3);
    if (ifs == NULL) {
        ifs = " \t\n";
    }
    bool white = false;
    size_t i = 0;
    const char *np = names;
    while (*np != '\0') {
        const char *ne = strchr(np, ' ');
        size_t nlen = ne != NULL ? (size_t)(ne - np) : strlen(np);
        while (i < n && !kept[i] && read_sep(ifs, line[i], &white) && white) {
            i++;
        }
        size_t from = i;
        size_t to = n;
        if (ne != NULL) { /* not the last: one field, and the break after it */
            while (i < n && (kept[i] || !read_sep(ifs, line[i], &white))) {
                i++;
            }
            to = i;
            bool other = false; /* the byte of IFS no blank that ends it, one */
            while (i < n && !kept[i] && read_sep(ifs, line[i], &white) && (white || !other)) {
                if (!white) {
                    other = true;
                }
                i++;
            }
        } else {
            while (to > from && !kept[to - 1] && read_sep(ifs, line[to - 1], &white) && white) {
                to--;
            }
        }
        char word[SH_NAME_MAX + SH_VALUE_MAX + 2];
        size_t vlen = to - from;
        (void)snprintf(word, sizeof(word), "%.*s=%.*s", (int)nlen, np, (int)vlen, line + from);
        assign(m, word);
        np = ne != NULL ? ne + 1 : np + nlen;
    }
    return nl ? 0 : 1;
}

/* read [-r] name...: a line of the block's < or |, the command's own, or
   one typed at the prompt (the line waits for it). */
static void read_cmd(roc *m, const sh_line *sl) {
    bool raw = false;
    int i = 1;
    if (i < sl->argc && strcmp(sl->argv[i], "-r") == 0) {
        raw = true;
        i++;
    }
    char names[SH_LINE_MAX];
    size_t k = 0;
    if (i == sl->argc) {
        roc_err(m, "read", "", "a name is expected");
        m->status = 2;
        return;
    }
    for (; i < sl->argc; i++) {
        size_t n = strlen(sl->argv[i]);
        if (!sh_is_name(sl->argv[i], n) || k + n + 2 > sizeof(names)) {
            roc_err(m, "read", sl->argv[i], "bad variable name");
            m->status = 2;
            return;
        }
        if (k > 0) {
            names[k++] = ' ';
        }
        memcpy(names + k, sl->argv[i], n);
        k += n;
    }
    names[k] = '\0';
    if (m->piped) { /* a | or < of its own */
        if (roc_input_spooled(m, "read")) {
            return;
        }
        size_t cur = 0;
        m->status = read_into(m, names, raw, m->in, m->in_len, &cur, false);
        return;
    }
    if (m->nbins > 0) {
        size_t start = m->bins[m->nbins - 1].start;
        size_t len = m->bins[m->nbins - 1].len;
        size_t *cur = &m->bins[m->nbins - 1].cursor;
        if (*cur >= len) {
            m->status = read_into(m, names, raw, m->bin + start, 0, cur, false);
            return;
        }
        m->status = read_into(m, names, raw, m->bin + start, len, cur, false);
        return;
    }
    if (sub_depth > 0) {
        m->status = read_into(m, names, raw, (const uint8_t *)"", 0, &k, false);
        return;
    }
    memcpy(m->read_names, names, k + 1);
    m->read_raw = raw;
    m->read_wait = true; /* roc_read_answer fills them */
}

bool roc_host_command(const roc *m, const char *name, size_t len) {
    const char *p = m->host.commands != NULL ? m->host.commands : "";
    while (*p != '\0') {
        size_t n = strcspn(p, " ");
        if (n == len && n > 0 && memcmp(p, name, n) == 0) {
            return true;
        }
        p += n + strspn(p + n, " ");
    }
    return false;
}

static bool host_run(roc *m, const sh_line *sl) {
    if (m->host.run == NULL || roc_output_captured(m) ||
        !m->host.run(m->host.ctx, sl->argc, sl->argv)) {
        return false;
    }
    m->hosted = true;
    return true;
}

void roc_run_done(roc *m, int status) {
    if (!m->hosted) {
        return;
    }
    m->hosted = false;
    m->status = status;
    roc_prompt(m); /* or the rest of the line it was in */
}

void roc_read_answer(roc *m, const char *line, bool eof) {
    m->read_wait = false;
    size_t cur = 0;
    size_t len = strlen(line);
    bool whole = true; /* a line typed: its newline was the Enter */
    if (eof) {
        len = 0;
        whole = false;
    }
    m->status = read_into(m, m->read_names, m->read_raw, (const uint8_t *)line, len, &cur, whole);
}

/* set -- words: the parameters, of the function or script running (when
   nothing it opened keeps words after them), or of the shell. */
static void set_params(roc *m, char *const *words, int n) {
    roc_list *l = &m->list;
    int k = l->nframes - 1;
    while (k >= 0 && (l->frames[k].rw != SH_RW_CALL || l->frames[k].seen_through)) {
        k--;
    }
    if (k < 0) {
        size_t used = 0;
        uint32_t count = 0;
        for (int i = 0; i < n; i++) {
            size_t len = strlen(words[i]) + 1;
            if (used + len > sizeof(m->top_params)) {
                roc_err(m, "set", "", "too many words");
                return;
            }
            memcpy(m->top_params + used, words[i], len);
            used += len;
            count++;
        }
        m->top_params_len = used;
        m->top_nparams = count;
        return;
    }
    roc_frame *f = &l->frames[k];
    const char *w = l->words + f->words;
    size_t own = 0;
    for (uint32_t i = 0; i < f->nwords; i++) {
        own += strlen(w + own) + 1;
    }
    size_t zero = strlen(w) + 1; /* its $0 stays */
    size_t fresh = zero;
    for (int i = 0; i < n; i++) {
        fresh += strlen(words[i]) + 1;
    }
    /* the words of the for and case opened in it come after its own: moved */
    size_t old_end = f->words + own;
    size_t after = l->words_used - old_end;
    if (f->words + fresh + after > sizeof(l->words)) {
        roc_err(m, "set", "", "too many words");
        return;
    }
    memmove(l->words + f->words + fresh, l->words + old_end, after);
    for (int j = k + 1; j < l->nframes; j++) {
        int rw = l->frames[j].rw;
        if (rw == SH_RW_FOR || rw == SH_RW_CASE || rw == SH_RW_CALL) {
            l->frames[j].words = l->frames[j].words - own + fresh;
        }
    }
    size_t at = f->words + zero;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(words[i]) + 1;
        memcpy(l->words + at, words[i], len);
        at += len;
    }
    l->words_used = f->words + fresh + after;
    f->nwords = (uint32_t)n + 1;
    f->word = 1;
}

/* set -e -u -x (+ turns them off), set -- words, set words. */
static void set_cmd(roc *m, const sh_line *sl) {
    for (int i = 1; i < sl->argc; i++) {
        const char *w = sl->argv[i];
        if (strcmp(w, "--") == 0) {
            set_params(m, sl->argv + i + 1, sl->argc - i - 1);
            return;
        }
        if ((w[0] != '-' && w[0] != '+') || w[1] == '\0') {
            set_params(m, sl->argv + i, sl->argc - i);
            return;
        }
        bool on = w[0] == '-';
        for (const char *p = w + 1; *p != '\0'; p++) {
            if (*p == 'e') {
                m->opt_e = on;
            } else if (*p == 'u') {
                sh_nounset = on;
            } else if (*p == 'x') {
                m->opt_x = on;
            } else {
                roc_err(m, "set", w, "bad option (e, u and x are)");
                m->status = 2;
                return;
            }
        }
    }
}

/* local name[=value]...: the variable as it was before comes back when the
   function returns (roc_local_restore, from pop). */
static void local_cmd(roc *m, const sh_line *sl) {
    const roc_list *l = &m->list;
    int k = l->nframes - 1;
    while (k >= 0 && (l->frames[k].rw != SH_RW_CALL || l->frames[k].seen_through)) {
        k--;
    }
    if (k < 0) {
        roc_err(m, "local", "", "only in a function");
        return;
    }
    for (int i = 1; i < sl->argc; i++) {
        const char *w = sl->argv[i];
        const char *eq = strchr(w, '=');
        size_t n = eq != NULL ? (size_t)(eq - w) : strlen(w);
        if (!sh_is_name(w, n) || n >= SH_NAME_MAX) {
            roc_err(m, "local", w, "bad variable name");
            continue;
        }
        if (m->nlocals == ROC_CFG_LOCALS) {
            roc_err(m, "local", w, "too many locals");
            return;
        }
        roc_local *s = &m->locals[m->nlocals];
        memcpy(s->name, w, n);
        s->name[n] = '\0';
        const char *old = sh_var_get(&m->vars, w, n);
        s->was_set = old != NULL;
        (void)snprintf(s->value, sizeof(s->value), "%s", old != NULL ? old : "");
        s->frame = k;
        m->nlocals++;
        if (eq != NULL) {
            assign(m, w);
        }
    }
}

/* A function's frame k goes: its locals as they were before it. */
static void locals_restore(roc *m, int k) {
    while (m->nlocals > locals_floor && m->locals[m->nlocals - 1].frame >= k) {
        const roc_local *s = &m->locals[--m->nlocals];
        if (s->was_set) {
            char word[SH_NAME_MAX + SH_VALUE_MAX + 1];
            (void)snprintf(word, sizeof(word), "%s=%s", s->name, s->value);
            char why[48];
            (void)sh_var_assign(&m->vars, word, why, sizeof(why));
        } else {
            sh_var_unset(&m->vars, s->name);
        }
    }
}

/* getopts optstring name [arg...]: the next option of the words (the
   parameters when none), in name; OPTARG its argument, OPTIND where the
   next word is. $? 1 when the options end. */
static void getopts_cmd(roc *m, const sh_line *sl) {
    if (sl->argc < 3) {
        roc_err(m, "getopts", "", "usage: getopts optstring name [arg...]");
        m->status = 2;
        return;
    }
    const char *spec = sl->argv[1];
    const char *name = sl->argv[2];
    bool silent = spec[0] == ':';
    static char *args[SH_WORDS_MAX];
    int nargs = 0;
    if (sl->argc > 3) {
        for (int i = 3; i < sl->argc; i++) {
            args[nargs++] = sl->argv[i];
        }
    } else {
        static char params[SH_SCRIPT_MAX];
        const char *all = param(m, "@", 1);
        const char *count = param(m, "#", 1);
        (void)snprintf(params, sizeof(params), "%s", all);
        if (strcmp(count, "0") != 0) {
            char *p = params;
            while (nargs < SH_WORDS_MAX) {
                args[nargs++] = p;
                char *cut = strchr(p, '\x1f');
                if (cut == NULL) {
                    break;
                }
                *cut = '\0';
                p = cut + 1;
            }
        }
    }
    const char *ind_text = sh_var_get(&m->vars, "OPTIND", 6);
    int ind = 1;
    if (ind_text != NULL) {
        ind = 0;
        for (const char *d = ind_text; *d >= '0' && *d <= '9' && ind < 100000; d++) {
            ind = (ind * 10) + (*d - '0');
        }
    }
    if (ind != m->getopts_ind || ind < 1) { /* OPTIND set anew: from a word's start */
        m->getopts_sub = 1;
    }
    char word[SH_NAME_MAX + SH_VALUE_MAX + 1];
    char opt[2] = {'?', '\0'};
    bool done = false;
    const char *arg = NULL;
    if (ind >= 1 && ind <= nargs) {
        arg = args[ind - 1];
    }
    if (arg == NULL || (m->getopts_sub == 1 && (arg[0] != '-' || arg[1] == '\0'))) {
        done = true;
    } else if (m->getopts_sub == 1 && strcmp(arg, "--") == 0) {
        ind++;
        done = true;
    }
    sh_var_unset(&m->vars, "OPTARG");
    if (!done) {
        char c = arg[m->getopts_sub++];
        const char *at = c != ':' ? strchr(spec, c) : NULL;
        opt[0] = c;
        if (at == NULL) {
            if (silent) {
                (void)snprintf(word, sizeof(word), "OPTARG=%c", c);
                assign(m, word);
            } else {
                const char bad[2] = {c, '\0'};
                roc_err(m, "getopts", bad, "illegal option");
            }
            opt[0] = '?';
        } else if (at[1] == ':') { /* it takes an argument */
            const char *value = NULL;
            if (arg[m->getopts_sub] != '\0') {
                value = arg + m->getopts_sub;
                ind++;
            } else if (ind < nargs) {
                value = args[ind];
                ind += 2;
            }
            m->getopts_sub = 1;
            if (value != NULL) {
                (void)snprintf(word, sizeof(word), "OPTARG=%s", value);
                assign(m, word);
            } else if (silent) {
                opt[0] = ':';
                (void)snprintf(word, sizeof(word), "OPTARG=%c", c);
                assign(m, word);
            } else {
                const char bad[2] = {c, '\0'};
                roc_err(m, "getopts", bad, "option requires an argument");
                opt[0] = '?';
            }
        }
        if (m->getopts_sub > 1 && arg[m->getopts_sub] == '\0') {
            ind++;
            m->getopts_sub = 1;
        }
    }
    (void)snprintf(word, sizeof(word), "%s=%s", name, opt);
    assign(m, word);
    (void)snprintf(word, sizeof(word), "OPTIND=%d", ind);
    assign(m, word);
    m->getopts_ind = ind;
    m->status = done ? 1 : 0;
}

/* sleep seconds (a fraction too: 0.5): the line waits that long, the tick
   counting it down; ^C ends it. */
static void sleep_cmd(roc *m, const sh_line *sl) {
    const char *s = sl->argc == 2 ? sl->argv[1] : "";
    uint32_t whole = 0;
    uint32_t ms = 0;
    uint32_t scale = 100;
    bool any = false;
    while (*s >= '0' && *s <= '9' && whole < 100000) {
        whole = (whole * 10) + (uint32_t)(*s++ - '0');
        any = true;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            ms += (uint32_t)(*s++ - '0') * scale;
            scale /= 10;
            any = true;
        }
    }
    if (!any || *s != '\0') {
        roc_err(m, "sleep", sl->argc > 1 ? sl->argv[1] : "", "usage: sleep <seconds>");
        m->status = 2;
        return;
    }
    if (whole > 3600) { /* yagni: an hour at most, one is plenty for a board */
        whole = 3600;
    }
    m->sleep_ms = (whole * 1000U) + ms;
}

/* trap [action condition...]: EXIT (0) runs action when the shell, or the
   script it is in, ends; INT (2) after a ^C stops a line. - forgets one,
   no arguments lists them. HUP and TERM are taken and never come. */
static void trap_cmd(roc *m, const sh_line *sl) {
    if (sl->argc == 1) {
        const char *names[] = {"EXIT", "INT"};
        const char *texts[] = {m->trap_exit, m->trap_int};
        for (int i = 0; i < 2; i++) {
            if (texts[i][0] == '\0') {
                continue;
            }
            char *word[1] = {(char *)texts[i]};
            char quoted[(4 * SH_LINE_MAX) + 3];
            if (sh_join(word, 1, quoted, sizeof(quoted))) {
                term_puts(&m->t, "trap -- ");
                term_puts(&m->t, quoted);
                term_puts(&m->t, " ");
                term_puts(&m->t, names[i]);
                term_puts(&m->t, "\r\n");
            }
        }
        return;
    }
    const char *action = sl->argv[1];
    bool forget = strcmp(action, "-") == 0;
    int first = 2;
    if (sl->argc == 2) { /* trap EXIT alone: forget it, as POSIX reads it */
        forget = true;
        first = 1;
    }
    for (int i = first; i < sl->argc; i++) {
        const char *cond = sl->argv[i];
        char *slot = m->trap_exit;
        if (strcmp(cond, "INT") == 0 || strcmp(cond, "SIGINT") == 0 || strcmp(cond, "2") == 0) {
            slot = m->trap_int;
        } else if (strcmp(cond, "HUP") == 0 || strcmp(cond, "TERM") == 0 ||
                   strcmp(cond, "1") == 0 || strcmp(cond, "15") == 0) {
            continue; /* taken, never sent here */
        } else if (strcmp(cond, "EXIT") != 0 && strcmp(cond, "0") != 0) {
            roc_err(m, "trap", cond, "bad trap");
            continue;
        }
        if (forget) {
            slot[0] = '\0';
        } else if (strlen(action) >= SH_LINE_MAX) {
            roc_err(m, "trap", cond, "action too long");
        } else {
            memcpy(slot, action, strlen(action) + 1);
        }
    }
}

/* #! and an sh: the interpreter's last part is sh, or env runs sh. */
static bool shebang_sh(const uint8_t *head, size_t n) {
    if (n < 3 || head[0] != '#' || head[1] != '!') {
        return false;
    }
    size_t i = 2;
    for (int word = 0; word < 2; word++) {
        while (i < n && (head[i] == ' ' || head[i] == '\t')) {
            i++;
        }
        size_t from = i;
        while (i < n && head[i] != ' ' && head[i] != '\t' && head[i] != '\n' && head[i] != '\r') {
            i++;
        }
        size_t base = from;
        for (size_t k = from; k < i; k++) {
            if (head[k] == '/') {
                base = k + 1;
            }
        }
        if (i - base == 2 && memcmp(head + base, "sh", 2) == 0) {
            return true;
        }
        if (word > 0 || i - base != 3 || memcmp(head + base, "env", 3) != 0) {
            return false;
        }
    }
    return false;
}

int roc_exec_kind(roc *m, const char *path, bool bring) {
    roc_stat st;
    roc_origin from = ROC_FROM_HOME;
    if (!roc_stat_path(m, path, true, &st, &from) || st.dir || (st.away && !bring) ||
        from == ROC_FROM_SITE) {
        return 0;
    }
    uint8_t head[64];
    size_t n = 0;
    void *h = NULL;
    uint64_t size = 0;
    int r = ROC_HOST_NOT_MINE;
    if (m->host.fh_open != NULL) {
        r = m->host.fh_open(m->host.ctx, path, ROC_FH_READ, &h, &size);
    }
    const uint8_t *data = NULL;
    size_t len = 0;
    if (r == ROC_HOST_YES) { /* the first bytes only, however large the file */
        long long got = m->host.fh_read(m->host.ctx, h, 0, head, sizeof(head));
        (void)m->host.fh_close(m->host.ctx, h, false);
        n = got > 0 ? (size_t)got : 0;
    } else if (r == ROC_HOST_NOT_MINE && roc_find_file(m, path, &data, &len)) {
        n = len < sizeof(head) ? len : sizeof(head);
        memcpy(head, data, n);
    }
    if (roc_is_program(head, n)) {
        return 'p';
    }
    return shebang_sh(head, n) ? 's' : 0;
}

/* The shell's own: they change the shell itself (its directory, its
   variables, its flow), so no program can be one, and PATH is not looked
   at for them (POSIX's special and intrinsic utilities); help adds what
   the app has. Every other command is looked for in PATH first, and the
   shell's table after it: a command of the user's goes over the shell's. */
static bool intrinsic(const char *name) {
    static const char *const own[] = {
        "break", "continue", "return", "exit",   "eval",     ".",     ":",
        "set",   "shift",    "unset",  "export", "readonly", "local", "getopts",
        "trap",  "command",  "type",   "read",   "cd",       "alias", "unalias",
        "wait",  "true",     "false",  "logout", "sh",       "help",  "?",
    };
    for (size_t i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
        if (strcmp(name, own[i]) == 0) {
            return true;
        }
    }
    return false;
}

int roc_path_find(roc *m, const char *name, char *out, size_t cap) {
    if (name[0] == '\0' || strchr(name, '/') != NULL) {
        return 0;
    }
    const char *p = sh_var_get(&m->vars, "PATH", 4);
    if (p == NULL) {
        p = "/bin"; /* unset: the default, as a shell has one */
    }
    while (true) {
        size_t n = strcspn(p, ":");
        char dir[VFS_PATH_MAX];
        char want[VFS_PATH_MAX];
        /* an empty entry is the directory you are in, as sh reads it */
        (void)snprintf(dir, sizeof(dir), "%.*s", n == 0 ? 1 : (int)n, n == 0 ? "." : p);
        if (snprintf(want, sizeof(want), "%s/%s", dir, name) < (int)sizeof(want) &&
            vfs_resolve(m->cwd, want, out, cap)) {
            int kind = roc_exec_kind(m, out, true);
            if (kind != 0) {
                return kind;
            }
        }
        if (p[n] == '\0') {
            return 0;
        }
        p += n + 1;
    }
}

static int func_find(const roc *m, const char *name);

/* What name is to the shell, in the order a line looks: 'k' a reserved
   word, 'f' a function, 'b' a builtin of its own, 'p' a program or a
   script in PATH (path set), 'b' a command of its table, 'h' the host's,
   0 nothing. */
static char what_is(roc *m, const char *name, char *path, size_t cap) {
    static const char *const builtins[] = {
        "test", "[", "echo", "printf", "sleep", "edit",
    };
    size_t len = 0;
    if (sh_var_get(&m->aliases, name, strlen(name)) != NULL) {
        return 'a';
    }
    if (sh_reserved(name, &len) != SH_RW_NONE && name[len] == '\0') {
        return 'k';
    }
    if (func_find(m, name) >= 0) {
        return 'f';
    }
    if (intrinsic(name)) {
        return 'b';
    }
    if (roc_path_find(m, name, path, cap) != 0) {
        return 'p';
    }
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        if (strcmp(name, builtins[i]) == 0) {
            return 'b';
        }
    }
    for (size_t i = 0; i < roc_command_count(); i++) {
        if (strcmp(name, roc_command_name(i)) == 0 && roc_command_here(m, name)) {
            return 'b';
        }
    }
    if (roc_host_command(m, name, strlen(name))) {
        return 'h';
    }
    return 0;
}

/* command -v / -V name... and type name...: what each name is. */
static void type_cmd(roc *m, const sh_line *sl, bool terse, int first) {
    m->status = 0;
    for (int i = first; i < sl->argc; i++) {
        const char *name = sl->argv[i];
        char path[VFS_PATH_MAX];
        char kind = what_is(m, name, path, sizeof(path));
        if (kind == 0) {
            if (!terse) {
                term_puts(&m->t, name);
                term_puts(&m->t, ": not found\r\n");
            }
            m->status = 1;
            continue;
        }
        const char *value = sh_var_get(&m->aliases, name, strlen(name));
        if (kind == 'a' && value != NULL) { /* as sh shows it: alias name='value' */
            const char *words[1] = {value};
            char quoted[(2 * SH_VALUE_MAX) + 8];
            if (!sh_join((char *const *)words, 1, quoted, sizeof(quoted))) {
                quoted[0] = '\0';
            }
            term_puts(&m->t, terse ? "alias " : "");
            term_puts(&m->t, name);
            term_puts(&m->t, terse ? "=" : " is an alias for ");
            term_puts(&m->t, terse ? quoted : value);
            term_puts(&m->t, "\r\n");
            continue;
        }
        term_puts(&m->t, terse && kind != 'p' ? name : "");
        if (terse) {
            term_puts(&m->t, kind == 'p' ? path : "");
            term_puts(&m->t, "\r\n");
            continue;
        }
        term_puts(&m->t, name);
        if (kind == 'k') {
            term_puts(&m->t, " is a shell keyword\r\n");
        } else if (kind == 'f') {
            term_puts(&m->t, " is a function\r\n");
        } else if (kind == 'b') {
            term_puts(&m->t, " is a shell builtin\r\n");
        } else if (kind == 'h') {
            term_puts(&m->t, " is a command of the app the shell runs in\r\n");
        } else {
            term_puts(&m->t, " is ");
            term_puts(&m->t, path);
            term_puts(&m->t, "\r\n");
        }
    }
}

static void func_remove(roc *m, int i);
static void shift_params(roc *m, const sh_line *sl);

/* One command of a list, read and expanded: its assignments, where its
   output goes, and the command. */
enum { REST_MAX = (16 * SH_LINE_MAX) + 1 };
static void run_named(roc *m, const sh_line *sl, char *rest);

/* cppcheck-suppress constParameterCallback ; sh_alias's shape */
static const char *alias_find(void *user, const char *name, size_t len) {
    const roc *m = user;
    return sh_var_get(&m->aliases, name, len);
}

static void alias_show(roc *m, const sh_var *v) {
    const char *words[1] = {v->value};
    char quoted[(2 * SH_VALUE_MAX) + 8];
    if (!sh_join((char *const *)words, 1, quoted, sizeof(quoted))) {
        quoted[0] = '\0';
    }
    term_puts(&m->t, v->name);
    term_puts(&m->t, "=");
    term_puts(&m->t, quoted);
    term_puts(&m->t, "\r\n");
}

/* alias [name[=value]...]: kept, shown, or all of them shown. */
static void alias_cmd(roc *m, const sh_line *sl) {
    for (int i = 0; sl->argc == 1 && i < m->aliases.n; i++) {
        alias_show(m, &m->aliases.v[i]);
    }
    for (int i = 1; i < sl->argc; i++) {
        const char *w = sl->argv[i];
        const char *eq = strchr(w, '=');
        char why[48];
        if (eq != NULL && !sh_var_assign(&m->aliases, w, why, sizeof(why))) {
            roc_err(m, "alias", w, why);
            continue;
        }
        if (eq != NULL) {
            continue;
        }
        bool found = false;
        for (int k = 0; k < m->aliases.n; k++) {
            if (strcmp(m->aliases.v[k].name, w) == 0) {
                alias_show(m, &m->aliases.v[k]);
                found = true;
            }
        }
        if (!found) {
            roc_err(m, "alias", w, "not found");
        }
    }
}

/* unalias name... or unalias -a: forgotten. */
static void unalias_cmd(roc *m, const sh_line *sl) {
    if (sl->argc == 2 && strcmp(sl->argv[1], "-a") == 0) {
        m->aliases.n = 0;
        return;
    }
    for (int i = 1; i < sl->argc; i++) {
        if (sh_var_get(&m->aliases, sl->argv[i], strlen(sl->argv[i])) == NULL) {
            roc_err(m, "unalias", sl->argv[i], "not found");
            continue;
        }
        sh_var_unset(&m->aliases, sl->argv[i]);
    }
}

static void run_command(roc *m, const sh_line *sl) {
    static char rest[REST_MAX];
    m->status = 0;
    /* the output first, then the errors, then the input, so cat < f > f
       reads the emptied file, as sh does */
    if (sl->out != NULL && !roc_capture_begin(m, sl->out, sl->append)) {
        return;
    }
    if (sl->out == NULL && sl->out_to_err) {
        roc_pipe_begin(m);
        m->cap.pipe = false;
        m->cap.to_err = true;
    }
    if ((sl->err != NULL || sl->err_to_out) && !err_begin(m, sl)) {
        return;
    }
    if (sl->in != NULL && !read_input(m, sl->in)) {
        return;
    }
    if (sl->here != NULL) { /* cmd <<WORD: its text the input, as a | hands it over */
        size_t n = sl->here_len < sizeof(m->pipe_buf) ? sl->here_len : sizeof(m->pipe_buf);
        memcpy(m->pipe_buf, sl->here, n);
        m->in = m->pipe_buf;
        m->in_len = n;
        m->piped = true;
    }
    if (sl->argc == 0) {
        /* NAME=value alone: the shell's; before a command, the command's
           only, and programs have no environment yet to see it in */
        if (sub_ran) {
            m->status = sub_status;
        }
        for (int i = 0; i < sl->nassign; i++) {
            assign(m, sl->assign[i]);
        }
        return; /* > file alone: the file, made or emptied */
    }
    /* what a command reads as its arguments: the words after its name, in
       the shape sh_split reads back into the same words */
    if (!sh_join(sl->argv + 1, sl->argc - 1, rest, REST_MAX)) {
        term_puts(&m->t, "rocchetto: Line too long\r\n");
        return;
    }
    /* NAME=value before it: set while it runs (IFS= read -r l), then as
       it was; a command that waits sees it only as it starts */
    /* exported while it runs, as POSIX says of these; as many as the shell
       keeps variables, a line with more refused rather than cut */
    enum { PREFIX_MAX = SH_VARS_MAX };
    if (sl->nassign > PREFIX_MAX) {
        roc_err(m, sl->argv[0], "", "too many assignments before a command");
        return;
    }
    struct {
        char word[SH_NAME_MAX + SH_VALUE_MAX + 1];
        bool was_set;
        bool was_exported;
    } before[PREFIX_MAX];
    int nbefore = 0;
    for (int i = 0; i < sl->nassign; i++) {
        const char *w = sl->assign[i];
        size_t len = (size_t)(strchr(w, '=') - w);
        const char *old = sh_var_get(&m->vars, w, len);
        before[nbefore].was_set = old != NULL;
        (void)snprintf(before[nbefore].word, sizeof(before[nbefore].word), "%.*s=%s", (int)len, w,
                       old != NULL ? old : "");
        char name[SH_NAME_MAX];
        (void)snprintf(name, sizeof(name), "%.*s", (int)len, w);
        before[nbefore].was_exported = sh_var_exported(&m->vars, name);
        nbefore++;
        assign(m, w);
        char why[48];
        (void)sh_var_export(&m->vars, name, true, why, sizeof(why));
    }
    run_named(m, sl, rest);
    int status = m->status;
    for (int i = nbefore - 1; i >= 0; i--) {
        char *eq = strchr(before[i].word, '=');
        if (before[i].was_set) {
            assign(m, before[i].word);
            *eq = '\0';
            char why[48];
            (void)sh_var_export(&m->vars, before[i].word, before[i].was_exported, why, sizeof(why));
        } else {
            *eq = '\0';
            sh_var_unset(&m->vars, before[i].word);
        }
    }
    m->status = status;
}

/* export -p: each exported variable as a line the shell reads back. */
static void export_list(roc *m) {
    for (int i = 0; i < m->vars.n; i++) {
        const sh_var *v = &m->vars.v[i];
        if (!v->exported) {
            continue;
        }
        char *const word[1] = {(char *)v->value};
        char quoted[(SH_VALUE_MAX * 4) + 8];
        if (!sh_join(word, 1, quoted, sizeof(quoted))) {
            continue;
        }
        char line[SH_NAME_MAX + sizeof(quoted) + 16];
        int n = snprintf(line, sizeof(line), "export %s=%s\n", v->name,
                         v->value[0] != '\0' ? quoted : "''");
        if (n > 0) {
            roc_out(m, (const uint8_t *)line, (size_t)n);
        }
    }
}

/* A command with a name, its words after it joined in rest. */
static void run_named(roc *m, const sh_line *sl, char *rest) {
    const char *cmd = sl->argv[0];
    const char *first = sl->argc > 1 ? sl->argv[1] : "";
    /* PATH before the table: the user's command over the shell's */
    if (strchr(cmd, '/') == NULL && !intrinsic(cmd) && script_run(m, cmd, rest)) {
        return;
    }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        /* the app's own commands: under the pager, there when it closes;
           after the text when a | or a > takes it */
        bool app = false;
        if (m->host.commands != NULL) {
            app = m->host.commands[0] != '\0';
        }
        bool paged = roc_out_terminal(m);
        if (app && paged) {
            term_puts(&m->t, "from the app too: ");
            term_puts(&m->t, m->host.commands);
            term_puts(&m->t, "\r\n");
        }
        (void)script_run(m, "help", rest);
        if (app && !paged) {
            roc_out(m, (const uint8_t *)"\nand from the app: ", 19);
            roc_out(m, (const uint8_t *)m->host.commands, strlen(m->host.commands));
            roc_out(m, (const uint8_t *)"\n", 1);
        }
    } else if (strcmp(cmd, "filo") == 0) {
        if (sl->argc == 1) {
            script_repl_begin(m); /* no file: the language itself */
            return;
        }
#if ROC_APP_TOOLS
        if (roc_filo_tool(m, rest)) {
            return; /* one of filo's tools: build, dump, decompile... */
        }
#endif
        /* filo FILE [ARGS]: the file, and the words after it its arguments */
        if (!sh_join(sl->argv + 2, sl->argc - 2, rest, REST_MAX)) {
            term_puts(&m->t, "rocchetto: Line too long\r\n");
            return;
        }
        script_run_path(m, first, rest);
    } else if (strchr(cmd, '/') != NULL) {
        script_exec_path(m, cmd, rest); /* ./name, /bin/name: a program by its bytes */
#if ROC_APP_BOARD
    } else if (strcmp(cmd, "articles") == 0 && roc_command_here(m, cmd)) {
        articles(m, sl);
#endif
    } else if (strcmp(cmd, "cd") == 0 || strcmp(cmd, "cat") == 0 || strcmp(cmd, "less") == 0 ||
               strcmp(cmd, "more") == 0 || strcmp(cmd, "ed") == 0) {
        if (sl->argc > 2 && strcmp(cmd, "cat") == 0) {
            cat_many(m, sl);
            return;
        }
        if (sl->argc > 2) {
            roc_err(m, cmd, sl->argv[2], "too many arguments");
            return;
        }
        bool reads = false; /* cat, less and more read what a | hands over */
        if (strcmp(cmd, "cd") != 0 && strcmp(cmd, "ed") != 0) {
            reads = true;
        }
        if (reads && sl->argc == 1 && m->piped) {
            if (m->in_spool != NULL && strcmp(cmd, "cat") == 0) {
                cat_spooled(m);
                return;
            }
            if (roc_input_spooled(m, cmd)) {
                return;
            }
            roc_show_bytes(m, strcmp(cmd, "cat") != 0, "-", m->in, m->in_len);
            return;
        }
        if (strcmp(cmd, "cd") == 0) {
            roc_cmd_cd(m, first);
        } else if (strcmp(cmd, "cat") == 0) {
            roc_cmd_cat(m, first);
        } else if (strcmp(cmd, "ed") == 0) {
            ed_begin(m, first);
        } else {
            roc_cmd_less(m, first);
        }
#if ROC_APP_TOOLS
    } else if (strcmp(cmd, "diff") == 0) {
        roc_cmd_diff(m, rest);
#endif
    } else if (strcmp(cmd, "pbcopy") == 0) {
        /* what the | hands over; without one, the words themselves */
        bool ok;
        if (m->piped) {
            if (roc_input_spooled(m, "pbcopy")) {
                return;
            }
            ok = roc_cmd_pbcopy(m, m->in, m->in_len);
        } else {
            ok = roc_cmd_pbcopy(m, (const uint8_t *)rest, strlen(rest));
        }
        m->status = ok ? 0 : 1;
    } else if (strcmp(cmd, "pbpaste") == 0) {
        m->status = roc_cmd_pbpaste(m) ? 0 : 1;
#if ROC_APP_COREWAR
    } else if (strcmp(cmd, "mars") == 0) {
        roc_cmd_mars(m, rest);
    } else if (strcmp(cmd, "corewar") == 0) {
        roc_cmd_corewar(m, rest);
#endif
#if ROC_APP_LIVE
    } else if (strcmp(cmd, "live") == 0) {
        live_begin(m);
#endif
#if ROC_APP_BOARD
    } else if (strcmp(cmd, "menu") == 0) {
        screen_enter(m, "main");
#endif
    } else if (strcmp(cmd, "test") == 0 || strcmp(cmd, "[") == 0) {
        test_cmd(m, sl);
    } else if (strcmp(cmd, "alias") == 0) {
        alias_cmd(m, sl);
    } else if (strcmp(cmd, "unalias") == 0) {
        unalias_cmd(m, sl);
    } else if (strcmp(cmd, "wait") == 0) {
        /* what cmd & ran has ended already: wait $! is its $?, wait alone 0 */
        m->status = 0;
        for (int i = 1; i < sl->argc; i++) {
            char id[12];
            (void)snprintf(id, sizeof(id), "%d", m->last_bg + 1);
            m->status = m->last_bg > 0 && strcmp(sl->argv[i], id) == 0 ? m->bg_status : 127;
        }
    } else if (strcmp(cmd, "true") == 0 || strcmp(cmd, ":") == 0) {
        m->status = 0;
    } else if (strcmp(cmd, "false") == 0) {
        m->status = 1;
    } else if (strcmp(cmd, "sleep") == 0) {
        sleep_cmd(m, sl);
    } else if (strcmp(cmd, "printf") == 0) {
        roc_cmd_printf(m, sl->argc, sl->argv);
    } else if (strcmp(cmd, "echo") == 0) {
        /* the words, one space between them, as sh's echo; -n: no newline */
        int from = 1;
        if (sl->argc > 1 && strcmp(sl->argv[1], "-n") == 0) {
            from = 2;
        }
        for (int i = from; i < sl->argc; i++) {
            if (i > from) {
                roc_out(m, (const uint8_t *)" ", 1);
            }
            roc_out(m, (const uint8_t *)sl->argv[i], strlen(sl->argv[i]));
        }
        if (from == 1) {
            roc_out(m, (const uint8_t *)"\n", 1);
        }
    } else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "logout") == 0) {
        m->list.active = false; /* the rest of the list goes with the shell */
                                /* the shell "spawned" this shell; exit returns to it. Logoff for
                                   real is the menu's X. Without an index there is no board. */
#if ROC_APP_BOARD
        if (m->indexed && (m->flags & ROC_F_PROMPT) == 0) {
            screen_enter(m, "main");
            return;
        }
#endif
        term_puts(&m->t, "NO CARRIER\r\n");
        m->exited = true;
    } else if (strcmp(cmd, "set") == 0 && sl->argc == 1) {
        list_vars(m);
    } else if (strcmp(cmd, "set") == 0) {
        set_cmd(m, sl);
    } else if (strcmp(cmd, "export") == 0 && sl->argc == 2 && strcmp(sl->argv[1], "-p") == 0) {
        export_list(m); /* export -p: the exported ones, as lines that set them again */
    } else if (strcmp(cmd, "export") == 0 || strcmp(cmd, "readonly") == 0) {
        /* export: what the utilities the shell runs see (env, env-get) */
        for (int i = 1; i < sl->argc; i++) {
            const char *w = sl->argv[i];
            const char *eq = strchr(w, '=');
            size_t n = eq != NULL ? (size_t)(eq - w) : strlen(w);
            char name[SH_NAME_MAX];
            if (!sh_is_name(w, n) || n >= sizeof(name)) {
                roc_err(m, cmd, w, "bad variable name");
                continue;
            }
            memcpy(name, w, n);
            name[n] = '\0';
            if (eq != NULL) {
                assign(m, w);
            }
            char why[48];
            if (cmd[0] == 'r' && !readonly(m, name) &&
                !sh_var_hold(&m->vars, name, why, sizeof(why))) {
                roc_err(m, cmd, name, why);
            }
            if (cmd[0] == 'e' && !sh_var_export(&m->vars, name, true, why, sizeof(why))) {
                roc_err(m, cmd, name, why);
            }
        }
    } else if (strcmp(cmd, "trap") == 0) {
        trap_cmd(m, sl);
    } else if (strcmp(cmd, "type") == 0) {
        type_cmd(m, sl, false, 1);
    } else if (strcmp(cmd, "command") == 0 && sl->argc > 1 &&
               (strcmp(sl->argv[1], "-v") == 0 || strcmp(sl->argv[1], "-V") == 0)) {
        type_cmd(m, sl, sl->argv[1][1] == 'v', 2);
    } else if (strcmp(cmd, "local") == 0) {
        local_cmd(m, sl);
    } else if (strcmp(cmd, "getopts") == 0) {
        getopts_cmd(m, sl);
    } else if (strcmp(cmd, "read") == 0) {
        read_cmd(m, sl);
    } else if (strcmp(cmd, "shift") == 0) {
        shift_params(m, sl);
    } else if (strcmp(cmd, "unset") == 0 && sl->argc > 1 && strcmp(sl->argv[1], "-f") == 0) {
        for (int i = 2; i < sl->argc; i++) {
            int fi = func_find(m, sl->argv[i]);
            if (fi >= 0 && m->ncalls > 0) {
                roc_err(m, "unset", sl->argv[i], "functions are running");
            } else if (fi >= 0) {
                func_remove(m, fi);
            }
        }
    } else if (strcmp(cmd, "unset") == 0) {
        for (int i = 1; i < sl->argc; i++) {
            if (readonly(m, sl->argv[i])) {
                roc_err(m, "unset", sl->argv[i], "readonly variable");
                continue;
            }
            sh_var_unset(&m->vars, sl->argv[i]);
        }
    } else if (strcmp(cmd, "edit") == 0) {
        (void)script_run(m, "edt", rest); /* the word hands reach for first */
    } else if (!host_run(m, sl)) {
        if (m->host.run != NULL && roc_output_captured(m) &&
            roc_host_command(m, cmd, strlen(cmd))) {
            roc_err(m, cmd, "",
                    "a command of the app takes the whole terminal: not under | > or $(...)");
            m->status = 126;
            return;
        }
        roc_err(m, cmd, "", "command not found (try 'help')");
        m->status = 127;
    }
}

/* Whether the shell is back at its prompt: nothing reading, no screen
   up, neither ed nor the REPL taking the lines. */
static bool idle(const roc *m) {
    if (m->mode != ROC_MODE_LINE || m->req_kind != ROC_REQ_NONE || m->exited) {
        return false;
    }
    if (m->read_wait || m->sleep_ms > 0 || m->hosted) {
        return false; /* read at the prompt, a sleep: first the line, or the time */
    }
    if (m->t.napps > 0 || m->sc.repl) {
        return false;
    }
    if (ed_active(m)) {
        return false;
    }
    return true;
}

bool roc_list_waiting(const roc *m) {
    if (!m->list.active || m->list.running) {
        return false;
    }
    return idle(m);
}

/* ---- pathname expansion: * ? [...] over the tree ---- */

enum { GLOB_MAX = SH_WORDS_MAX };

/* Whether a component has a * ? [ that is not escaped. */
static bool globby(const char *comp) {
    for (const char *p = comp; *p != '\0'; p++) {
        if (*p == '\\' && p[1] != '\0') {
            p++;
            continue;
        }
        if (*p == '*' || *p == '?' || *p == '[') {
            return true;
        }
    }
    return false;
}

/* A component with its escapes taken away, in dst. */
static void unescape(const char *comp, char *dst, size_t cap) {
    size_t n = 0;
    for (const char *p = comp; *p != '\0' && n + 1 < cap; p++) {
        if (*p == '\\' && p[1] != '\0') {
            p++;
        }
        dst[n++] = *p;
    }
    dst[n] = '\0';
}

typedef struct {
    roc *m;
    char text[VFS_PATH_MAX]; /* the pattern's components, a NUL after each */
    const char *comps[16];
    int ncomps;
    char *found[GLOB_MAX];
    int nfound;
    char *out;
    size_t used;
    size_t cap;
    bool full;
} globber;

static void glob_keep(globber *g, const char *typed) {
    size_t n = strlen(typed);
    if (g->nfound == GLOB_MAX || n + 1 > g->cap - g->used) {
        g->full = true;
        return;
    }
    memcpy(g->out + g->used, typed, n + 1);
    g->found[g->nfound] = g->out + g->used;
    g->nfound++;
    g->used += n + 1;
}

/* Component k on, from the directory dir (absolute), the path typed so far
   in typed (as the pattern was written: relative stays relative). */
static void glob_walk(globber *g, const char *dir, const char *typed, int k) {
    if (g->full) {
        return;
    }
    if (k == g->ncomps) {
        glob_keep(g, typed);
        return;
    }
    char next[VFS_PATH_MAX];
    char shown[VFS_PATH_MAX];
    const char *comp = g->comps[k];
    bool last = k + 1 == g->ncomps;
    if (!globby(comp)) {
        char lit[VFS_PATH_MAX];
        unescape(comp, lit, sizeof(lit));
        if (!vfs_resolve(dir, lit, next, sizeof(next))) {
            return;
        }
        const vfs_node *node = vfs_lookup(&g->m->fs, next);
        if (node == NULL || (!last && !node->dir)) {
            return;
        }
        (void)snprintf(shown, sizeof(shown), "%s%s%s", typed, lit, last ? "" : "/");
        glob_walk(g, next, shown, k + 1);
        return;
    }
    roc_sync_dir(g->m, dir);
    for (size_t i = 0; i < g->m->fs.nnodes; i++) {
        const vfs_node *node = &g->m->fs.nodes[i];
        if (!vfs_is_child(node, dir)) {
            continue;
        }
        const char *base = vfs_base(node);
        if (base[0] == '.' && comp[0] != '.') {
            continue; /* a dot file only for a pattern that starts with one */
        }
        if (!sh_match(comp, base) || (!last && !node->dir)) {
            continue;
        }
        (void)snprintf(shown, sizeof(shown), "%s%s%s", typed, base, last ? "" : "/");
        glob_walk(g, node->path, shown, k + 1);
    }
}

/* sh_glob for the lexer: the paths of the tree the pattern matches. */
static const char *glob_paths(void *user, const char *pattern, int *count) {
    static globber g;
    static char out[8 * SH_LINE_MAX];
    static char packed[8 * SH_LINE_MAX];
    roc *m = user;
    *count = 0;
    if (!m->indexed) {
        return NULL;
    }
    g.m = m;
    g.ncomps = 0;
    g.nfound = 0;
    g.out = out;
    g.used = 0;
    g.cap = sizeof(out);
    g.full = false;
    const char *p = pattern;
    bool absolute = *p == '/';
    size_t used = 0;
    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *e = p;
        while (*e != '\0' && *e != '/') {
            e++;
        }
        size_t n = (size_t)(e - p);
        if (g.ncomps == 16 || n + 1 > sizeof(g.text) - used) {
            return NULL; /* longer than any path: it matches none */
        }
        memcpy(g.text + used, p, n);
        g.text[used + n] = '\0';
        g.comps[g.ncomps++] = g.text + used;
        used += n + 1;
        p = e;
    }
    glob_walk(&g, absolute ? "/" : m->cwd, absolute ? "/" : "", 0);
    if (g.full) {
        *count = -1; /* more than a line holds */
        return NULL;
    }
    if (g.nfound == 0) {
        return NULL; /* none: the word as it is */
    }
    for (int i = 1; i < g.nfound; i++) { /* sorted, as sh sorts them */
        char *v = g.found[i];
        int j = i;
        while (j > 0 && strcmp(g.found[j - 1], v) > 0) {
            g.found[j] = g.found[j - 1];
            j--;
        }
        g.found[j] = v;
    }
    size_t at = 0;
    for (int i = 0; i < g.nfound; i++) {
        size_t n = strlen(g.found[i]) + 1;
        memcpy(packed + at, g.found[i], n);
        at += n;
    }
    *count = g.nfound;
    return packed;
}

static void unwind(roc *m);

/* Whether the line a $(...) ran is over: nothing of it still going. */
static bool ended(const roc *m) {
    if (m->list.active) {
        return false;
    }
    return idle(m);
}

/* Whatever a $(...) left running, stopped: it had to end where it was. */
static void stop_waiting(roc *m) {
    m->list.active = false; /* the rest of the $(...) goes too */
    if (m->req_kind != ROC_REQ_NONE) {
        roc_reader_abort_quiet(m);
    }
    while (m->t.napps > 0) {
        roc_app_leave(m, NULL);
    }
    m->sc.repl = false;
    m->mode = ROC_MODE_LINE;
}

/* $(command) for sh_expand: the command's line run here and now, what it
   writes kept, and the line that called it (its list, its input) put back
   as it was, since it is halfway read. ROC_CFG_SUB_DEPTH deep, and only
   what ends at once: a file of the site, a screen or the REPL cannot answer
   a line being read. */
static const char *substitute(void *user, const char *cmd, size_t len, size_t *out_len, char *why,
                              size_t cap) {
    roc *m = user;
    static uint8_t saved_in[ROC_CAP_MAX]; /* the | input of each line waiting, one after another */
    static size_t saved_in_used;
    if (sub_depth >= ROC_CFG_SUB_DEPTH) {
        (void)snprintf(why, cap, "$(...) nested too deep");
        return NULL;
    }
    if (len > SH_LINE_MAX) {
        (void)snprintf(why, cap, "Line too long");
        return NULL;
    }
    const uint8_t *in = m->in;
    size_t in_len = m->in_len;
    void *in_spool = m->in_spool; /* the $(...) has none of its own */
    uint64_t in_spool_len = m->in_spool_len;
    m->in_spool = NULL;
    m->in_spool_len = 0;
    size_t in_at = saved_in_used;
    bool keep_in = false; /* its | input, in pipe_buf, which the $(...) uses too */
    if (in == m->pipe_buf && in_len > 0) {
        keep_in = true;
    }
    if (keep_in && in_len > sizeof(saved_in) - in_at) {
        (void)snprintf(why, cap, "$(...): Input too large to keep");
        return NULL;
    }
    if (keep_in) {
        memcpy(saved_in + in_at, m->pipe_buf, in_len);
        saved_in_used += in_len;
    }
    roc_list *saved = &sub_saved[sub_depth];
    *saved = m->list;
    int ncalls = m->ncalls;
    bool piped = m->piped;
    roc_list *l = &m->list;
    /* cmd is in the text it replaces (saved above); a subshell, run as ( ) is */
    memmove(l->text + 1, cmd, len);
    l->text[0] = '(';
    memcpy(l->text + 1 + len, "\n)", 3);
    l->text[len + 3] = '\0';
    size_t stext_used = m->stext_used;
    int nbins = m->nbins;
    int floor = locals_floor;
    locals_floor = m->nlocals;
    l->src = l->text;
    l->pos = 0;
    l->op = SH_OP_SEMI;
    l->active = true;
    l->nframes = 0; /* its own: a break in it breaks nothing outside */
    l->words_used = 0;
    m->piped = false;
    /* what it writes goes after what the one around it kept so far */
    size_t out_at = sub_len;
    bool outer_overflow = sub_overflow;
    bool outer_long = sub_long;
    int outer_base = sub_base;
    sub_overflow = false;
    sub_long = false;
    sub_base = m->nbouts;
    sub_depth++;
    roc_list_continue(m);
    sub_depth--;
    bool done = ended(m);
    if (!done) {
        stop_waiting(m);
    }
    int status = m->status;
    unwind(m);
    locals_floor = floor;
    m->stext_used = stext_used;
    m->nbins = nbins;
    if (m->cap.on) {
        roc_capture_end(m);
    }
    roc_err_end(m);
    block_out_drop_to(m, sub_base);
    m->list = *saved;
    m->ncalls = ncalls;
    if (keep_in) {
        memcpy(m->pipe_buf, saved_in + in_at, in_len);
    }
    saved_in_used = in_at;
    m->in = in;
    m->in_len = in_len;
    roc_spool_drop(m);
    m->in_spool = in_spool;
    m->in_spool_len = in_spool_len;
    m->piped = piped;
    bool overflow = sub_overflow;
    bool too_long = sub_long;
    sub_overflow = outer_overflow;
    sub_long = outer_long;
    sub_base = outer_base;
    size_t n = sub_len - out_at;
    sub_len = out_at; /* its text stays there until the next $(...) writes */
    if (!done) {
        (void)snprintf(why, cap, "$(...) cannot wait for the site or a screen");
        return NULL;
    }
    if (too_long) {
        (void)snprintf(why, cap, "$(...) ran too long");
        return NULL;
    }
    if (overflow) {
        (void)snprintf(why, cap, "$(...): Output too large");
        return NULL;
    }
    sub_ran = true;
    sub_status = status;
    *out_len = n;
    return (const char *)sub_out + out_at;
}

size_t roc_call_keep(const roc *m) {
    size_t n = m->cap.on ? m->cap.len : 0;
    if (m->in == m->pipe_buf) {
        n += m->in_len;
    }
    return n;
}

bool roc_call(roc *m, const char *line, size_t len, const uint8_t *in, size_t in_len, uint8_t *keep,
              size_t keep_cap, roc_called *res, char *why, size_t why_cap) {
    if (roc_call_keep(m) > keep_cap) {
        (void)snprintf(why, why_cap, "no room to keep what the command wrote so far");
        return false;
    }
    /* the running command's: its capture (the bytes into keep), 2>, input,
       $? and directory */
    roc_capture *c = &m->cap;
    bool on = c->on;
    bool paused = c->paused;
    bool append = c->append;
    bool overflow = c->overflow;
    bool pipe = c->pipe;
    bool discard = c->discard;
    bool kept = c->keep;
    bool to_err = c->to_err;
    uint8_t esc = c->esc;
    size_t clen = on ? c->len : 0;
    char cpath[VFS_PATH_MAX];
    memcpy(cpath, c->path, sizeof(cpath));
    memcpy(keep, c->buf, clen);
    bool in_pipe = m->in == m->pipe_buf;
    size_t was_len = m->in_len;
    if (in_pipe) {
        memcpy(keep + clen, m->pipe_buf, was_len);
    }
    const uint8_t *was_in = m->in;
    bool was_piped = m->piped;
    /* the running command's spools, its | input and what it wrote past
       buf, are its own: the line called runs without them */
    void *was_spool = m->in_spool;
    uint64_t was_spool_len = m->in_spool_len;
    void *out_spool = c->spool;
    uint64_t out_spool_len = c->spool_len;
    m->in_spool = NULL;
    m->in_spool_len = 0;
    c->spool = NULL;
    c->spool_len = 0;
    bool spilled = c->spilled; /* its > open on its file, too */
    bool spill_host = c->spill_host;
    void *spill_h = c->spill_h;
    char spill_tmp[VFS_PATH_MAX];
    memcpy(spill_tmp, c->spill_tmp, sizeof(spill_tmp));
    c->spilled = false;
    roc_errcap err = m->errcap;
    int status = m->status;
    char cwd[sizeof(m->cwd)];
    memcpy(cwd, m->cwd, sizeof(cwd));

    c->on = false;
    c->paused = false;
    roc_errcap *e = &m->errcap;
    e->on = true;
    e->to_out = false;
    e->discard = false;
    e->keep = true;
    e->overflow = false;
    e->len = 0;
    m->in = in;
    m->in_len = in != NULL ? in_len : 0;
    m->piped = in != NULL;
    const uint8_t *outer_in = call_in;
    size_t outer_in_len = call_in_len;
    int outer_level = call_level;
    call_in = in;
    call_in_len = in != NULL ? in_len : 0;
    call_level = sub_depth + 1; /* the depth substitute runs the line at */
    size_t olen = 0;
    const char *o = substitute(m, line, len, &olen, why, why_cap);
    call_in = outer_in;
    call_in_len = outer_in_len;
    call_level = outer_level;
    memcpy(res->err, e->buf, e->len);
    res->err_len = e->len;
    res->err_cut = e->overflow;
    res->status = sub_status;

    m->errcap = err;
    c->on = on;
    c->paused = paused;
    c->append = append;
    c->overflow = overflow;
    c->pipe = pipe;
    c->discard = discard;
    c->keep = kept;
    c->to_err = to_err;
    c->esc = esc;
    c->len = clen;
    memcpy(c->path, cpath, sizeof(cpath));
    memcpy(c->buf, keep, clen);
    if (in_pipe) {
        memcpy(m->pipe_buf, keep + clen, was_len);
    }
    m->in = was_in;
    m->in_len = was_len;
    roc_spool_drop(m);
    spill_abandon(m, c); /* what the line called left open: its spool, its > */
    m->in_spool = was_spool;
    m->in_spool_len = was_spool_len;
    c->spool = out_spool;
    c->spool_len = out_spool_len;
    c->spilled = spilled;
    c->spill_host = spill_host;
    c->spill_h = spill_h;
    memcpy(c->spill_tmp, spill_tmp, sizeof(spill_tmp));
    m->piped = was_piped;
    m->status = status;
    memcpy(m->cwd, cwd, sizeof(cwd));
    if (o == NULL) {
        return false;
    }
    res->out = (const uint8_t *)o;
    res->out_len = olen;
    return true;
}

/* ---- if, while, until, for: jumps in the text ---- */

static bool run_now(const roc *m) {
    const roc_list *l = &m->list;
    if (l->op == SH_OP_AND) {
        return m->status == 0;
    }
    if (l->op == SH_OP_OR) {
        return m->status != 0;
    }
    if (l->op == SH_OP_PIPE) {
        return l->ran;
    }
    return true;
}

static void say_why(roc *m, const char *why) {
    term_puts(&m->t, "rocchetto: ");
    term_puts(&m->t, why);
    term_puts(&m->t, "\r\n");
    m->status = 2;
}

/* To the reserved word of targets, depth closers on from here; its
   position. False, said, when the text has none (sh_check says it has). */
static bool jump(roc *m, size_t from, unsigned targets, int depth, size_t *at, int *found) {
    if (!sh_skip(m->list.src, from, targets, depth, at, found)) {
        say_why(m, "Syntax error: lost in if, while or for");
        return false;
    }
    return true;
}

static unsigned bit(int rw) {
    return 1U << (unsigned)rw;
}

/* for's variable set to its word k. */
static bool for_word(roc *m, const roc_frame *f, uint32_t k) {
    const char *w = m->list.words + f->words;
    for (uint32_t i = 0; i < k; i++) {
        w += strlen(w) + 1;
    }
    char assign_to[SH_NAME_MAX + SH_VALUE_MAX + 1];
    (void)snprintf(assign_to, sizeof(assign_to), "%s=%s", f->var, w);
    char why[48];
    if (!sh_var_assign(&m->vars, assign_to, why, sizeof(why))) {
        roc_err(m, "for", f->var, why);
        return false;
    }
    return true;
}

/* The head of a for or a case and case's patterns, each read and copied
   out before the next is: one line a depth for the three (a $(...) in them
   reads its own, a depth down). */
static sh_line heads[ROC_CFG_SUB_DEPTH + 1];

/* for NAME in words: the head read and expanded, its words kept. */
static void params_into(const roc *m, sh_line *sl);

static size_t spaces_at(const char *p) {
    size_t n = 0;
    while (p[n] == ' ' || p[n] == '\t') {
        n++;
    }
    return n;
}

static bool for_head(roc *m, roc_frame *f) {
    roc_list *l = &m->list;
    sh_line *head = &heads[sub_depth];
    const sh_env env = {lookup, substitute, glob_paths, m, set_var};
    char why[128];
    size_t for_do = sh_for_do(l->src + l->pos);
    if (for_do > 0) { /* for name do: the name alone, do next */
        size_t k = l->pos + spaces_at(l->src + l->pos);
        size_t n = 0;
        while (k + n < l->pos + for_do && l->src[k + n] != ' ' && l->src[k + n] != '\t' &&
               l->src[k + n] != '\n' && n + 1 < sizeof(f->var)) {
            n++;
        }
        head->argc = 1;
        (void)snprintf(head->buf, sizeof(head->buf), "%.*s", (int)n, l->src + k);
        head->argv[0] = head->buf;
        head->argv[1] = NULL;
        head->next = for_do;
    } else if (!sh_expand(l->src + l->pos, head, &env, why, sizeof(why))) {
        say_why(m, why);
        return false;
    }
    l->pos += head->next;
    (void)snprintf(f->var, sizeof(f->var), "%s", head->argv[0]);
    f->words = l->words_used;
    f->nwords = 0;
    int first = 2;
    if (head->argc == 1) { /* for name; do: the parameters, "$@" */
        params_into(m, head);
        first = 1;
    }
    for (int i = first; i < head->argc; i++) {
        size_t n = strlen(head->argv[i]) + 1;
        if (n > sizeof(l->words) - l->words_used) {
            say_why(m, "for: too many words");
            return false;
        }
        memcpy(l->words + l->words_used, head->argv[i], n);
        l->words_used += n;
        f->nwords++;
    }
    return true;
}

/* What a subshell keeps apart: its variables, its directory, its traps and
   the functions it defines (none it redefines or unsets: calls run). */
typedef struct {
    sh_vars vars;
    char cwd[VFS_PATH_MAX];
    char trap_exit[SH_LINE_MAX];
    int nfuncs;
    size_t ftext_used;
} subshell_saved;

static void subshell_restore(roc *m, const roc_frame *f) {
    subshell_saved saved;
    memcpy(&saved, m->stext + f->text_from, sizeof(saved));
    m->vars = saved.vars;
    memcpy(m->cwd, saved.cwd, sizeof(m->cwd));
    memcpy(m->trap_exit, saved.trap_exit, sizeof(m->trap_exit));
    m->nfuncs = saved.nfuncs;
    m->ftext_used = saved.ftext_used;
}

static void pop(roc *m) {
    roc_list *l = &m->list;
    l->nframes--;
    const roc_frame *f = &l->frames[l->nframes];
    if (f->rw == SH_RW_FOR || f->rw == SH_RW_CASE || f->rw == SH_RW_CALL) {
        l->words_used = f->words; /* its words go with it */
    }
    if (f->rw == SH_RW_CALL) {
        m->ncalls--;
        locals_restore(m, l->nframes);
    }
    if (f->captured) {
        block_out_drop_to(m, m->nbouts - 1); /* left by a break N, not by its end */
    }
    if (f->fed) {
        m->nbins--;
    }
    if (f->errs) {
        block_err_pop(m);
    }
    if (f->script) {
        if (f->subshell) { /* what the script changed goes with it */
            subshell_restore(m, f);
        }
        m->stext_used = f->text_from;
    }
}

/* A list given up halfway (an error, ^C, a $(...) that could not wait):
   what its locals and subshells changed put back, as their ends would
   have, innermost first. */
static void unwind(roc *m) {
    roc_list *l = &m->list;
    for (int k = l->nframes - 1; k >= 0; k--) {
        locals_restore(m, k);
        if (l->frames[k].subshell) {
            subshell_restore(m, &l->frames[k]);
        }
    }
    l->nframes = 0;
}

/* From the patterns of a branch of the case on top: the branch whose
   pattern matches its word, run next, or esac. With match false (a branch
   ran, its ;; came) only on to esac, the patterns read, not expanded. */
static bool case_next(roc *m, bool match) {
    roc_list *l = &m->list;
    if (l->nframes == 0 || l->frames[l->nframes - 1].rw != SH_RW_CASE) {
        say_why(m, "Syntax error: \";;\" unexpected");
        return false;
    }
    roc_frame *f = &l->frames[l->nframes - 1];
    sh_line *pl = &heads[sub_depth];
    const sh_env run = {lookup, substitute, NULL, m, set_var};
    const sh_env read = {lookup, NULL, NULL, m, NULL};
    const char *word = l->words + f->words;
    char why[128];
    l->op = SH_OP_SEMI;
    for (;;) {
        l->pos += sh_blanks(l->src + l->pos);
        size_t len = 0;
        if (sh_reserved(l->src + l->pos, &len) == SH_RW_ESAC) {
            return true; /* control closes it */
        }
        if (!sh_case_patterns(l->src + l->pos, pl, match ? &run : &read, &len, why, sizeof(why))) {
            say_why(m, why);
            return false;
        }
        l->pos += len;
        for (int i = 0; match && i < pl->argc; i++) {
            if (sh_match(pl->argv[i], word)) {
                f->taken = true;
                return true;
            }
        }
        size_t at = 0;
        int found = 0;
        if (!jump(m, l->pos, bit(SH_RW_ESAC) | bit(SH_RW_DSEMI), 0, &at, &found)) {
            return false;
        }
        l->pos = at;
    }
}

/* case WORD in: the word kept, then the branch that matches it. */
static bool case_head(roc *m, roc_frame *f) {
    roc_list *l = &m->list;
    sh_line *head = &heads[sub_depth];
    const sh_env env = {lookup, substitute, NULL, m, set_var};
    char why[128];
    size_t len = 0;
    if (!sh_case_head(l->src + l->pos, head, &env, &len, why, sizeof(why))) {
        say_why(m, why);
        return false;
    }
    l->pos += len;
    size_t n = strlen(head->argv[0]) + 1;
    if (n > sizeof(l->words) - l->words_used) {
        say_why(m, "case: word too long");
        return false;
    }
    f->words = l->words_used;
    memcpy(l->words + l->words_used, head->argv[0], n);
    l->words_used += n;
    return case_next(m, true);
}

/* if, while, until, for, where a command would start: its fi or done
   found, and what follows that read (> f, | b, the operator), for where
   the run goes when the block ends, or now when && or || skips it. */
static bool open_block(roc *m, int rw, size_t len) {
    roc_list *l = &m->list;
    static sh_line tails[ROC_CFG_SUB_DEPTH + 1];
    sh_line *tail = &tails[sub_depth];
    size_t at = 0;
    int found = 0;
    /* from the opener itself (depth -1): sh_skip reads case's head and
       patterns as such, not as commands */
    if (!jump(m, l->pos, bit(sh_closer(rw)), -1, &at, &found)) {
        return false;
    }
    size_t closer_len = 0;
    (void)sh_reserved(l->src + at, &closer_len);
    at += closer_len;
    bool run = run_now(m);
    const sh_env env = {lookup, run ? substitute : NULL, NULL, m, run ? set_var : NULL};
    char why[128];
    if (!sh_after_close(l->src + at, tail, &env, why, sizeof(why))) {
        say_why(m, why);
        return false;
    }
    int op = tail->op == SH_OP_END ? SH_OP_SEMI : tail->op;
    if (!run) { /* && or || said no: the whole of it goes, its pipeline too */
        l->negate_next = false;
        l->pos = at + tail->next;
        l->op = op;
        l->ran = false;
        return true;
    }
    if (l->nframes == SH_NEST_MAX) {
        say_why(m, "Too deeply nested");
        return false;
    }
    roc_frame *f = &l->frames[l->nframes];
    memset(f, 0, sizeof(*f));
    f->rw = rw;
    f->tail = at + tail->next;
    f->tail_op = op;
    f->negated = l->negate_next;
    l->negate_next = false;
    if (rw == SH_RW_IF || rw == SH_RW_WHILE || rw == SH_RW_UNTIL) {
        f->in_cond = true;
    }
    if (tail->in != NULL || tail->here != NULL || l->op == SH_OP_PIPE) { /* < wins over | */
        if (!block_in_push(m, tail)) {
            return false;
        }
        f->fed = true;
    }
    if (tail->out != NULL || tail->out_to_err || tail->op == SH_OP_PIPE) {
        if (!block_out_push(m, tail)) {
            if (f->fed) {
                m->nbins--;
            }
            return false;
        }
        f->captured = true;
    }
    if (tail->err != NULL || tail->err_to_out) {
        if (!block_err_push(m, tail)) {
            if (f->captured) {
                block_out_drop_to(m, m->nbouts - 1);
            }
            if (f->fed) {
                m->nbins--;
            }
            return false;
        }
        f->errs = true;
    }
    l->nframes++;
    l->pos += len;
    l->op = SH_OP_SEMI;
    f->cond = l->pos;
    if (rw == SH_RW_FOR) {
        return for_head(m, f);
    }
    if (rw == SH_RW_CASE) {
        return case_head(m, f);
    }
    return true;
}

/* fi, or done when the loop is over: what the block wrote where it goes,
   and the run on after it. */
static void end_block(roc *m) {
    roc_list *l = &m->list;
    roc_frame *f = &l->frames[l->nframes - 1];
    l->to_file = false;
    if (f->captured) {
        f->captured = false;
        l->to_file = block_out_pop(m);
    }
    if (f->rw == SH_RW_CALL) {
        l->src = f->src; /* back where the function was called */
    }
    l->pos = f->tail;
    l->op = f->tail_op;
    l->ran = true;
    bool negated = f->negated;
    pop(m);
    if (negated) {
        m->status = m->status == 0 ? 1 : 0;
    }
}

/* set -e and a command failing where nothing looks at its $?: out of the
   script it is in (sh f, ./f.sh), or else the line ends. */
static bool leave_script(roc *m, int status);

static void errexit(roc *m) {
    if (!leave_script(m, m->status)) {
        m->list.active = false;
    }
}

/* Whether the command running now is looked at: a condition of if, while or
   until (in this function, not the one that called it). */
static bool in_condition(const roc_list *l) {
    for (int k = l->nframes - 1; k >= 0; k--) {
        if (l->frames[k].in_cond) {
            return true;
        }
        if (l->frames[k].rw == SH_RW_CALL && !l->frames[k].seen_through) {
            return false;
        }
    }
    return false;
}

/* ---- functions: NAME() { ... }, kept for the session ---- */

static int func_find(const roc *m, const char *name) {
    for (int i = 0; i < m->nfuncs; i++) {
        if (strcmp(m->funcs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void func_remove(roc *m, int i) {
    size_t off = m->funcs[i].off;
    size_t n = m->funcs[i].len + 1;
    memmove(m->ftext + off, m->ftext + off + n, m->ftext_used - off - n);
    m->ftext_used -= n;
    for (int k = 0; k < m->nfuncs; k++) {
        if (m->funcs[k].off > off) {
            m->funcs[k].off -= n;
        }
    }
    m->nfuncs--;
    memmove(&m->funcs[i], &m->funcs[i + 1], (size_t)(m->nfuncs - i) * sizeof(m->funcs[0]));
}

/* The builtins POSIX finds before any function: none is one. */
static bool special_builtin(const char *name) {
    static const char *const names[] = {
        "break",    ":",      "continue", ".",     "eval",  "exec", "exit",  "export",
        "readonly", "return", "set",      "shift", "times", "trap", "unset",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(name, names[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void func_define(roc *m, const char *name, const char *body, size_t len) {
    if (special_builtin(name)) {
        roc_err(m, name, "", "a special builtin: not a function's name");
        return;
    }
    int old = func_find(m, name);
    if (old >= 0 && m->ncalls > 0) {
        roc_err(m, name, "", "functions are running: define it again after");
        return;
    }
    if (old >= 0) {
        func_remove(m, old);
    }
    if (m->nfuncs < 0 || m->nfuncs >= ROC_CFG_FUNCS || len + 1 > sizeof(m->ftext) - m->ftext_used) {
        roc_err(m, name, "", "no room for another function");
        return;
    }
    roc_func *f = &m->funcs[m->nfuncs++];
    (void)snprintf(f->name, sizeof(f->name), "%s", name);
    f->off = m->ftext_used;
    f->len = len;
    memcpy(m->ftext + f->off, body, len);
    m->ftext[f->off + len] = '\0';
    m->ftext_used += len + 1;
}

/* NAME() and its block, where a command would start: kept, and the run on
   after it. False when the line cannot go on. */
static bool define_here(roc *m, size_t fdef, size_t name_len) {
    roc_list *l = &m->list;
    static sh_line tails[ROC_CFG_SUB_DEPTH + 1];
    sh_line *tail = &tails[sub_depth];
    char name[SH_NAME_MAX];
    if (name_len >= sizeof(name)) {
        say_why(m, "Syntax error: a function's name too long");
        return false;
    }
    memcpy(name, l->src + l->pos, name_len);
    name[name_len] = '\0';
    size_t start = l->pos + fdef;
    start += sh_blanks(l->src + start);
    size_t len = 0;
    int rw = sh_reserved(l->src + start, &len);
    size_t at = 0;
    int found = 0;
    if (!sh_opens(rw) || !jump(m, start, bit(sh_closer(rw)), -1, &at, &found)) {
        say_why(m, "Syntax error: a function's body is { }, ( ), if, while, for or case");
        return false;
    }
    (void)sh_reserved(l->src + at, &len);
    size_t end = at + len;
    const sh_env env = {lookup, NULL, NULL, m, NULL};
    char why[128];
    if (!sh_after_close(l->src + end, tail, &env, why, sizeof(why))) {
        say_why(m, why);
        return false;
    }
    if (tail->out != NULL || tail->in != NULL || tail->op == SH_OP_PIPE) {
        say_why(m, "Syntax error: a redirection or a pipe after a function's body: not yet");
        return false;
    }
    bool run = run_now(m);
    l->pos = end + tail->next;
    l->op = tail->op == SH_OP_END ? SH_OP_SEMI : tail->op;
    l->ran = run;
    if (run) {
        m->status = 0;
        func_define(m, name, l->src + start, end - start);
    }
    return true;
}

/* A function called: its words the parameters, its body the text run
   until it ends (or return), then back after the call. False when the
   line cannot go on. */
/* A call: a function's body or a script's text run as the list's src,
   argv[first] its $0 and the words after it $1..., a | or < into it and a
   | or > out of it levels of bin and bcap. 1 when it runs, 0 when it was
   refused (said; the line goes on), -1 when the line cannot go on. */
static int push_call(roc *m, const sh_line *sl, int first, const char *src) {
    roc_list *l = &m->list;
    if (l->nframes == SH_NEST_MAX) {
        roc_err(m, sl->argv[first], "", "Too deeply nested");
        return -1;
    }
    roc_frame *f = &l->frames[l->nframes];
    memset(f, 0, sizeof(*f));
    f->rw = SH_RW_CALL;
    f->words = l->words_used;
    f->word = 1; /* past $0 */
    for (int i = first; i < sl->argc; i++) {
        size_t n = strlen(sl->argv[i]) + 1;
        if (n > sizeof(l->words) - l->words_used) {
            l->words_used = f->words;
            roc_err(m, sl->argv[first], "", "too many words");
            return -1;
        }
        memcpy(l->words + l->words_used, sl->argv[i], n);
        l->words_used += n;
        f->nwords++;
    }
    f->tail = l->pos;
    f->tail_op = l->op == SH_OP_END ? SH_OP_SEMI : l->op;
    if (sl->in != NULL || sl->here != NULL || m->piped) {
        if (!block_in_push(m, sl)) {
            l->words_used = f->words;
            return 0;
        }
        f->fed = true;
    }
    if (sl->out != NULL || sl->out_to_err || sl->op == SH_OP_PIPE) {
        if (!block_out_push(m, sl)) {
            l->words_used = f->words;
            if (f->fed) {
                m->nbins--;
            }
            return 0;
        }
        f->captured = true;
    }
    if (sl->err != NULL || sl->err_to_out) {
        if (!block_err_push(m, sl)) {
            l->words_used = f->words;
            if (f->captured) {
                block_out_drop_to(m, m->nbouts - 1);
            }
            if (f->fed) {
                m->nbins--;
            }
            return 0;
        }
        f->errs = true;
    }
    l->nframes++;
    m->ncalls++;
    f->src = l->src;
    l->src = src;
    l->pos = 0;
    l->op = SH_OP_SEMI;
    m->status = 0;
    return 1;
}

static bool run_text(roc *m, const sh_line *sl, int first, bool subshell, const char *text,
                     size_t len);

static bool call_func(roc *m, const sh_line *sl, int fi) {
    return push_call(m, sl, 0, m->ftext + m->funcs[fi].off) >= 0;
}

/* sh FILE [ARG...], sh -c TEXT [NAME ARG...], . FILE, ./FILE.sh: the text
   checked whole, then run as a call; sh and ./ as a subshell (variables and
   directory back after it, exit leaves only it). first: the word that
   names it. False when the line cannot go on. */
static bool run_script(roc *m, const sh_line *sl, int first, bool subshell, const char *raw,
                       size_t raw_len) {
    char why[128];
    static char as_read[SH_SCRIPT_MAX + 1];
    if (raw_len > SH_SCRIPT_MAX) {
        roc_err(m, sl->argv[first], "", "Script too large");
        return true;
    }
    memcpy(as_read, raw, raw_len);
    as_read[raw_len] = '\0';
    static char moved[SH_SCRIPT_MAX + 1]; /* its here-documents moved in */
    if (!sh_heredocs(as_read, moved, sizeof(moved), why, sizeof(why))) {
        char said[160];
        (void)snprintf(said, sizeof(said), "%s: %s", sl->argv[first], why);
        say_why(m, said);
        return true;
    }
    /* and its aliases replaced, back in as_read */
    if (!sh_aliases(moved, as_read, sizeof(as_read), alias_find, m, why, sizeof(why))) {
        roc_err(m, sl->argv[first], "", why);
        return true;
    }
    return run_text(m, sl, first, subshell, as_read, strlen(as_read));
}

/* A text of the shell, its here-documents in it already, checked and run as
   a call (see run_script). */
static bool run_text(roc *m, const sh_line *sl, int first, bool subshell, const char *text,
                     size_t len) {
    char why[128];
    size_t need = len + 1 + (subshell ? sizeof(subshell_saved) : 0);
    if (len > SH_SCRIPT_MAX || need > sizeof(m->stext) - m->stext_used) {
        roc_err(m, sl->argv[first], "", "Script too large");
        return true;
    }
    size_t from = m->stext_used;
    char *at = m->stext + from;
    if (subshell) {
        subshell_saved saved;
        saved.vars = m->vars;
        memcpy(saved.cwd, m->cwd, sizeof(saved.cwd));
        memcpy(saved.trap_exit, m->trap_exit, sizeof(saved.trap_exit));
        saved.nfuncs = m->nfuncs;
        saved.ftext_used = m->ftext_used;
        memcpy(at, &saved, sizeof(saved));
        at += sizeof(saved);
    }
    memcpy(at, text, len);
    at[len] = '\0';
    if (strlen(at) != len) {
        roc_err(m, sl->argv[first], "", "Not a text: a NUL in it");
        return true;
    }
    if (!sh_check(at, why, sizeof(why))) {
        char said[160];
        (void)snprintf(said, sizeof(said), "%s: %s", sl->argv[first], why);
        say_why(m, said);
        return true;
    }
    m->stext_used += need;
    int r = push_call(m, sl, first, at);
    if (r <= 0 || m->list.nframes < 1 || m->list.nframes > SH_NEST_MAX) {
        m->stext_used = from;
        return r == 0;
    }
    roc_frame *f = &m->list.frames[m->list.nframes - 1];
    f->script = true;
    f->subshell = subshell;
    f->text_from = from;
    if (subshell) {
        m->trap_exit[0] = '\0'; /* its own traps, none yet */
    }
    return true;
}

/* The script a command names, if it names one: sh FILE, sh -c TEXT, . FILE,
   a path to a file that starts #!/bin/sh, or a name PATH finds one for
   (after the shell's own and the functions). False, nothing done, for any
   other command; *ok false when the line cannot go on. */
static bool script_command(roc *m, const sh_line *sl, bool *ok) {
    const char *cmd = sl->argv[0];
    int first = 0;
    bool subshell = true;
    *ok = true;
    if (strcmp(cmd, "sh") == 0 && sl->argc > 2 && strcmp(sl->argv[1], "-c") == 0) {
        /* sh -c TEXT [NAME ARG...]: NAME is $0 */
        static sh_line c;
        c = *sl;
        int name = 2;
        if (sl->argc > 3) {
            name = 3;
        } else {
            c.argv[2] = "sh";
        }
        const char *text = sl->argv[2];
        *ok = run_script(m, &c, name, true, text, strlen(text));
        return true;
    }
    if (strcmp(cmd, "sh") == 0 || strcmp(cmd, ".") == 0) {
        if (sl->argc < 2) {
            roc_err(m, cmd, "", "a file is expected");
            m->status = 2;
            return true;
        }
        first = 1;
        subshell = cmd[0] != '.';
    }
    char path[VFS_PATH_MAX];
    if (first == 0 && strchr(cmd, '/') == NULL) {
        if (intrinsic(cmd) || func_find(m, cmd) >= 0 ||
            roc_path_find(m, cmd, path, sizeof(path)) != 's') {
            return false;
        }
    } else if (first == 0) {
        if (!roc_resolve_arg(m, cmd, path, sizeof(path)) || roc_exec_kind(m, path, true) != 's') {
            return false; /* not a script: exec says what it is */
        }
    } else if (!roc_resolve_arg(m, sl->argv[first], path, sizeof(path))) {
        return true;
    }
    const uint8_t *data = NULL;
    size_t len = 0;
    const vfs_node *node = roc_lookup(m, path);
    if (node != NULL && node->dir) {
        roc_err(m, sl->argv[first], "", "Is a directory");
        return true;
    }
    if (!roc_find_file(m, path, &data, &len)) {
        if (node != NULL) {
            roc_err(m, sl->argv[first], "", "On the site, not here: cp it home first");
            return true;
        }
        roc_err(m, sl->argv[first], "", "No such file or directory");
        m->status = 127;
        return true;
    }
    *ok = run_script(m, sl, first, subshell, (const char *)data, len);
    return true;
}

/* eval ARG...: the words joined with spaces and read as a line here, with
   the parameters and the loops around it (a transparent call: $1, return,
   shift and break see through it). */
static bool eval_cmd(roc *m, const sh_line *sl) {
    static char text[SH_SCRIPT_MAX + 1];
    size_t k = 0;
    for (int i = 1; i < sl->argc; i++) {
        size_t n = strlen(sl->argv[i]);
        if (k + n + 1 > SH_SCRIPT_MAX) {
            roc_err(m, "eval", "", "Line too long");
            return true;
        }
        if (i > 1) {
            text[k++] = ' ';
        }
        memcpy(text + k, sl->argv[i], n);
        k += n;
    }
    text[k] = '\0';
    if (k == 0) {
        m->status = 0;
        return true;
    }
    int before = m->list.nframes;
    bool ok = run_script(m, sl, 0, false, text, k);
    if (m->list.nframes > before && m->list.nframes <= SH_NEST_MAX) {
        m->list.frames[m->list.nframes - 1].seen_through = true;
    }
    return ok;
}

/* exit [n] in a script run as a subshell: out of it, what it opened closed.
   False when no such script runs: the shell's own exit. */
static bool script_exit(roc *m, const sh_line *sl) {
    int status = m->status;
    if (sl->argc > 1) {
        status = 0;
        for (const char *d = sl->argv[1]; *d >= '0' && *d <= '9'; d++) {
            status = ((status * 10) + (*d - '0')) % 256;
        }
    }
    return leave_script(m, status);
}

/* Out of the innermost script run as a subshell, what it opened closed, $?
   status. False when none runs. */
static bool leave_script(roc *m, int status) {
    roc_list *l = &m->list;
    int k = l->nframes - 1;
    while (k >= 0 && !(l->frames[k].rw == SH_RW_CALL && l->frames[k].subshell)) {
        k--;
    }
    if (k < 0) {
        return false;
    }
    while (l->nframes > k + 1) {
        pop(m);
    }
    m->status = status;
    roc_frame *f = &l->frames[k];
    if (m->trap_exit[0] != '\0') { /* to its end, where its EXIT trap runs */
        f->leaving = true;
        f->body_status = status;
        l->pos = strlen(l->src);
        return true;
    }
    end_block(m); /* which turns it when a ! came before the script */
    return true;
}

/* The text of a trap run here, as eval runs its words: a transparent call. */
static bool run_trap(roc *m, const char *text) {
    static char copy[SH_LINE_MAX];
    (void)snprintf(copy, sizeof(copy), "%s", text);
    static sh_line trap_line;
    trap_line.argc = 2;
    trap_line.argv[0] = "trap";
    trap_line.argv[1] = copy;
    trap_line.argv[2] = NULL;
    trap_line.out = NULL;
    trap_line.in = NULL;
    trap_line.here = NULL;
    trap_line.err = NULL;
    trap_line.err_to_out = false;
    trap_line.op = SH_OP_SEMI;
    int before = m->list.nframes;
    int status = m->status;
    bool ok = run_script(m, &trap_line, 0, false, copy, strlen(copy));
    if (m->list.nframes > before && m->list.nframes <= SH_NEST_MAX) {
        m->list.frames[m->list.nframes - 1].seen_through = true;
    }
    m->status = status; /* a trap leaves $? as it found it */
    return ok;
}

/* return [n]: out of the function, the blocks in it closed on the way. */
static void func_return(roc *m, const sh_line *sl) {
    const roc_list *l = &m->list;
    int k = l->nframes - 1;
    while (k >= 0 && (l->frames[k].rw != SH_RW_CALL || l->frames[k].seen_through)) {
        k--;
    }
    if (k < 0) {
        roc_err(m, "return", "", "only in a function");
        return;
    }
    int status = m->status;
    if (sl->argc > 1) {
        status = 0;
        for (const char *d = sl->argv[1]; *d != '\0'; d++) {
            if (*d < '0' || *d > '9') {
                roc_err(m, "return", sl->argv[1], "Illegal number");
                return;
            }
            status = ((status * 10) + (*d - '0')) % 256;
        }
    }
    while (l->nframes > k + 1) {
        pop(m);
    }
    end_block(m);
    m->status = status;
}

/* shift [n]: $1 goes, $2 is $1. */
static void shift_params(roc *m, const sh_line *sl) {
    roc_list *l = &m->list;
    int k = l->nframes - 1;
    while (k >= 0 && (l->frames[k].rw != SH_RW_CALL || l->frames[k].seen_through)) {
        k--;
    }
    uint32_t n = 1;
    if (sl->argc > 1) {
        n = 0;
        for (const char *d = sl->argv[1]; *d != '\0' && n < 100000; d++) {
            n = (n * 10) + (uint32_t)(*d - '0');
        }
    }
    if (k < 0 || n > l->frames[k].nwords - l->frames[k].word) {
        roc_err(m, "shift", "", "can't shift that many");
        return;
    }
    l->frames[k].word += n;
}

/* The parameters as they are here, $0 first, as the words of sl: what a
   subshell starts with. */
static void params_into(const roc *m, sh_line *sl) {
    const char *w = NULL;
    const roc_frame *f = call_frame(m, &w);
    uint32_t n = m->top_nparams;
    if (f == NULL) {
        w = m->top_params;
    } else {
        n = f->nwords - f->word;
        for (uint32_t i = 0; i < f->word; i++) {
            w += strlen(w) + 1; /* shifted out */
        }
    }
    static char zero[SH_VALUE_MAX];
    (void)snprintf(zero, sizeof(zero), "%s", param(m, "0", 1));
    sl->argv[0] = zero;
    sl->argc = 1;
    for (uint32_t i = 0; i < n && sl->argc < SH_WORDS_MAX; i++) {
        sl->argv[sl->argc++] = (char *)w; /* push_call copies it */
        w += strlen(w) + 1;
    }
    sl->argv[sl->argc] = NULL;
}

/* ( list ): the list a subshell, run as a call of its own text with copies
   of the parameters around it; what follows the ) (> f, | b, the
   operator) read as after any block. False when the line cannot go on. */
static bool open_subshell(roc *m, size_t len) {
    roc_list *l = &m->list;
    static sh_line tails[ROC_CFG_SUB_DEPTH + 1];
    sh_line *tail = &tails[sub_depth];
    size_t at = 0;
    int found = 0;
    if (!jump(m, l->pos, bit(SH_RW_RPAREN), -1, &at, &found)) {
        return false;
    }
    size_t body = l->pos + len;
    bool run = run_now(m);
    const sh_env env = {lookup, run ? substitute : NULL, NULL, m, run ? set_var : NULL};
    char why[128];
    if (!sh_after_close(l->src + at + 1, tail, &env, why, sizeof(why))) {
        say_why(m, why);
        return false;
    }
    bool piped = l->op == SH_OP_PIPE;
    bool negated = l->negate_next;
    l->negate_next = false;
    l->pos = at + 1 + tail->next;
    l->op = tail->op == SH_OP_END ? SH_OP_SEMI : tail->op;
    l->ran = run;
    if (!run) { /* && or || said no: the whole of it goes, its pipeline too */
        return true;
    }
    params_into(m, tail);
    m->piped = piped;
    int frames = l->nframes;
    bool ok = run_text(m, tail, 0, true, l->src + body, at - body);
    /* cppcheck-suppress knownConditionTrueFalse ; the call pushed a frame through m */
    if (l->nframes > frames && l->nframes <= SH_NEST_MAX) {
        l->frames[l->nframes - 1].negated = negated;
    } else {
        l->negate_due = negated;
    }
    return ok;
}

/* A reserved word where a command would start: what it opens, where the
   run goes from it. False when the line cannot go on. */
static bool control(roc *m, int rw, size_t len) {
    roc_list *l = &m->list;
    size_t at = 0;
    int found = 0;
    if (rw == SH_RW_LPAREN) {
        return open_subshell(m, len);
    }
    if (rw == SH_RW_RPAREN) {
        say_why(m, "Syntax error: \")\" unexpected");
        return false;
    }
    if (sh_opens(rw)) {
        return open_block(m, rw, len);
    }
    roc_frame *f = l->nframes > 0 ? &l->frames[l->nframes - 1] : NULL;
    if (f == NULL) {
        say_why(m, "Syntax error: lost in if, while or for");
        return false;
    }
    l->op = SH_OP_SEMI;
    switch (rw) {
    case SH_RW_THEN:
        f->in_cond = false;
        if (m->status == 0) {
            f->taken = true;
            l->pos += len;
            return true;
        }
        if (!jump(m, l->pos + len, bit(SH_RW_ELIF) | bit(SH_RW_ELSE) | bit(SH_RW_FI), 0, &at,
                  &found)) {
            return false;
        }
        l->pos = at;
        return true;
    case SH_RW_ELIF:
    case SH_RW_ELSE:
        if (f->taken) { /* a branch ran: on to fi */
            if (!jump(m, l->pos + len, bit(SH_RW_FI), 0, &at, &found)) {
                return false;
            }
            l->pos = at;
            return true;
        }
        f->taken = rw == SH_RW_ELSE;
        f->in_cond = rw == SH_RW_ELIF;
        l->pos += len;
        return true;
    case SH_RW_RBRACE:
        end_block(m); /* $? the last command's */
        return true;
    case SH_RW_FI:
    case SH_RW_ESAC:
        if (!f->taken) {
            m->status = 0; /* no branch ran: 0, as sh says */
        }
        end_block(m);
        return true;
    case SH_RW_DO: {
        bool go = false;
        if (f->rw == SH_RW_FOR) {
            go = f->word < f->nwords;
            if (go && !for_word(m, f, f->word)) {
                return false;
            }
            f->word++;
        } else {
            go = (m->status == 0) == (f->rw == SH_RW_WHILE);
        }
        f->in_cond = false;
        if (go) {
            l->pos += len;
            f->body = l->pos;
            return true;
        }
        f->leaving = true;
        if (!jump(m, l->pos + len, bit(SH_RW_DONE), 0, &at, &found)) {
            return false;
        }
        l->pos = at;
        return true;
    }
    default: /* done */
        if (!f->leaving) {
            f->ran_body = true;
            f->body_status = m->status;
            if (f->rw != SH_RW_FOR) {
                l->pos = f->cond; /* the condition again */
                f->in_cond = true;
                return true;
            }
            if (f->word < f->nwords) {
                if (!for_word(m, f, f->word)) {
                    return false;
                }
                f->word++;
                l->pos = f->body;
                return true;
            }
        }
        m->status = f->ran_body ? f->body_status : 0;
        end_block(m);
        return true;
    }
}

/* break [n] and continue [n]: out of the n-th loop around, or on to its next
   round, the ifs and loops inside it closed on the way. */
static bool loop_jump(roc *m, const sh_line *sl, bool is_break) {
    roc_list *l = &m->list;
    int n = 1;
    if (sl->argc > 1) {
        n = 0;
        for (const char *d = sl->argv[1]; *d != '\0' && n <= SH_NEST_MAX; d++) {
            if (*d < '0' || *d > '9') {
                n = 0;
                break;
            }
            n = (n * 10) + (*d - '0');
        }
        if (n == 0) {
            roc_err(m, sl->argv[0], sl->argv[1], "Illegal number");
            return true;
        }
    }
    int k = l->nframes - 1;
    while (k >= 0) {
        if (l->frames[k].rw == SH_RW_CALL && !l->frames[k].seen_through && l->frames[k].subshell) {
            return leave_script(m, 0); /* (break) in a loop: out of the subshell, as sh */
        }
        if (l->frames[k].rw == SH_RW_CALL && !l->frames[k].seen_through) {
            k = -1; /* a loop outside the function is not its to break */
            break;
        }
        if (l->frames[k].rw != SH_RW_IF && l->frames[k].rw != SH_RW_CASE &&
            l->frames[k].rw != SH_RW_LBRACE && l->frames[k].rw != SH_RW_CALL) {
            n--;
            if (n == 0) {
                break;
            }
        }
        k--;
    }
    if (k < 0) {
        roc_err(m, sl->argv[0], "", "only in a loop");
        return true;
    }
    /* an eval between: done, and the jump goes on from after it, in the
       text the loop is in */
    for (int e = k + 1; e < l->nframes; e++) {
        if (l->frames[e].rw == SH_RW_CALL) {
            while (l->nframes > e + 1) {
                pop(m);
            }
            end_block(m);
            break;
        }
    }
    size_t at = 0;
    int found = 0;
    size_t from = l->pos;
    if (l->op == SH_OP_DSEMI) {
        from -= 2; /* from the ;; itself: patterns come after it */
    }
    if (!jump(m, from, bit(SH_RW_DONE), l->nframes - 1 - k, &at, &found)) {
        return false;
    }
    while (l->nframes > k + 1) {
        pop(m);
    }
    roc_frame *f = &l->frames[k];
    m->status = 0;
    if (is_break) {
        f->leaving = true;
        f->ran_body = true;
        f->body_status = 0;
    }
    l->pos = at;
    l->op = SH_OP_SEMI;
    return true;
}

/* set -x: the command as it runs, expanded, on the errors' way. */
static void trace(roc *m, const sh_line *sl) {
    static char text[SH_LINE_MAX * 2];
    size_t k = 0;
    text[k++] = '+';
    char *const *lists[2] = {sl->assign, sl->argv};
    const int counts[2] = {sl->nassign, sl->argc};
    for (int part = 0; part < 2; part++) {
        for (int i = 0; i < counts[part]; i++) {
            char one[SH_LINE_MAX * 2];
            if (!sh_join(lists[part] + i, 1, one, sizeof(one))) {
                continue;
            }
            size_t n = strlen(one);
            if (k + n + 2 > sizeof(text)) {
                break;
            }
            text[k++] = ' ';
            memcpy(text + k, one, n);
            k += n;
        }
    }
    text[k] = '\0';
    err_line(m, text, k);
}

void roc_list_continue(roc *m) {
    roc_list *l = &m->list;
    static sh_line lines[ROC_CFG_SUB_DEPTH + 1]; /* the line's, and a $(...)'s while it is read */
    sh_line *sl = &lines[sub_depth];
    const sh_env env = {lookup, substitute, glob_paths, m, set_var};
    l->running = true;
    uint32_t steps = 0;
    while (l->active && idle(m)) {
        if (l->negate_due) { /* ! cmd: its $? turned */
            l->negate_due = false;
            m->status = m->status == 0 ? 1 : 0;
        }
        if (l->op == SH_OP_BG) { /* cmd & ran, and ended: its $? for wait, 0 the list's */
            m->last_bg++;
            m->bg_status = m->status;
            m->status = 0;
            l->op = SH_OP_SEMI;
            l->check_due = false;
        }
        if (l->check_due) {
            l->check_due = false;
            if (m->opt_e && m->status != 0) {
                errexit(m);
                continue;
            }
        }
        steps++;
        if (sub_depth == 0 && steps > ROC_CFG_LIST_STEPS && l->op != SH_OP_PIPE) {
            break; /* the tick comes back: roc_list_waiting */
        }
        if (sub_depth > 0 && steps > ROC_CFG_SUB_STEPS) {
            sub_long = true;
            l->active = false;
            break;
        }
        l->pos += sh_blanks(l->src + l->pos);
        if (l->src[l->pos] == '\0') {
            const roc_frame *top = l->nframes > 0 ? &l->frames[l->nframes - 1] : NULL;
            if (top != NULL && top->rw == SH_RW_CALL && top->subshell && m->trap_exit[0] != '\0') {
                static char text[SH_LINE_MAX]; /* the script's EXIT trap, once */
                memcpy(text, m->trap_exit, sizeof(text));
                m->trap_exit[0] = '\0';
                if (!run_trap(m, text)) {
                    l->active = false;
                    break;
                }
                continue;
            }
            if (top != NULL && top->rw == SH_RW_CALL) {
                if (top->leaving) { /* exit n: its status, whatever the trap did */
                    m->status = top->body_status;
                }
                end_block(m); /* the function ended: back after its call */
                continue;
            }
            l->active = false;
            break;
        }
        if (l->op == SH_OP_DSEMI) { /* a branch of case ran: on to its esac */
            if (!case_next(m, false)) {
                l->active = false;
                break;
            }
            continue;
        }
        size_t bang = sh_bang(l->src + l->pos);
        if (bang > 0) { /* ! pipeline: its $? turned when it ends */
            l->pos += bang;
            l->negate_next = true;
            continue;
        }
        size_t rwlen = 0;
        int rw = sh_reserved(l->src + l->pos, &rwlen);
        if (rw != SH_RW_NONE) {
            if (!control(m, rw, rwlen)) {
                l->active = false;
                break;
            }
            continue;
        }
        size_t name_len = 0;
        size_t fdef = sh_fdef(l->src + l->pos, &name_len);
        if (fdef > 0) {
            if (!define_here(m, fdef, name_len)) {
                l->active = false;
                break;
            }
            continue;
        }
        /* && and || skip the command, which is read all the same; a
           pipeline runs, or is skipped, whole */
        bool run = run_now(m);
        m->piped = l->op == SH_OP_PIPE;
        if (!m->piped) {
            roc_spool_drop(m); /* a pipeline before this one: its last | was read */
        }
        if (m->piped && l->to_file) {
            m->in_len = 0; /* > wins over |: the next reads nothing */
        }
        if (!m->piped && call_in != NULL && call_level == sub_depth) {
            m->piped = true; /* a command roc_call runs: the input it was given */
            m->in = call_in;
            m->in_len = call_in_len;
        }
        char why[128];
        bool read = false;
        sub_ran = false;
        if (run && sh_assigns_only(l->src + l->pos)) {
            read = sh_expand_assigning(l->src + l->pos, sl, &env, why, sizeof(why));
        } else {
            read = sh_expand(l->src + l->pos, sl, &env, why, sizeof(why));
        }
        if (!read) {
            term_puts(&m->t, "rocchetto: ");
            term_puts(&m->t, why);
            term_puts(&m->t, "\r\n");
            m->status = 2;
            l->active = false;
            break;
        }
        l->pos += sl->next;
        l->op = sl->op;
        l->ran = run;
        bool negated = false; /* this ends a ! pipeline */
        if (l->negate_next && sl->op != SH_OP_PIPE) {
            negated = true;
            l->negate_next = false;
        }
        if (run && m->opt_x && (sl->argc > 0 || sl->nassign > 0)) {
            trace(m, sl);
        }
        if (run && sl->argc > 0 &&
            (strcmp(sl->argv[0], "break") == 0 || strcmp(sl->argv[0], "continue") == 0)) {
            if (!loop_jump(m, sl, sl->argv[0][0] == 'b')) {
                l->active = false;
                break;
            }
            continue;
        }
        if (run && sl->argc > 0 && strcmp(sl->argv[0], "return") == 0) {
            func_return(m, sl);
            continue;
        }
        if (run && sl->argc > 0 && strcmp(sl->argv[0], "exit") == 0 && script_exit(m, sl)) {
            continue;
        }
        if (run && sl->argc > 0 && strcmp(sl->argv[0], "exit") == 0 && m->trap_exit[0] != '\0') {
            /* the shell's EXIT trap, then this exit again (the trap gone) */
            static char again[SH_LINE_MAX + 16];
            (void)snprintf(again, sizeof(again), "%s\nexit", m->trap_exit);
            m->trap_exit[0] = '\0';
            while (l->nframes > 0) {
                pop(m);
            }
            l->src = again;
            l->pos = 0;
            l->op = SH_OP_SEMI;
            continue;
        }
        bool no_function = false; /* command name: past any function of that name */
        if (run && sl->argc > 1 && strcmp(sl->argv[0], "command") == 0 && sl->argv[1][0] != '-') {
            for (int k = 0; k < sl->argc; k++) { /* argv[argc] is NULL: it moves too */
                sl->argv[k] = sl->argv[k + 1];
            }
            sl->argc--;
            no_function = true;
        }
        /* eval, a script, a function: a call; ! turns it when it returns */
        int frames = l->nframes;
        bool ok = true;
        bool called = false;
        int fi = run && sl->argc > 0 && !no_function ? func_find(m, sl->argv[0]) : -1;
        if (run && sl->argc > 0 && strcmp(sl->argv[0], "eval") == 0) {
            ok = eval_cmd(m, sl);
            called = true;
        } else if (run && sl->argc > 0 && script_command(m, sl, &ok)) {
            called = true;
        } else if (fi >= 0 && fi < ROC_CFG_FUNCS) {
            ok = call_func(m, sl, fi);
            called = true;
        }
        if (called) {
            if (!ok) {
                l->active = false;
                break;
            }
            /* cppcheck-suppress knownConditionTrueFalse ; the call pushed a frame through m */
            if (l->nframes > frames && l->nframes <= SH_NEST_MAX) {
                l->frames[l->nframes - 1].negated = negated;
            } else {
                l->negate_due = negated; /* it ended at once (refused, or eval of nothing) */
            }
            continue;
        }
        l->to_file = sl->out_to_err; /* >&2 is as a > f for the | after it */
        if (sl->out != NULL) {
            l->to_file = true;
        }
        if (run && sl->op == SH_OP_PIPE && sl->out == NULL) {
            roc_pipe_begin(m);
        }
        if (run && sub_depth > 0 && !m->cap.on && m->nbouts == sub_base) {
            roc_pipe_begin(m); /* a $(...)'s: what it writes is the line's */
            m->cap.pipe = false;
            m->cap.keep = true;
        }
        bool nothing = false; /* no command: only a ;; */
        if (sl->argc == 0 && sl->nassign == 0 && sl->out == NULL && sl->in == NULL &&
            sl->err == NULL && !sl->err_to_out && !sl->out_to_err) {
            nothing = true;
        }
        if (run && !nothing) {
            run_command(m, sl);
        }
        if (sl->op != SH_OP_PIPE && m->pipe_cut) { /* the pipeline's end: it read what was cut */
            m->pipe_cut = false;
            if (m->status == 0) {
                m->status = 1;
            }
        }
        l->negate_due = negated;
        /* set -e looks at it: not a condition, not before && || |, not ! */
        if (run && !nothing && m->opt_e && !negated && sl->op != SH_OP_AND && sl->op != SH_OP_OR &&
            sl->op != SH_OP_PIPE && !in_condition(l)) {
            l->check_due = true;
        }
        if (m->cap.on && m->req_kind == ROC_REQ_NONE) {
            roc_capture_end(m); /* each command its own > */
        }
        if (idle(m)) {
            roc_err_end(m); /* and its own 2> */
        }
    }
    if (!l->active) {
        unwind(m); /* stopped halfway: the prompt after it sees the shell as it was */
        m->pipe_cut = false;
    }
    l->running = false;
}

void roc_exec(roc *m, char *line) {
    if (m->read_wait) { /* the line read waits for */
        roc_read_answer(m, line, false);
        return;
    }
    /* ed first, and before the trim: while it reads text the indentation is
       part of the line and an empty line is an empty line. */
    if (ed_active(m)) {
        ed_line(m, line);
        return;
    }
    /* the blanks around a line go, but not around one that continues
       another: a here-document's lines are text, empty ones too */
    while (*line == ' ' && !m->more_on) {
        line++;
    }
    size_t len = strlen(line);
    while (len > 0 && line[len - 1] == ' ' && !m->more_on) {
        len--;
        line[len] = '\0';
    }
    if (len == 0 && !m->more_on) {
        return;
    }
    if (m->sc.repl) {
        script_repl_line(m, line); /* whole: the language reads its own spaces */
        return;
    }
    /* a line that continues the one before: the two, a newline between */
    size_t n = strlen(line);
    if (m->more_len + n + 2 > sizeof(m->more)) {
        m->more_on = false;
        m->more_len = 0;
        say_why(m, "Line too long");
        return;
    }
    if (!m->more_on) {
        m->more_len = 0;
    }
    memcpy(m->more + m->more_len, line, n + 1);
    static char text[SH_SCRIPT_MAX + 1]; /* the here-documents moved in */
    char why[128];
    roc_list *l = &m->list; /* not running: its text takes the line, aliases replaced */
    bool read = sh_heredocs(m->more, text, sizeof(text), why, sizeof(why));
    if (read) {
        read = sh_aliases(text, l->text, sizeof(l->text), alias_find, m, why, sizeof(why));
    }
    if (!read || !sh_check(l->text, why, sizeof(why))) {
        if (sh_needs_more(why)) {
            m->more_len += n;
            m->more[m->more_len++] = '\n'; /* more to come: "> " asks */
            m->more[m->more_len] = '\0';
            m->more_on = true;
            return;
        }
        m->more_on = false;
        say_why(m, why);
        return;
    }
    m->more_on = false;
    m->piped = false;
    roc_spool_drop(m);
    unwind(m);
    l->words_used = 0;
    m->ncalls = 0;
    m->nlocals = 0;
    m->nbins = 0;
    m->nberrs = 0;
    l->negate_next = false;
    l->negate_due = false;
    l->check_due = false;
    m->stext_used = 0;
    block_out_drop_to(m, 0);
    l->src = l->text;
    l->pos = 0;
    l->op = SH_OP_SEMI;
    l->active = true;
    roc_list_continue(m);
}
