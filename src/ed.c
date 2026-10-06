#include "ed.h"

#include <string.h>

#include "roc.h"

/* ed answers a bad command with "?" and nothing else. That is right for a
   tool you use every day and wrong for one you reach for once a year, in
   the dark, because something is broken — so the reason follows the mark. */
static void oops(roc *m, const char *why) {
    term_puts(&m->t, "?");
    if (why != NULL) {
        term_puts(&m->t, " ");
        term_puts(&m->t, why);
    }
    term_puts(&m->t, "\r\n");
}

static void put_line(roc *m, size_t i, bool numbered) {
    const uint8_t *p = NULL;
    size_t n = 0;
    tb_line(&m->ed_l.tb, i, &p, &n);
    if (numbered) {
        term_put_u32(&m->t, (uint32_t)(i + 1));
        term_puts(&m->t, "\t");
    }
    term_write(&m->t, p, n);
    term_puts(&m->t, "\r\n");
}

/* Moves the cursor to the start of line i: every edit below works from
   there, so the buffer's own cursor is the only position ed keeps. */
static void seek_line(ed_state *e, size_t i) {
    tb_goto_line(&e->tb, i);
    tb_home(&e->tb, false);
}

static size_t last_line(const ed_state *e) {
    return e->tb.nlines > 0 ? e->tb.nlines - 1 : 0;
}

/* One address: ".", "$", a number, or nothing. Returns false when what
   follows is not an address, which is how an omitted one is told from a
   malformed one. */
static bool parse_addr(const ed_state *e, const char **s, size_t *out) {
    const char *p = *s;
    if (*p == '.') {
        *out = e->dot;
        *s = p + 1;
        return true;
    }
    if (*p == '$') {
        *out = last_line(e);
        *s = p + 1;
        return true;
    }
    if (*p < '0' || *p > '9') {
        return false;
    }
    size_t v = 0;
    while (*p >= '0' && *p <= '9') {
        v = (v * 10) + (size_t)(*p - '0');
        p++;
    }
    *out = v > 0 ? v - 1 : 0; /* the user counts from one */
    *s = p;
    return true;
}

/* The range a command applies to: "," and "%" mean the whole file, one
   address means that line, two mean the span, and nothing means the
   current line. given says an address was written, which is what tells a
   line out of range from a command that simply took the default. */
static bool parse_range(const ed_state *e, const char **s, size_t *from, size_t *to, bool *given) {
    const char *p = *s;
    *given = false;
    if (*p == ',' || *p == '%') {
        *from = 0;
        *to = last_line(e);
        *given = true;
        *s = p + 1;
        return true;
    }
    size_t a = 0;
    if (!parse_addr(e, &p, &a)) {
        *from = e->dot;
        *to = e->dot;
        return true;
    }
    *given = true;
    if (*p != ',') {
        *from = a;
        *to = a;
        *s = p;
        return true;
    }
    p++;
    size_t b = 0;
    if (!parse_addr(e, &p, &b)) {
        b = last_line(e);
    }
    if (b < a) {
        return false;
    }
    *from = a;
    *to = b;
    *s = p;
    return true;
}

static void delete_range(ed_state *e, size_t from, size_t to) {
    size_t n = to - from + 1;
    seek_line(e, from);
    while (n > 0 && e->tb.nlines > 0) {
        tb_delete_line(&e->tb);
        n--;
    }
    e->dot = from > 0 && from > last_line(e) ? last_line(e) : from;
}

/* s/old/new/ over one line; returns false when old is not there. The
   delimiter is whatever follows the s, as in ed, so a path can be
   substituted without escaping every slash. */
static bool substitute(ed_state *e, size_t i, const char *old, size_t oldn, const char *new_,
                       size_t newn, bool all) {
    const uint8_t *p = NULL;
    size_t n = 0;
    tb_line(&e->tb, i, &p, &n);
    if (oldn == 0 || oldn > n) {
        return false;
    }
    uint8_t out[TB_CAP];
    size_t len = 0;
    size_t at = 0;
    bool hit = false;
    while (at < n) {
        bool match = false;
        if (at + oldn <= n) {
            match = memcmp(p + at, old, oldn) == 0;
        }
        if (match && (all || !hit)) {
            if (len + newn > sizeof(out)) {
                return false;
            }
            memcpy(out + len, new_, newn);
            len += newn;
            at += oldn;
            hit = true;
            continue;
        }
        if (len >= sizeof(out)) {
            return false;
        }
        out[len] = p[at];
        len++;
        at++;
    }
    if (!hit) {
        return false;
    }
    seek_line(e, i);
    tb_end(&e->tb, true); /* the line, selected: inserting replaces it */
    return tb_insert(&e->tb, out, len);
}

static void do_write(roc *m, const char *arg) {
    ed_state *e = &m->ed_l;
    if (arg[0] != '\0') {
        char path[VFS_PATH_MAX];
        if (!roc_resolve_arg(m, arg, path, sizeof(path))) {
            return;
        }
        memcpy(e->path, path, strlen(path) + 1);
    }
    if (e->path[0] == '\0') {
        oops(m, "no file name");
        return;
    }
    const char *why = roc_write_check(m, e->path, e->tb.text, e->tb.len);
    if (why != NULL) {
        oops(m, why);
        return;
    }
    e->tb.dirty = false;
    term_put_u32(&m->t, (uint32_t)e->tb.len);
    term_puts(&m->t, "\r\n");
}

static void leave(roc *m) {
    m->ed_l.on = false;
    m->ed_l.inserting = false;
}

void ed_begin(roc *m, const char *path) {
    ed_state *e = &m->ed_l;
    memset(e, 0, sizeof(*e));
    tb_init(&e->tb);
    e->on = true;
    if (path[0] == '\0') {
        term_puts(&m->t, "0\r\n");
        return;
    }
    char full[VFS_PATH_MAX];
    if (!roc_resolve_arg(m, path, full, sizeof(full))) {
        leave(m);
        return;
    }
    memcpy(e->path, full, strlen(full) + 1);
    const uint8_t *data = NULL;
    size_t len = 0;
    if (!roc_find_file(m, full, &data, &len)) {
        /* a name that is not there yet is how a file is made, so this is
           not a refusal — ed says the count and waits */
        term_puts(&m->t, "0\r\n");
        return;
    }
    if (!tb_load(&e->tb, data, len)) {
        oops(m, "file too large");
        leave(m);
        return;
    }
    e->dot = last_line(e);
    term_put_u32(&m->t, (uint32_t)len);
    term_puts(&m->t, "\r\n");
}

bool ed_active(const roc *m) {
    return m->ed_l.on;
}

const char *ed_prompt(const roc *m) {
    if (!m->ed_l.on) {
        return NULL;
    }
    return m->ed_l.inserting ? "" : "*";
}

/* A line of text while a, i or c is reading. */
static void insert_text(roc *m, char *line) {
    ed_state *e = &m->ed_l;
    if (strcmp(line, ".") == 0) {
        e->inserting = false;
        e->dot = e->insert_at > 0 ? e->insert_at - 1 : 0;
        return;
    }
    seek_line(e, e->insert_at);
    size_t n = strlen(line);
    if (!tb_insert(&e->tb, (const uint8_t *)line, n) ||
        !tb_insert(&e->tb, (const uint8_t *)"\n", 1)) {
        oops(m, "no room");
        e->inserting = false;
        return;
    }
    e->insert_at++;
}

void ed_line(roc *m, char *line) {
    ed_state *e = &m->ed_l;
    if (e->inserting) {
        insert_text(m, line);
        return;
    }
    const char *s = line;
    while (*s == ' ') {
        s++;
    }
    size_t from = 0;
    size_t to = 0;
    bool given = false;
    if (!parse_range(e, &s, &from, &to, &given)) {
        oops(m, "bad address");
        return;
    }
    if (given && to > last_line(e)) {
        oops(m, "line out of range");
        return;
    }
    char cmd = *s;
    if (cmd != '\0') {
        s++;
    }
    while (*s == ' ') {
        s++;
    }

    switch (cmd) {
    case '\0': /* an address alone moves there and shows it */
        e->dot = to;
        put_line(m, e->dot, false);
        return;
    case 'p':
    case 'n': {
        size_t i = from;
        while (i <= to) {
            put_line(m, i, cmd == 'n');
            i++;
        }
        e->dot = to;
        return;
    }
    case '=':
        term_put_u32(&m->t, (uint32_t)(to + 1));
        term_puts(&m->t, "\r\n");
        return;
    case 'a':
        e->inserting = true;
        e->insert_at = to + 1;
        return;
    case 'i':
        e->inserting = true;
        e->insert_at = to;
        return;
    case 'c':
        delete_range(e, from, to);
        e->inserting = true;
        e->insert_at = from;
        return;
    case 'd':
        delete_range(e, from, to);
        return;
    case 's': {
        char delim = *s;
        if (delim == '\0') {
            oops(m, "usage: s/old/new/");
            return;
        }
        s++;
        const char *old = s;
        const char *mid = strchr(s, delim);
        if (mid == NULL) {
            oops(m, "usage: s/old/new/");
            return;
        }
        const char *new_ = mid + 1;
        const char *end = strchr(new_, delim);
        size_t newn = end != NULL ? (size_t)(end - new_) : strlen(new_);
        bool all = false;
        if (end != NULL) {
            all = end[1] == 'g';
        }
        size_t i = from;
        bool any = false;
        while (i <= to) {
            if (substitute(e, i, old, (size_t)(mid - old), new_, newn, all)) {
                any = true;
                e->dot = i;
            }
            i++;
        }
        if (!any) {
            oops(m, "no match");
        }
        return;
    }
    case 'w':
        do_write(m, s);
        return;
    case 'f':
        term_puts(&m->t, e->path[0] != '\0' ? e->path : "(no file)");
        term_puts(&m->t, "\r\n");
        return;
    case 'q':
        if (e->tb.dirty) {
            oops(m, "not written (Q leaves anyway)");
            return;
        }
        leave(m);
        return;
    case 'Q':
        leave(m);
        return;
    default:
        oops(m, "unknown command");
        return;
    }
}
