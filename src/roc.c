#include "roc.h"

#include <stdio.h>
#include <string.h>

/* ---- output ---- */

/* Text out of a stream: escapes dropped, \r dropped. */
static void capture_feed(roc *m, roc_capture *c, const uint8_t *data, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t b = data[i];
        i++;
        if (c->esc == 1) {
            if (b == '[') {
                c->esc = 2;
            } else if (b == ']' || b == 'P' || b == 'X' || b == '^' || b == '_') {
                c->esc = 3;
            } else {
                c->esc = 0; /* a two-byte sequence: gone with it */
            }
            continue;
        }
        if (c->esc == 2) {
            if (b >= 0x40 && b <= 0x7E) {
                c->esc = 0;
            }
            continue;
        }
        if (c->esc == 3) {
            if (b == 0x07) {
                c->esc = 0;
            } else if (b == 0x1B) {
                c->esc = 4;
            }
            continue;
        }
        if (c->esc == 4) {
            c->esc = b == '\\' ? 0 : 3;
            continue;
        }
        if (b == 0x1B) {
            c->esc = 1;
            continue;
        }
        if (b == '\r') {
            continue;
        }
        if (c->len >= sizeof(c->buf) && !roc_capture_spill(m, c)) {
            c->overflow = true;
            continue;
        }
        c->buf[c->len] = b;
        c->len++;
    }
}

/* Installed as the terminal's sink: while a command's output is being
   redirected it becomes file text instead of reaching the grid. A refusal
   pauses the capture, and those bytes go out the way stderr does. */
static bool capture_sink(void *ctx, const uint8_t *data, size_t n) {
    roc *m = ctx;
    if (m->cap.paused) {
        return false;
    }
    if (m->cap.on) {
        capture_feed(m, &m->cap, data, n);
        return true;
    }
    if (roc_output_captured(m)) { /* a block's > or |, under the command's own */
        capture_feed(m, &m->bcap, data, n);
        return true;
    }
    return false;
}

bool roc_output_captured(const roc *m) {
    if (m->cap.on) {
        return true;
    }
    if (m->nbouts == 0 || m->mode == ROC_MODE_APP) {
        return false;
    }
    return m->list.active;
}

bool roc_exited(const roc *m) {
    return m->exited;
}

/* ---- prompt and line editing ---- */

static const char *prompt_cwd(const roc *m, char *buf, size_t cap);

void roc_prompt(roc *m) {
    if (m->list.running) {
        return; /* a command of a list ended: the list goes on, not the prompt */
    }
    if (m->read_wait || m->sleep_ms > 0 || m->hosted) {
        return; /* read asks with no prompt; a sleep has the line wait */
    }
    if (m->int_pending && !m->list.active) { /* a ^C stopped a line: trap INT */
        m->int_pending = false;
        static char text[SH_LINE_MAX];
        memcpy(text, m->trap_int, sizeof(text));
        roc_exec(m, text);
        if (m->list.active || m->mode != ROC_MODE_LINE) {
            return;
        }
    }
    if (m->req_kind == ROC_REQ_NONE) {
        roc_err_end(m); /* the command that ended had its 2> */
    }
    if (roc_list_waiting(m)) {
        roc_list_continue(m);
        if (m->list.active || m->mode != ROC_MODE_LINE || m->exited) {
            return; /* waiting again, or something else has the terminal */
        }
    }
    const char *line_ed = ed_prompt(m);
    if (line_ed != NULL) {
        term_puts(&m->t, line_ed);
        return;
    }
    const char *repl = script_repl_prompt(m);
    if (repl != NULL) {
        term_puts(&m->t, "\x1b[35m");
        term_puts(&m->t, repl);
        term_puts(&m->t, "\x1b[0m");
        return;
    }
    if (m->more_on) {
        term_puts(&m->t, "> "); /* PS2: the line before wants more */
        return;
    }
    term_puts(&m->t, "\x1b[36m");
    term_puts(&m->t, m->user);
    if (m->host.host_name != NULL && m->host.host_name[0] != '\0') {
        term_puts(&m->t, "@");
        term_puts(&m->t, m->host.host_name);
    }
    term_puts(&m->t, "\x1b[0m:\x1b[1;34m"); /* bold: the bright blue, readable on black */
    char shown[VFS_PATH_MAX];
    term_puts(&m->t, prompt_cwd(m, shown, sizeof(shown)));
    term_puts(&m->t, "\x1b[0m$ ");
}

/* Byte length of the rune that starts at i, from its lead byte alone. */
static size_t rune_len_at(const uint8_t *buf, size_t i, size_t len) {
    uint8_t b = buf[i];
    size_t n = 1;
    if (b >= 0xF0) {
        n = 4;
    } else if (b >= 0xE0) {
        n = 3;
    } else if (b >= 0xC0) {
        n = 2;
    }
    return n > len - i ? len - i : n;
}

/* Terminal columns taken by the bytes in [from, to). */
static size_t cols_between(const uint8_t *buf, size_t from, size_t to) {
    utf8_dec d;
    utf8_dec_init(&d);
    size_t cols = 0;
    size_t i = from;
    while (i < to) {
        uint32_t cp = 0;
        int resync = 0;
        utf8_result r = utf8_dec_feed(&d, buf[i], &cp, &resync);
        if (r == UTF8_ERROR) {
            cols++; /* a stray byte shows as one cell */
            utf8_dec_init(&d);
        } else if (r == UTF8_RUNE) {
            int w = utf8_width(cp);
            cols += w > 0 ? (size_t)w : 0;
        }
        i++;
    }
    return cols;
}

/* The working directory as the prompt spells it: the home is ~. One
   function, because the line editor's cursor math has to see the same
   text the prompt prints. */
static const char *prompt_cwd(const roc *m, char *buf, size_t cap) {
    char home[VFS_PATH_MAX];
    if (!roc_home_path(m, "", home, sizeof(home))) {
        return m->cwd;
    }
    size_t hn = strlen(home);
    bool inside = strncmp(m->cwd, home, hn) == 0;
    if (inside && m->cwd[hn] != '\0' && m->cwd[hn] != '/') {
        inside = false;
    }
    if (!inside || 1 + strlen(m->cwd + hn) >= cap) {
        return m->cwd;
    }
    buf[0] = '~';
    memcpy(buf + 1, m->cwd + hn, strlen(m->cwd + hn) + 1);
    return buf;
}

/* The columns roc_prompt takes, whichever prompt it draws: ed's, the
   REPL's, or the shell's, whose host is there only when the host names one.
   The cursor arithmetic of the line rests on it. */
static size_t prompt_cols(const roc *m) {
    const char *line_ed = ed_prompt(m);
    if (line_ed != NULL) {
        return utf8_swidth(line_ed);
    }
    const char *repl = script_repl_prompt(m);
    if (repl != NULL) {
        return utf8_swidth(repl);
    }
    if (m->read_wait) {
        return 0;
    }
    if (m->more_on) {
        return 2;
    }
    char shown[VFS_PATH_MAX];
    size_t n = utf8_swidth(m->user) + 1 + utf8_swidth(prompt_cwd(m, shown, sizeof(shown))) + 2;
    if (m->host.host_name != NULL && m->host.host_name[0] != '\0') {
        n += 1 + utf8_swidth(m->host.host_name);
    }
    return n;
}

/* Where the cursor sits after `x` columns from the start of the prompt row,
   on a terminal that wraps. A row that is exactly full leaves the cursor on
   its last column, not on the next row: that is the terminal's deferred
   wrap, and the math has to agree with it or Home lands one row off. */
typedef struct {
    size_t row;
    size_t col;
} line_pos;

static line_pos pos_at(const roc *m, size_t x) {
    size_t cols = m->t.cols > 0 ? m->t.cols : 80;
    line_pos p;
    if (x > 0 && x % cols == 0) {
        p.row = (x / cols) - 1;
        p.col = cols;
        return p;
    }
    p.row = x / cols;
    p.col = x % cols;
    return p;
}

static line_pos cursor_pos(const roc *m, size_t cur) {
    return pos_at(m, prompt_cols(m) + cols_between(m->line, 0, cur));
}

static void move_rows(roc *m, size_t from, size_t to) {
    if (to < from) {
        term_puts(&m->t, "\x1b[");
        term_put_u32(&m->t, (uint32_t)(from - to));
        term_puts(&m->t, "A");
    } else if (to > from) {
        term_puts(&m->t, "\x1b[");
        term_put_u32(&m->t, (uint32_t)(to - from));
        term_puts(&m->t, "B");
    }
}

/* Moves the terminal cursor from where it is (`from`, in bytes of the line)
   to `to`, and makes `to` the edit point. Nothing is repainted. */
static void hist_leave(roc *m);

static void line_seek_from(roc *m, size_t from, size_t to) {
    line_pos a = cursor_pos(m, from);
    line_pos b = cursor_pos(m, to);
    m->line_cur = to;
    move_rows(m, a.row, b.row);
    term_puts(&m->t, "\r");
    if (b.col > 0) {
        term_puts(&m->t, "\x1b[");
        term_put_u32(&m->t, (uint32_t)b.col);
        term_puts(&m->t, "C");
    }
}

static void line_seek(roc *m, size_t to) {
    hist_leave(m);
    line_seek_from(m, m->line_cur, to);
}

/* The REPL's line in the edt's colours, a stretch of each class at a time;
   a string left open on a line before it, in the same form, still colours
   this one. */
typedef struct {
    roc *m;
    const uint8_t *line;
} line_colours;

/* cppcheck-suppress constParameterCallback ; hl_span's shape */
static void colour_span(void *user, hl_class cls, size_t from, size_t to) {
    static const int16_t fg[] = {-1, 244, 108, 139, 74}; /* plain comment string number keyword */
    const line_colours *lc = user;
    term *t = &lc->m->t;
    if (cls == HL_PLAIN || (size_t)cls >= sizeof(fg) / sizeof(fg[0])) {
        term_write(t, lc->line + from, to - from);
        return;
    }
    term_puts(t, "\x1b[38;5;");
    term_put_u32(t, (uint32_t)fg[cls]);
    term_puts(t, "m");
    term_write(t, lc->line + from, to - from);
    term_puts(t, "\x1b[39m");
}

/* The line as typed: coloured at the REPL, plain in the shell. */
static void line_text(roc *m) {
    if (!m->sc.repl) {
        term_write(&m->t, m->line, m->line_len);
        return;
    }
    hl_state h;
    hl_begin(&h, HL_FILO);
    const uint8_t *b = m->sc.repl_buf;
    size_t at = 0;
    while (at < m->sc.repl_len) { /* the form's lines so far, for what they leave open */
        size_t end = at;
        while (end < m->sc.repl_len && b[end] != '\n') {
            end++;
        }
        hl_spans(&h, b + at, end - at, NULL, NULL);
        at = end + 1;
    }
    line_colours lc = {m, m->line};
    hl_spans(&h, m->line, m->line_len, colour_span, &lc);
}

/* Repaints prompt and line from the prompt row down, then puts the cursor
   back on the edit point. The way to change anything that is not at the end
   of the line. */
static void line_paint_from(roc *m, size_t row_now) {
    move_rows(m, row_now, 0);
    term_puts(&m->t, "\r");
    roc_prompt(m);
    line_text(m);
    term_puts(&m->t, "\x1b[J");
    line_seek_from(m, m->line_len, m->line_cur);
}

static void line_redraw(roc *m) {
    line_paint_from(m, cursor_pos(m, m->line_cur).row);
}

/* Swaps the whole line for another one and paints it. The row the cursor is
   on has to be taken from the line that is still on screen. */
static void line_replace(roc *m, const uint8_t *bytes, size_t n, size_t cur) {
    size_t row_now = cursor_pos(m, m->line_cur).row;
    memcpy(m->line, bytes, n);
    m->line_len = n;
    m->line_cur = cur > n ? n : cur;
    line_paint_from(m, row_now);
}

/* ---- history ---- */

const uint8_t *roc_history_at(const roc *m, size_t i, size_t *len) {
    size_t phys = (m->hist_next + ROC_HIST_MAX - m->hist_n + i) % ROC_HIST_MAX;
    *len = m->hist_len[phys];
    return m->hist[phys];
}

static void hist_push(roc *m, const uint8_t *line, size_t n) {
    while (n > 0 && line[n - 1] == ' ') {
        n--; /* what Tab and fingers leave at the end is not the command */
    }
    if (n == 0) {
        return;
    }
    if (m->hist_n > 0) {
        size_t last_len = 0;
        const uint8_t *last = roc_history_at(m, m->hist_n - 1, &last_len);
        if (last_len == n && memcmp(last, line, n) == 0) {
            return; /* the same line twice in a row is one memory, not two */
        }
    }
    memcpy(m->hist[m->hist_next], line, n);
    m->hist_len[m->hist_next] = (uint16_t)n;
    m->hist_next = (m->hist_next + 1) % ROC_HIST_MAX;
    if (m->hist_n < ROC_HIST_MAX) {
        m->hist_n++;
    }
}

void roc_history_add(roc *m, const uint8_t *line, size_t n) {
    hist_push(m, line, n < ROC_LINE_MAX ? n : ROC_LINE_MAX);
}

/* The session's history as the lines of a file, oldest first. */
size_t roc_history_text(const roc *m, uint8_t *buf, size_t cap) {
    size_t n = 0;
    size_t i = 0;
    while (i < m->hist_n) {
        size_t len = 0;
        const uint8_t *e = roc_history_at(m, i, &len);
        if (len + 1 > cap - n) {
            break;
        }
        memcpy(buf + n, e, len);
        n += len;
        buf[n++] = '\n';
        i++;
    }
    return n;
}

/* Any edit or cursor move ends a walk through history: the line on show is
   the user's own from here, and the next Up starts a fresh search from
   wherever the cursor is then. */
static void hist_leave(roc *m) {
    m->hist_pos = m->hist_n;
}

static bool hist_matches(const roc *m, size_t i) {
    size_t len = 0;
    const uint8_t *e = roc_history_at(m, i, &len);
    if (len < m->hist_seed_len) {
        return false;
    }
    return memcmp(e, m->hist_orig, m->hist_seed_len) == 0;
}

/* With a prefix the cursor stays after it (history-beginning-search); with
   nothing typed the recalled line is taken whole, cursor at its end
   (up-line-or-history). That pair is what zsh setups bind Up to. */
static size_t hist_cursor(const roc *m, size_t len) {
    return m->hist_seed_len == 0 ? len : m->hist_seed_len;
}

static void hist_show(roc *m, size_t i) {
    size_t len = 0;
    const uint8_t *e = roc_history_at(m, i, &len);
    m->hist_pos = i;
    line_replace(m, e, len, hist_cursor(m, len));
}

static void hist_up(roc *m) {
    if (m->hist_pos == m->hist_n) {
        /* first step: what is left of the cursor is the search key, and the
           whole line is what Down returns to */
        memcpy(m->hist_orig, m->line, m->line_len);
        m->hist_orig_len = m->line_len;
        m->hist_seed_len = m->line_cur;
    }
    size_t i = m->hist_pos;
    while (i > 0) {
        i--;
        if (hist_matches(m, i)) {
            hist_show(m, i);
            return;
        }
    }
}

static void hist_down(roc *m) {
    if (m->hist_pos == m->hist_n) {
        return; /* not walking: nothing newer than now */
    }
    size_t i = m->hist_pos + 1;
    while (i < m->hist_n) {
        if (hist_matches(m, i)) {
            hist_show(m, i);
            return;
        }
        i++;
    }
    m->hist_pos = m->hist_n;
    line_replace(m, m->hist_orig, m->hist_orig_len, hist_cursor(m, m->hist_orig_len));
}

static void line_clear_display(roc *m) {
    line_pos now = cursor_pos(m, m->line_cur);
    move_rows(m, now.row, 0);
    term_puts(&m->t, "\r\x1b[J");
    roc_prompt(m);
}

static void line_reset(roc *m) {
    m->line_len = 0;
    m->line_cur = 0;
    hist_leave(m);
}

static void line_backspace(roc *m) {
    hist_leave(m);
    if (m->line_cur == 0) {
        return;
    }
    size_t n = utf8_last_rune_len(m->line, m->line_cur);
    if (n == 0) {
        n = 1;
    }
    if (m->line_cur < m->line_len || m->sc.repl) { /* at the REPL, colours may change */
        size_t row = cursor_pos(m, m->line_cur).row;
        memmove(m->line + m->line_cur - n, m->line + m->line_cur, m->line_len - m->line_cur);
        m->line_len -= n;
        m->line_cur -= n;
        line_paint_from(m, row);
        return;
    }
    /* at the end: erase in place, the cheap way */
    size_t w = cols_between(m->line, m->line_len - n, m->line_len);
    if (w == 0) {
        w = 1; /* orphan combining mark: erase one column, best effort */
    }
    m->line_len -= n;
    m->line_cur = m->line_len;
    while (w > 0) {
        term_puts(&m->t, "\b \b");
        w--;
    }
}

static void line_delete(roc *m) {
    hist_leave(m);
    if (m->line_cur >= m->line_len) {
        return;
    }
    size_t n = rune_len_at(m->line, m->line_cur, m->line_len);
    memmove(m->line + m->line_cur, m->line + m->line_cur + n, m->line_len - m->line_cur - n);
    m->line_len -= n;
    line_redraw(m);
}

static void line_insert_bytes(roc *m, const uint8_t *bytes, size_t n) {
    hist_leave(m);
    if (n > ROC_LINE_MAX - 1 - m->line_len) {
        return; /* line full */
    }
    if (m->line_cur < m->line_len) {
        memmove(m->line + m->line_cur + n, m->line + m->line_cur, m->line_len - m->line_cur);
        memcpy(m->line + m->line_cur, bytes, n);
        m->line_len += n;
        m->line_cur += n;
        line_redraw(m);
        return;
    }
    size_t row = cursor_pos(m, m->line_cur).row;
    memcpy(m->line + m->line_len, bytes, n);
    m->line_len += n;
    m->line_cur = m->line_len;
    if (m->sc.repl) {
        line_paint_from(m, row); /* what it adds may colour what is before it */
        return;
    }
    term_write(&m->t, bytes, n); /* echo */
}

static void line_insert_rune(roc *m, const uint8_t *bytes, size_t n, uint32_t cp) {
    if (cp < 0x20 || cp == 0x7F) {
        return; /* never store control bytes; Tab has its own meaning */
    }
    line_insert_bytes(m, bytes, n);
}

/* ---- Ctrl-R: the history searched backwards as it is typed ---- */

static bool contains(const uint8_t *hay, size_t n, const uint8_t *needle, size_t k) {
    if (k == 0) {
        return true;
    }
    for (size_t i = 0; i + k <= n; i++) {
        if (memcmp(hay + i, needle, k) == 0) {
            return true;
        }
    }
    return false;
}

/* The search line where the line was: "(reverse-i-search)`q': match". */
static void search_paint(roc *m) {
    move_rows(m, m->srch_rows, 0);
    term_puts(&m->t, "\r\x1b[J");
    const char *head = m->srch_failed ? "(failed reverse-i-search)`" : "(reverse-i-search)`";
    term_puts(&m->t, head);
    term_write(&m->t, m->srch_q, m->srch_qlen);
    term_puts(&m->t, "': ");
    term_write(&m->t, m->line, m->line_len);
    size_t w = strlen(head) + cols_between(m->srch_q, 0, m->srch_qlen) + 3 +
               cols_between(m->line, 0, m->line_len);
    size_t cols = m->t.cols > 0 ? m->t.cols : 80;
    m->srch_rows = (w - 1) / cols; /* the row the cursor ends on (w counts the head) */
}

/* The newest entry, from index from down, holding what is looked for, on
   show; false, the line left as it was, when there is none. */
static bool search_from(roc *m, size_t from) {
    size_t i = from + 1;
    while (i > 0 && m->hist_n > 0) {
        i--;
        if (i >= m->hist_n) {
            continue;
        }
        size_t len = 0;
        const uint8_t *e = roc_history_at(m, i, &len);
        if (contains(e, len, m->srch_q, m->srch_qlen)) {
            memcpy(m->line, e, len);
            m->line_len = len;
            m->line_cur = len;
            m->srch_at = i;
            return true;
        }
    }
    return false;
}

static void search_begin(roc *m) {
    size_t row = cursor_pos(m, m->line_cur).row;
    memcpy(m->hist_orig, m->line, m->line_len);
    m->hist_orig_len = m->line_len;
    hist_leave(m);
    m->srch_on = true;
    m->srch_failed = false;
    m->srch_qlen = 0;
    m->srch_at = m->hist_n;
    m->srch_rows = row;
    m->line_cur = m->line_len;
    search_paint(m);
}

/* Out of the search: the line found (or, cancelled, the one before it) back
   at the prompt, ready to be edited. */
static void search_end(roc *m, bool keep) {
    move_rows(m, m->srch_rows, 0);
    term_puts(&m->t, "\r\x1b[J");
    m->srch_on = false;
    if (!keep) {
        memcpy(m->line, m->hist_orig, m->hist_orig_len);
        m->line_len = m->hist_orig_len;
    }
    m->line_cur = m->line_len;
    roc_line_repaint(m);
}

/* A rune while searching; false when the search is over and the rune is for
   the line, as readline gives it. */
static bool search_rune(roc *m, uint32_t cp, const uint8_t *bytes, size_t blen) {
    if (cp == 0x12) { /* Ctrl-R again: the one before */
        if (m->srch_qlen > 0) {
            m->srch_failed = true;
            if (m->srch_at > 0 && search_from(m, m->srch_at - 1)) {
                m->srch_failed = false;
            }
        }
        search_paint(m);
        return true;
    }
    if (cp == 0x07 || cp == 0x03) { /* Ctrl-G, Ctrl-C: back to the line before */
        search_end(m, false);
        return true;
    }
    if (cp == 0x7f || cp == 0x08) {
        size_t n = utf8_last_rune_len(m->srch_q, m->srch_qlen);
        if (n == 0 && m->srch_qlen > 0) {
            n = 1; /* a byte that is no rune: one at a time */
        }
        m->srch_qlen -= n;
        m->srch_failed = false;
        if (m->srch_qlen == 0 || !search_from(m, m->hist_n - 1)) {
            memcpy(m->line, m->hist_orig, m->hist_orig_len);
            m->line_len = m->hist_orig_len;
        }
        search_paint(m);
        return true;
    }
    if (cp < 0x20) {
        search_end(m, true); /* Enter, Ctrl-A, Ctrl-E...: on the line found */
        return false;
    }
    if (blen > sizeof(m->srch_q) - m->srch_qlen) {
        return true;
    }
    memcpy(m->srch_q + m->srch_qlen, bytes, blen);
    m->srch_qlen += blen;
    size_t from = m->srch_at < m->hist_n ? m->srch_at : m->hist_n - 1;
    m->srch_failed = true;
    if (m->hist_n > 0 && search_from(m, from)) {
        m->srch_failed = false;
    }
    search_paint(m);
    return true;
}

/* Prompt and line drawn where the cursor is now, edit point restored: for
   after something else was printed (a screen clear, a list of matches). */
void roc_line_repaint(roc *m) {
    roc_prompt(m);
    line_text(m);
    line_seek_from(m, m->line_len, m->line_cur);
}

/* ---- completion ---- */

typedef struct {
    const char *text; /* the whole candidate, prefix included */
    size_t len;
    bool dir;
} match;

enum { MATCH_MAX = 64 };

static bool starts_with(const char *s, size_t slen, const uint8_t *prefix, size_t plen) {
    if (slen < plen || memcmp(s, prefix, plen) != 0) {
        return false;
    }
    return true;
}

static size_t command_matches(roc *m, const uint8_t *prefix, size_t plen, match *out) {
    size_t n = 0;
    size_t i = 0;
    while (i < roc_command_count() && n < MATCH_MAX) {
        const char *name = roc_command_name(i);
        i++;
        if (starts_with(name, strlen(name), prefix, plen) && roc_command_here(m, name)) {
            out[n].text = name;
            out[n].len = strlen(name);
            out[n].dir = false;
            n++;
        }
    }
    /* the host's own (a terminal app's ssh) */
    const char *h = m->host.commands != NULL ? m->host.commands : "";
    while (*h != '\0' && n < MATCH_MAX) {
        size_t hl = strcspn(h, " ");
        if (hl > 0 && starts_with(h, hl, prefix, plen)) {
            out[n].text = h;
            out[n].len = hl;
            out[n].dir = false;
            n++;
        }
        h += hl + strspn(h + hl, " ");
    }
    /* and every program in the directories of PATH, by its name */
    const char *p = sh_var_get(&m->vars, "PATH", 4);
    p = p != NULL ? p : "/bin";
    while (n < MATCH_MAX) {
        size_t pl = strcspn(p, ":");
        char dir[VFS_PATH_MAX];
        char want[VFS_PATH_MAX];
        (void)snprintf(want, sizeof(want), "%.*s", pl == 0 ? 1 : (int)pl, pl == 0 ? "." : p);
        if (vfs_resolve(m->cwd, want, dir, sizeof(dir))) {
            roc_sync_dir(m, dir);
            for (i = 0; i < m->fs.nnodes && n < MATCH_MAX; i++) {
                const vfs_node *node = &m->fs.nodes[i];
                const char *base = strrchr(node->path, '/');
                if (node->dir || !vfs_is_child(node, dir) || base == NULL ||
                    !starts_with(base + 1, strlen(base + 1), prefix, plen) ||
                    roc_exec_kind(m, node->path, false) == 0) {
                    continue;
                }
                out[n].text = base + 1;
                out[n].len = strlen(base + 1);
                out[n].dir = false;
                n++;
            }
        }
        if (p[pl] == '\0') {
            break;
        }
        p += pl + 1;
    }
    return n;
}

/* The token is a path: what comes before its last '/' picks the directory,
   what comes after is the prefix to match against that directory's entries. */
static size_t file_matches(roc *m, const uint8_t *tok, size_t tlen, size_t *prefix_at, match *out) {
    size_t slash = tlen;
    size_t i = tlen;
    while (i > 0) {
        i--;
        if (tok[i] == '/') {
            slash = i;
            break;
        }
    }
    char dirpart[VFS_PATH_MAX];
    if (slash == tlen) {
        dirpart[0] = '\0';
    } else {
        size_t n = slash == 0 ? 1 : slash; /* "/x" keeps the root */
        if (n >= sizeof(dirpart)) {
            return 0;
        }
        memcpy(dirpart, tok, n);
        dirpart[n] = '\0';
    }
    char dir[VFS_PATH_MAX];
    if (dirpart[0] == '\0') {
        size_t n = strlen(m->cwd); /* no slash: the directory is here */
        if (n >= sizeof(dir)) {
            return 0;
        }
        memcpy(dir, m->cwd, n + 1);
    } else if (!vfs_resolve(m->cwd, dirpart, dir, sizeof(dir))) {
        return 0;
    }
    *prefix_at = slash == tlen ? 0 : slash + 1;
    const uint8_t *prefix = tok + *prefix_at;
    size_t plen = tlen - *prefix_at;
    size_t n = 0;
    roc_sync_dir(m, dir);
    i = 0;
    while (i < m->fs.nnodes && n < MATCH_MAX) {
        const vfs_node *node = &m->fs.nodes[i];
        i++;
        if (!vfs_is_child(node, dir)) {
            continue;
        }
        if (vfs_hidden(node) && (plen == 0 || prefix[0] != '.')) {
            continue; /* dot files complete only when the dot was typed */
        }
        const char *base = vfs_base(node);
        if (starts_with(base, strlen(base), prefix, plen)) {
            out[n].text = base;
            out[n].len = strlen(base);
            out[n].dir = node->dir;
            n++;
        }
    }
    return n;
}

/* The names Filo knows at the REPL, for Tab: its special forms, the
   builtins of the context the REPL runs in, and the globals defined there,
   the REPL's own def among them. */
static size_t filo_matches(roc *m, const uint8_t *prefix, size_t plen, match *out) {
    static const char *const forms[] = {
        "def", "fn",  "let", "letv", "if",     "cond", "else",
        "do",  "set", "and", "or",   "return", "exit",
    };
    size_t n = 0;
    for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]) && n < MATCH_MAX; i++) {
        if (starts_with(forms[i], strlen(forms[i]), prefix, plen)) {
            out[n].text = forms[i];
            out[n].len = strlen(forms[i]);
            out[n].dir = false;
            n++;
        }
    }
    const filo_ctx *ctx = &m->sc.ctx;
    for (uint32_t i = 0; i < ctx->nbuiltins && n < MATCH_MAX; i++) {
        const char *name = ctx->builtins[i].name;
        if (starts_with(name, strlen(name), prefix, plen)) {
            out[n].text = name;
            out[n].len = strlen(name);
            out[n].dir = false;
            n++;
        }
    }
    for (uint32_t i = 0; i < ctx->nsymbols && n < MATCH_MAX; i++) {
        const char *name = ctx->symbols[i];
        if (ctx->defined[i] && starts_with(name, strlen(name), prefix, plen)) {
            out[n].text = name;
            out[n].len = strlen(name);
            out[n].dir = false;
            n++;
        }
    }
    return n;
}

static size_t common_prefix(const match *ms, size_t n) {
    size_t len = ms[0].len;
    size_t i = 1;
    while (i < n) {
        size_t k = 0;
        while (k < len && k < ms[i].len && ms[0].text[k] == ms[i].text[k]) {
            k++;
        }
        len = k;
        i++;
    }
    return len;
}

/* What Tab found for the word whose last have bytes are typed: one match is
   finished off (a space after it, nothing after a directory, whose slash is
   already there); several share what they have in common, and when that adds
   nothing the names are listed, the way a shell does it. */
static void offer(roc *m, const match *ms, size_t n, size_t have) {
    if (n == 0) {
        return;
    }
    size_t common = common_prefix(ms, n);
    if (common > have) {
        line_insert_bytes(m, (const uint8_t *)ms[0].text + have, common - have);
    }
    if (n == 1) {
        line_insert_bytes(m, (const uint8_t *)(ms[0].dir ? "/" : " "), 1);
        return;
    }
    if (common > have) {
        return; /* something was added; the next Tab can tell more */
    }
    /* nothing more to add: show what the choice is */
    line_seek(m, m->line_len);
    term_puts(&m->t, "\r\n");
    size_t i = 0;
    while (i < n) {
        term_write(&m->t, (const uint8_t *)ms[i].text, ms[i].len);
        if (ms[i].dir) {
            term_puts(&m->t, "/");
        }
        term_puts(&m->t, i + 1 < n ? "  " : "\r\n");
        i++;
    }
    roc_line_repaint(m);
}

/* A byte that ends a word of Filo: a blank, a paren, a quote. */
static bool filo_delim(uint8_t b) {
    if (b == '\0') {
        return false;
    }
    return strchr(" ()\"'", b) != NULL;
}

/* Tab at the REPL: the Filo name being typed, outside a string. */
static void repl_complete(roc *m) {
    size_t cur = m->line_cur;
    if (cur < m->line_len && !filo_delim(m->line[cur])) {
        return; /* only at the end of a word */
    }
    size_t quotes = 0;
    for (size_t i = 0; i < cur; i++) {
        if (m->line[i] == '"' && (i == 0 || m->line[i - 1] != '\\')) {
            quotes++;
        }
    }
    if (quotes % 2 == 1) {
        return; /* inside a string: its words are not names */
    }
    size_t start = cur;
    while (start > 0 && !filo_delim(m->line[start - 1])) {
        start--;
    }
    match ms[MATCH_MAX];
    size_t n = filo_matches(m, m->line + start, cur - start, ms);
    offer(m, ms, n, cur - start);
}

/* Tab at the end of a word. The first word is a command and completes from
   the command table; any other word is a path and completes from the
   directory it names, relative to cwd. At the REPL, a word is a Filo name. */
static void line_complete(roc *m) {
    if (m->sc.repl) {
        repl_complete(m);
        return;
    }
    size_t cur = m->line_cur;
    if (cur < m->line_len && m->line[cur] != ' ') {
        return; /* only at the end of a word */
    }
    size_t start = cur;
    while (start > 0 && m->line[start - 1] != ' ') {
        start--;
    }
    size_t before = start;
    while (before > 0 && m->line[before - 1] == ' ') {
        before--;
    }
    const uint8_t *tok = m->line + start;
    size_t tlen = cur - start;
    match ms[MATCH_MAX];
    size_t n;
    size_t prefix_at = 0;
    bool is_command = before == 0;
    if (is_command) {
        n = command_matches(m, tok, tlen, ms);
    } else {
        if (!m->indexed) {
            return;
        }
        n = file_matches(m, tok, tlen, &prefix_at, ms);
    }
    offer(m, ms, n, tlen - prefix_at);
}

/* Arrows, Home, End and Delete at the prompt; the rest of the synthetic keys
   mean nothing here. */
static void line_key(roc *m, uint32_t cp) {
    switch (cp) {
    case FT_KEY_UP:
        hist_up(m);
        return;
    case FT_KEY_DOWN:
        hist_down(m);
        return;
    case FT_KEY_LEFT:
        if (m->line_cur > 0) {
            size_t n = utf8_last_rune_len(m->line, m->line_cur);
            line_seek(m, m->line_cur - (n > 0 ? n : 1));
        }
        return;
    case FT_KEY_RIGHT:
        if (m->line_cur < m->line_len) {
            line_seek(m, m->line_cur + rune_len_at(m->line, m->line_cur, m->line_len));
        }
        return;
    case FT_KEY_HOME:
        line_seek(m, 0);
        return;
    case FT_KEY_END:
        line_seek(m, m->line_len);
        return;
    case FT_KEY_DEL:
        line_delete(m);
        return;
    default:
        return;
    }
}

static void line_enter(roc *m) {
    if (m->line_cur < m->line_len) {
        line_seek(m, m->line_len);
    }
    term_puts(&m->t, "\r\n");
    if (!m->read_wait) { /* an answer to read is not a command to recall */
        hist_push(m, m->line, m->line_len);
    }
    hist_leave(m);
    m->line[m->line_len] = '\0';
    line_reset(m);
    roc_exec(m, (char *)m->line);
    if (m->cap.on && m->req_kind == ROC_REQ_NONE) {
        roc_capture_end(m); /* a command that finished here; a reader ends its own */
    }
    if (m->mode == ROC_MODE_LINE && !m->exited) {
        roc_prompt(m);
    }
}

/* ---- reader (cat / index streaming) ---- */

static void reader_begin(roc *m, roc_req_kind kind, const char *path) {
    if (m->t.napps == 0) {
        m->mode = ROC_MODE_READ; /* app-context reads keep the app's keys */
    }
    m->req_kind = kind;
    m->req_seq++;
    if (m->req_seq == 0) {
        m->req_seq = 1;
    }
    m->req_id = m->req_seq;
    m->rd_state = RD_START;
    m->rd_line_len = 0;
    m->rd_last = 0;
    m->fm_fence[0] = '\0';
    size_t plen = strlen(path);
    bool markdown = false;
    if (plen > 3 && memcmp(path + plen - 3, ".md", 3) == 0) {
        markdown = true;
    }
    if (!markdown && kind == ROC_REQ_LESS) {
        /* source read on the screen is coloured; cat is not, since what it
           writes may be going into a pipe or a file */
        md_source(&m->md, hl_lang_of_name(path));
    } else {
        md_reset(&m->md, markdown);
    }
    m->host.request(m->host.ctx, m->req_id, path);
}

void roc_reader_begin_cat(roc *m, const char *path) {
    reader_begin(m, ROC_REQ_CAT, path);
}

void roc_reader_begin_script(roc *m, const char *path) {
    reader_begin(m, ROC_REQ_SCRIPT, path);
}

void roc_reader_begin_less(roc *m, const char *path) {
    pager_reset(&m->pg, path);
    reader_begin(m, ROC_REQ_LESS, path);
}

void roc_app_enter(roc *m, const term_app *app) {
    if (m->cap.on) {
        roc_capture_end(m); /* a screen is not output: whatever came before it is */
    }
    if (term_app_enter(&m->t, app, m)) {
        m->mode = ROC_MODE_APP;
    }
}

void roc_flash(roc *m, const char *note) {
    term_flash(&m->t, note);
}

void roc_app_leave(roc *m, const char *note) {
    m->req_id = 0;
    m->req_kind = ROC_REQ_NONE;
    if (!term_app_leave(&m->t)) {
        term_flash(&m->t, note);
        return;
    }
    /* The restored cursor already sits right after the last main-buffer
       output, so the note and prompt flow on continuously — no parking, or
       the gap turns into blank lines in the scrollback. */
    m->mode = ROC_MODE_LINE;
    if (note != NULL) {
        term_puts(&m->t, note);
    }
    if (!m->exited) {
        roc_prompt(m);
    }
}

/* The home's files and directories gone from the store and the index:
   for home reload, which puts the kept ones in their place. */
static void home_clear(roc *m, const char *home) {
    size_t hl = strlen(home);
    for (size_t i = m->uf.nfiles; i > 0; i--) {
        const char *p = m->uf.files[i - 1].path;
        if (strncmp(p, home, hl) == 0 && p[hl] == '/') {
            char path[VFS_PATH_MAX];
            memcpy(path, p, strlen(p) + 1);
            (void)ufs_remove(&m->uf, path);
            (void)vfs_remove(&m->fs, path);
        }
    }
    for (size_t i = m->fs.nnodes; i > 0; i--) {
        const char *p = m->fs.nodes[i - 1].path;
        if (strncmp(p, home, hl) == 0 && p[hl] == '/') {
            char path[VFS_PATH_MAX];
            memcpy(path, p, strlen(p) + 1);
            (void)vfs_remove(&m->fs, path);
        }
    }
}

bool roc_home_reload(roc *m) {
    if (m->host.store_put == NULL) {
        roc_err(m, "home", "reload", "This terminal keeps nothing between visits");
        return false;
    }
    m->home_len = 0;
    m->home_overflow = false;
    reader_begin(m, ROC_REQ_HOME_RELOAD, ROC_HOME_PATH);
    return true;
}

static void reader_end(roc *m, const char *note) {
    m->req_id = 0;
    m->req_kind = ROC_REQ_NONE;
    if (m->cap.on) {
        roc_capture_end(m);
    }
    if (m->t.napps > 0) {
        roc_flash(m, note); /* the waiting app redraws with the note */
        return;
    }
    m->mode = ROC_MODE_LINE;
    if (note != NULL) {
        term_puts(&m->t, note);
    }
    roc_prompt(m);
}

static void reader_abort(roc *m) {
    if (m->req_kind == ROC_REQ_NONE) {
        return;
    }
    m->list.active = false; /* ^C ends the whole line, as in sh */
    m->int_pending = m->trap_int[0] != '\0';
    reader_end(m, "^C\r\n");
}

void roc_reader_abort_quiet(roc *m) {
    m->req_id = 0;
    m->req_kind = ROC_REQ_NONE;
}

/* The raw sink: rendered bytes land here and go to the pager or the screen.
   Everything upstream of it is markdown; nothing downstream of it is. */
void roc_body_emit(roc *m, const uint8_t *data, size_t n) {
    if (m->req_kind == ROC_REQ_LESS) {
        pager_load(&m->pg, data, n);
        return;
    }
    size_t start = 0;
    size_t i = 0;
    while (i < n) {
        if (data[i] == '\n') {
            term_write(&m->t, data + start, i - start);
            term_puts(&m->t, "\r\n");
            start = i + 1;
        }
        i++;
    }
    term_write(&m->t, data + start, n - start);
    if (n > 0) {
        m->rd_last = data[n - 1];
    }
}

static void body_write(roc *m, const uint8_t *data, size_t n) {
    md_feed(&m->md, data, n);
}

/* Shows bytes the shell already holds — a file of its own tree — through
   the same pipeline a fetched file goes through: markdown rendered by name,
   the pager for less, the terminal for cat. */
/* The pager as an app of the stack, which owns the alternate screen. */
static void pager_on_key(void *ctx, uint32_t cp) {
    roc *m = ctx;
    if (!pager_key(&m->pg, &m->t, cp)) {
        pager_hide(&m->t);
        roc_app_leave(m, NULL);
    }
}

static void pager_on_resize(void *ctx) {
    roc *m = ctx;
    pager_resize(&m->pg, &m->t);
}

static const term_app pager_app = {
    .on_key = pager_on_key,
    .on_resize = pager_on_resize,
};

static void show_pager(roc *m) {
    roc_app_enter(m, &pager_app);
    pager_show(&m->pg, &m->t);
}

void roc_show_bytes(roc *m, bool less, const char *name, const uint8_t *data, size_t n) {
    if (!less && roc_output_captured(m)) { /* cat > f, cat | x: the bytes, not their look */
        roc_out(m, data, n);
        return;
    }
    size_t plen = strlen(name);
    bool markdown = false;
    if (plen > 3 && memcmp(name + plen - 3, ".md", 3) == 0) {
        markdown = true;
    }
    m->req_kind = less ? ROC_REQ_LESS : ROC_REQ_CAT;
    m->rd_last = 0;
    if (less) {
        pager_reset(&m->pg, name);
    }
    if (!markdown && less) {
        md_source(&m->md, hl_lang_of_name(name));
    } else {
        md_reset(&m->md, markdown);
    }
    body_write(m, data, n);
    md_end(&m->md);
    m->req_kind = ROC_REQ_NONE;
    if (less) {
        show_pager(m);
        return;
    }
    if (n > 0 && m->rd_last != '\n') { /* nothing to show is no line */
        term_puts(&m->t, "\r\n");
    }
}

static bool is_fence(const uint8_t *line, size_t n, char fence[4]) {
    size_t len = n;
    if (len > 0 && line[len - 1] == '\r') {
        len--;
    }
    if (len != 3) {
        return false;
    }
    if (memcmp(line, "+++", 3) != 0 && memcmp(line, "---", 3) != 0) {
        return false;
    }
    memcpy(fence, line, 3);
    fence[3] = '\0';
    return true;
}

/* START/FM_SKIP work line by line through rd_line; BODY streams. */
static void cat_feed(roc *m, const uint8_t *data, size_t n) {
    size_t i = 0;
    /* strip BOM at the very beginning */
    if (m->rd_state == RD_START && m->rd_line_len == 0) {
        static const uint8_t bom[3] = {0xEF, 0xBB, 0xBF};
        if (n >= 3 && memcmp(data, bom, 3) == 0) {
            i = 3;
        }
    }
    while (i < n) {
        if (m->rd_state == RD_BODY) {
            body_write(m, data + i, n - i);
            return;
        }
        uint8_t b = data[i];
        i++;
        if (b != '\n') {
            if (m->rd_line_len < ROC_RDLINE_MAX) {
                m->rd_line[m->rd_line_len] = b;
                m->rd_line_len++;
                continue;
            }
            /* first line too long to be a fence: flush as body */
            if (m->rd_state == RD_START) {
                body_write(m, m->rd_line, m->rd_line_len);
                body_write(m, &b, 1);
                m->rd_state = RD_BODY;
            }
            m->rd_line_len = 0;
            continue;
        }
        if (m->rd_state == RD_START) {
            char fence[4];
            if (is_fence(m->rd_line, m->rd_line_len, fence)) {
                memcpy(m->fm_fence, fence, 4);
                m->rd_state = RD_FM_SKIP;
            } else {
                body_write(m, m->rd_line, m->rd_line_len);
                body_write(m, (const uint8_t *)"\n", 1);
                m->rd_state = RD_BODY;
            }
            m->rd_line_len = 0;
            continue;
        }
        /* RD_FM_SKIP */
        char fence[4];
        if (is_fence(m->rd_line, m->rd_line_len, fence) && strcmp(fence, m->fm_fence) == 0) {
            m->rd_state = RD_BODY;
        }
        m->rd_line_len = 0;
    }
}

void roc_feed(roc *m, uint32_t req_id, const uint8_t *data, size_t n) {
    if (req_id != m->req_id || m->req_kind == ROC_REQ_NONE || n > ROC_FEED_MAX) {
        return; /* stale or oversized feed */
    }
    if (m->req_kind == ROC_REQ_INDEX) {
        if (!vfs_append(&m->fs, data, n)) {
            term_puts(&m->t, "rocchetto: index too large\r\n");
            reader_end(m, NULL);
        }
        return;
    }
    if (m->req_kind == ROC_REQ_SCRIPT) {
        script_collect(m, data, n);
        return;
    }
    if (m->req_kind == ROC_REQ_HOME || m->req_kind == ROC_REQ_HOME_RELOAD) {
        if (n > sizeof(m->home_buf) - m->home_len) {
            m->home_overflow = true; /* not ours: nothing of it is trusted */
            return;
        }
        memcpy(m->home_buf + m->home_len, data, n);
        m->home_len += n;
        return;
    }
    cat_feed(m, data, n);
}

/* ~/.profile, when the user has one: run in this shell, as sh's login does,
   so what it sets (variables, aliases, functions, PATH) stays. */
static void profile_run(roc *m) {
    char path[VFS_PATH_MAX];
    const uint8_t *data = NULL;
    size_t len = 0;
    if (!roc_home_path(m, ".profile", path, sizeof(path)) || !roc_find_file(m, path, &data, &len)) {
        return;
    }
    static char line[VFS_PATH_MAX + 8];
    (void)snprintf(line, sizeof(line), ". '%s'", path);
    roc_exec(m, line);
    if (m->cap.on && m->req_kind == ROC_REQ_NONE) {
        roc_capture_end(m);
    }
}

/* The index is in, the home too when there is one: the shell opens. With no
   layer's home in the build there is nothing to open — the prompt is the
   session. */
static void boot_done(roc *m) {
    profile_run(m);
    if (m->mode != ROC_MODE_LINE || m->exited) {
        return; /* the profile waits on something, or ended the session */
    }
    if ((m->flags & ROC_F_PROMPT) != 0 || roc_layer_spec.home == NULL) {
        roc_prompt(m);
        return;
    }
#if ROC_APP_SCREENS
    if ((m->flags & ROC_F_NO_SPLASH) == 0 && roc_layer_spec.boot != NULL) {
        roc_layer_spec.boot(m); /* it ends on home */
        return;
    }
    screen_enter(m, roc_layer_spec.home); /* the layer's face; the shell is one of its areas */
#else
    roc_prompt(m);
#endif
}

void roc_stream_data(roc *m, const uint8_t *data, size_t n) {
    if (roc_layer_spec.stream_data != NULL) {
        roc_layer_spec.stream_data(m, data, n);
    }
}

void roc_stream_event(roc *m, uint32_t event) {
    if (roc_layer_spec.stream_event != NULL) {
        roc_layer_spec.stream_event(m, event);
    }
}

void roc_feed_eof(roc *m, uint32_t req_id) {
    if (req_id != m->req_id || m->req_kind == ROC_REQ_NONE) {
        return;
    }
    if (m->req_kind == ROC_REQ_SCRIPT) {
        /* back at the prompt first: the script may open the pager itself */
        m->req_id = 0;
        m->req_kind = ROC_REQ_NONE;
        if (m->t.napps == 0) {
            m->mode = ROC_MODE_LINE;
        }
        script_collected(m);
        if (m->cap.on) {
            roc_capture_end(m); /* wc /pub/x.md > f: the file is what the filter wrote */
        }
        if (m->mode == ROC_MODE_LINE) {
            roc_prompt(m);
        }
        return;
    }
    if (m->req_kind == ROC_REQ_INDEX) {
        size_t n = vfs_parse(&m->fs);
        tree_mount(m); /* the shell's own files, next to the site's */
        m->indexed = true;
        /* the shell opens at home, as shells do */
        char home[VFS_PATH_MAX];
        if (roc_home_path(m, "", home, sizeof(home)) && roc_lookup(m, home) != NULL) {
            memcpy(m->cwd, home, strlen(home) + 1);
        }
        if ((m->flags & ROC_F_PROMPT) == 0) { /* an app's shell opens quiet */
            term_put_u32(&m->t, (uint32_t)n);
            term_puts(&m->t, " entries indexed.\r\n");
        }
        m->req_id = 0;
        m->req_kind = ROC_REQ_NONE;
        m->mode = ROC_MODE_LINE;
        if (m->host.store_put != NULL) {
            /* one request at a time: the kept home comes after the index */
            m->home_len = 0;
            m->home_overflow = false;
            reader_begin(m, ROC_REQ_HOME, ROC_HOME_PATH);
            return;
        }
        boot_done(m);
        return;
    }
    if (m->req_kind == ROC_REQ_HOME) {
        m->req_id = 0;
        m->req_kind = ROC_REQ_NONE;
        m->mode = ROC_MODE_LINE;
        char home[VFS_PATH_MAX];
        if (!m->home_overflow && roc_home_path(m, "", home, sizeof(home))) {
            size_t n = home_unpack(&m->uf, &m->fs, home, m->home_buf, m->home_len, NULL);
            if (n > 0) {
                term_put_u32(&m->t, (uint32_t)n);
                term_puts(&m->t, n == 1 ? " file back in " : " files back in ");
                term_puts(&m->t, home);
                term_puts(&m->t, ".\r\n");
            }
        }
        boot_done(m);
        return;
    }
    if (m->req_kind == ROC_REQ_HOME_RELOAD) {
        char home[VFS_PATH_MAX];
        if (m->home_overflow || !roc_home_path(m, "", home, sizeof(home))) {
            reader_end(m, "rocchetto: home: the kept home could not be read\r\n");
            return;
        }
        home_clear(m, home);
        size_t dirs = 0;
        size_t n = home_unpack(&m->uf, &m->fs, home, m->home_buf, m->home_len, &dirs);
        m->home_conflict = false;
        m->home_len = 0;
        term_put_u32(&m->t, (uint32_t)n);
        term_puts(&m->t, n == 1 ? " file back in " : " files back in ");
        term_puts(&m->t, home);
        reader_end(m, ", the kept home.\r\n");
        return;
    }
    /* flush a body that did not end in newline */
    if (m->rd_state != RD_BODY && m->rd_line_len > 0) {
        body_write(m, m->rd_line, m->rd_line_len);
    }
    md_end(&m->md);
    if (m->req_kind == ROC_REQ_LESS) {
        m->req_id = 0;
        m->req_kind = ROC_REQ_NONE;
        show_pager(m);
        return;
    }
    if (m->rd_last != '\n') {
        term_puts(&m->t, "\r\n"); /* a body ending in \n already broke the line */
    }
    reader_end(m, NULL);
}

void roc_feed_fail(roc *m, uint32_t req_id) {
    if (req_id != m->req_id || m->req_kind == ROC_REQ_NONE) {
        return;
    }
    if (m->req_kind == ROC_REQ_INDEX) {
        m->indexed = false;
        reader_end(m, "rocchetto: no index; ls/cat unavailable\r\n");
        return;
    }
    if (m->req_kind == ROC_REQ_HOME_RELOAD) {
        reader_end(m, "rocchetto: home: nothing is kept in this browser\r\n");
        return;
    }
    if (m->req_kind == ROC_REQ_HOME) {
        /* the usual first visit: nothing kept yet, nothing to say */
        m->req_id = 0;
        m->req_kind = ROC_REQ_NONE;
        m->mode = ROC_MODE_LINE;
        boot_done(m);
        return;
    }
    reader_end(m, "rocchetto: read error\r\n");
}

/* ---- input state machine ---- */

static void handle_rune(roc *m, uint32_t cp) {
    const uint8_t *bytes = m->kin.dec.bytes;
    size_t blen = m->kin.dec.have;
    if (m->mode == ROC_MODE_APP) {
        if (m->req_kind != ROC_REQ_NONE) {
            /* an app is waiting for a file; only Ctrl-C interrupts */
            if (cp == 0x03) {
                reader_abort(m);
            }
            return;
        }
        if (cp == 0x03) { /* Ctrl-C behaves as ESC inside apps */
            cp = FT_KEY_ESC;
        }
        term_app_top(&m->t)->on_key(term_app_ctx(&m->t), cp);
        return;
    }
    if (m->mode == ROC_MODE_READ) {
        if (cp == 0x03) { /* Ctrl-C */
            reader_abort(m);
        }
        return; /* reading: everything else is ignored */
    }
    if (m->hosted) {
        return; /* the host's command has the keys */
    }
    if (m->sleep_ms > 0 && m->mode == ROC_MODE_LINE) { /* a sleep: only ^C ends it */
        if (cp == 0x03) {
            m->sleep_ms = 0;
            m->list.active = false;
            term_puts(&m->t, "^C\r\n");
            m->status = 130;
            m->int_pending = m->trap_int[0] != '\0';
            roc_prompt(m);
        }
        return;
    }
    if (roc_list_waiting(m)) { /* a long loop, back at the next tick */
        if (cp == 0x03) {
            m->list.active = false;
            term_puts(&m->t, "^C\r\n");
            m->status = 130;
            m->int_pending = m->trap_int[0] != '\0';
            roc_prompt(m);
        }
        return;
    }
    if (m->srch_on && search_rune(m, cp, bytes, blen)) {
        return;
    }
    if (cp >= FT_KEY_UP && cp <= FT_KEY_ESC) {
        line_key(m, cp);
        return;
    }
    switch (cp) {
    case 0x12: /* Ctrl-R */
        search_begin(m);
        return;
    case 0x01: /* Ctrl-A */
        line_seek(m, 0);
        return;
    case 0x05: /* Ctrl-E */
        line_seek(m, m->line_len);
        return;
    case '\r':
    case '\n':
        line_enter(m);
        return;
    case 0x03: /* Ctrl-C */
        term_puts(&m->t, "^C\r\n");
        m->more_on = false; /* the lines before go too */
        if (m->read_wait) {
            m->read_wait = false;
            m->list.active = false;
            m->status = 130;
            m->int_pending = m->trap_int[0] != '\0';
        }
        line_reset(m);
        roc_prompt(m);
        return;
    case 0x04: /* Ctrl-D on empty line leaves the shell like exit does */
        if (m->line_len == 0 && m->read_wait) { /* read gets no line: the end */
            term_puts(&m->t, "\r\n");
            roc_read_answer(m, "", true);
            roc_prompt(m);
            return;
        }
        if (m->line_len == 0 && m->more_on) { /* or ends what wanted more */
            m->more_on = false;
            m->status = 2;
            term_puts(&m->t, "\r\nrocchetto: Syntax error: end of file unexpected\r\n");
            roc_prompt(m);
            return;
        }
        if (m->line_len == 0) {
            if (m->sc.repl) {
                m->sc.repl = false; /* one level up: the REPL, not the shell */
                term_puts(&m->t, "\r\n");
                roc_prompt(m);
                return;
            }
#if ROC_APP_SCREENS
            if (m->indexed && (m->flags & ROC_F_PROMPT) == 0 && roc_layer_spec.home != NULL) {
                term_puts(&m->t, "logout\r\n");
                screen_enter(m, roc_layer_spec.home);
                return;
            }
#endif
            term_puts(&m->t, "logout\r\nNO CARRIER\r\n");
            m->exited = true;
        }
        return;
    case 0x08:
    case 0x7F:
        line_backspace(m);
        return;
    case '\t':
        line_complete(m);
        return;
    case 0x0C: /* Ctrl-L */
        term_puts(&m->t, "\x1b[2J\x1b[H");
        roc_line_repaint(m);
        return;
    case 0x15: /* Ctrl-U */
        line_reset(m);
        line_clear_display(m);
        return;
    default:
        line_insert_rune(m, bytes, blen, cp);
        return;
    }
}

static void handle_bare_esc(roc *m) {
    if (m->mode == ROC_MODE_APP) {
        if (m->req_kind != ROC_REQ_NONE) {
            reader_abort(m);
            return;
        }
        term_app_top(&m->t)->on_key(term_app_ctx(&m->t), FT_KEY_ESC);
        return;
    }
    if (m->mode == ROC_MODE_READ) {
        reader_abort(m);
        return;
    }
    if (m->srch_on) {
        search_end(m, true); /* out of Ctrl-R, on the line found */
        return;
    }
    /* in the shell ESC only clears the line. The way back to the shell is
       'exit', the same word that leaves any shell. */
    line_reset(m);
    line_clear_display(m);
}

/* What the decoder found in the input, to whoever takes it now: a key to
   the app on top or to the line, ESC alone as its own key. Bracketed paste
   (CSI 200~ ... 201~) is only there so an app holds its repaint until the
   paste is over: the text itself arrives as keys either way. */
static void on_input(void *user, keyin_event ev, uint32_t cp) {
    roc *m = user;
    switch (ev) {
    case KEYIN_ESC:
        handle_bare_esc(m);
        return;
    case KEYIN_PASTE_BEGIN:
        m->t.pasting = true;
        return;
    case KEYIN_PASTE_END: {
        m->t.pasting = false;
        const term_app *app = term_app_top(&m->t);
        if (m->mode == ROC_MODE_APP && app != NULL && app->on_resize != NULL) {
            app->on_resize(term_app_ctx(&m->t)); /* the one repaint for the whole paste */
        }
        return;
    }
    case KEYIN_KEY:
        break;
    }
    if (!ft_key_is_code(cp)) {
        handle_rune(m, cp);
        return;
    }
    if (m->mode == ROC_MODE_APP && m->req_kind == ROC_REQ_NONE) {
        term_app_top(&m->t)->on_key(term_app_ctx(&m->t), cp);
        return;
    }
    if (m->mode == ROC_MODE_LINE) {
        if (m->srch_on) {
            search_end(m, true); /* an arrow, Home...: on the line found */
        }
        line_key(m, cp);
    }
}

static void input_byte(roc *m, uint8_t b) {
    keyin_feed(&m->kin, b, on_input, m);
}

void roc_input(roc *m, const uint8_t *data, size_t n) {
    size_t i = 0;
    while (i < n) {
        input_byte(m, data[i]);
        i++;
    }
}

void roc_tick(roc *m, uint32_t ms) {
    m->ticks++;
    m->up_ms += ms;
    keyin_tick(&m->kin, ms, on_input, m);
    if (m->sleep_ms > 0) {
        m->sleep_ms = ms >= m->sleep_ms ? 0 : m->sleep_ms - ms;
    }
    if (m->mode == ROC_MODE_LINE && roc_list_waiting(m)) {
        roc_prompt(m); /* the line goes on; the prompt when it ends */
    }
    const term_app *app = term_app_top(&m->t);
    if (m->mode == ROC_MODE_APP && m->req_kind == ROC_REQ_NONE && app != NULL &&
        app->on_tick != NULL) {
        app->on_tick(term_app_ctx(&m->t), ms);
    }
}

void roc_resize(roc *m, uint16_t cols, uint16_t rows) {
    term_resize(&m->t, cols, rows);
    const term_app *app = term_app_top(&m->t);
    if (m->mode == ROC_MODE_APP && app != NULL && app->on_resize != NULL) {
        app->on_resize(term_app_ctx(&m->t));
    }
}

/* ---- init ---- */

/* The name goes into the prompt and into the prompt's column arithmetic, so
   it has to be printable and short; anything else is "guest". */
static void set_user(roc *m, const char *name) {
    size_t n = name != NULL ? strlen(name) : 0;
    size_t i = 0;
    while (i < n) {
        if ((uint8_t)name[i] < 0x20 || name[i] == ' ' || name[i] == 0x7F) {
            n = 0; /* not a name for a prompt */
            break;
        }
        i++;
    }
    if (n == 0 || n >= ROC_USER_MAX) {
        strcpy(m->user, "guest");
        return;
    }
    memcpy(m->user, name, n + 1);
}

void roc_set_user(roc *m, const char *name) {
    char from[VFS_PATH_MAX];
    char to[VFS_PATH_MAX];
    bool had = roc_home_path(m, "", from, sizeof(from)) && roc_lookup(m, from) != NULL;
    set_user(m, name);
    if (!had || !roc_home_path(m, "", to, sizeof(to)) || strcmp(from, to) == 0) {
        return;
    }
    if (roc_lookup(m, to) != NULL) {
        return; /* a home already there is not overwritten */
    }
    roc_rename_tree(m, from, to, true);
    size_t fl = strlen(from);
    if (strncmp(m->cwd, from, fl) == 0 && (m->cwd[fl] == '\0' || m->cwd[fl] == '/') &&
        strlen(to) + strlen(m->cwd + fl) < sizeof(m->cwd)) {
        char cwd[VFS_PATH_MAX];
        strcpy(cwd, to);
        strcat(cwd, m->cwd + fl);
        strcpy(m->cwd, cwd);
    }
}

void roc_notice(roc *m, const char *text) {
    if (m->mode == ROC_MODE_APP) {
        term_flash(&m->t, text); /* the app on top shows it on its own line */
        return;
    }
    if (m->mode == ROC_MODE_LINE && !m->srch_on) {
        /* as a list of completions is shown: under what was typed, which
           comes back whole below it */
        line_seek(m, m->line_len);
        term_puts(&m->t, "\r\n");
        term_puts(&m->t, text);
        term_puts(&m->t, "\r\n");
        roc_line_repaint(m);
        return;
    }
    term_puts(&m->t, "\r\n"); /* a command has the terminal: a line among its own */
    term_puts(&m->t, text);
    term_puts(&m->t, "\r\n");
}

static void md_sink(void *user, const uint8_t *data, size_t n) {
    roc_body_emit(user, data, n);
}

/* Hugo's ref shortcode names a page by slug; the index knows where that page
   actually lives, so the section never has to be guessed. */
/* cppcheck-suppress constParameterCallback ; md_slug_fn's shape */
static bool md_page(void *user, const uint8_t *slug, size_t n, char *path, size_t cap) {
    const roc *m = user;
    for (size_t i = 0; i < m->fs.nnodes; i++) {
        const vfs_node *nd = &m->fs.nodes[i];
        if (nd->dir) {
            continue;
        }
        const char *base = strrchr(nd->path, '/');
        base = base != NULL ? base + 1 : nd->path;
        size_t bl = strlen(base);
        if (bl < 4 || memcmp(base + bl - 3, ".md", 3) != 0 || bl - 3 != n ||
            memcmp(base, slug, n) != 0) {
            continue;
        }
        size_t pl = strlen(nd->path);
        if (pl + 1 > cap) {
            return false;
        }
        memcpy(path, nd->path, pl + 1);
        return true;
    }
    return false;
}

void roc_scratch_reset(roc *m) {
    m->scratch_used = 0;
}

void *roc_scratch_take(roc *m, size_t n) {
    size_t at = (m->scratch_used + 15U) & ~(size_t)15U;
    if (m->host.scratch == NULL || n > SIZE_MAX - at) {
        return NULL;
    }
    uint8_t *base = m->host.scratch(m->host.ctx, at + n);
    if (base == NULL) {
        return NULL;
    }
    m->scratch_used = at + n;
    return base + at;
}

size_t roc_scratch_mark(const roc *m) {
    return m->scratch_used;
}

void roc_scratch_release(roc *m, size_t mark) {
    if (mark < m->scratch_used) {
        m->scratch_used = mark;
    }
}

void roc_init(roc *m, const roc_host *host, uint16_t cols, uint16_t rows, uint32_t flags) {
    memset(m, 0, sizeof(*m));
    m->host = *host;
    term_init(&m->t, cols, rows);
    m->t.sink = capture_sink;
    m->t.sink_ctx = m;
    m->flags = flags;
    keyin_init(&m->kin);
    vfs_init(&m->fs);
    ufs_init(&m->uf);
    strcpy(m->cwd, "/");
    set_user(m, host->user);
    /* where a command is looked for, by name; exported, as a shell starts */
    char why[64];
    (void)sh_var_assign(&m->vars, "PATH=/bin", why, sizeof(why));
    (void)sh_var_export(&m->vars, "PATH", true, why, sizeof(why));
    /* the site's origin turns site-relative links into absolute ones: a
       terminal hyperlink has no base to resolve against */
    m->md.emit = md_sink;
    m->md.user = m;
    m->md.base = host->site_base != NULL ? host->site_base : "";
    m->md.slug = md_page;
    if ((m->flags & ROC_F_PROMPT) == 0) {
        term_puts(&m->t, "\x1b[1mrocchetto\x1b[0m " ROC_VERSION " \xc2\xb7 ");
        if (host->host_name != NULL && host->host_name[0] != '\0') {
            term_puts(&m->t, host->host_name);
            term_puts(&m->t, " \xc2\xb7 ");
        }
        term_puts(&m->t, "type 'help'\r\n");
    }
    reader_begin(m, ROC_REQ_INDEX, ROC_INDEX_PATH);
}
