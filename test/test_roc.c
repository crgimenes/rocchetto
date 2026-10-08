#include "regex.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/roc.h"

#ifndef ROC_ROOT
#define ROC_ROOT "." /* the repository: its fixtures and commands */
#endif
#include "../src/sh.h"

#if defined(__aarch64__)
#define UNAME_M "arm64"
#else
#define UNAME_M "x86_64"
#endif

static int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* ---- mock host: records the last request; tests answer it explicitly ---- */

typedef struct {
    uint32_t req_id;
    char path[VFS_PATH_MAX];
    int pending;
} mock_host;

static void mock_request(void *ctx, uint32_t req_id, const char *path) {
    mock_host *h = ctx;
    h->req_id = req_id;
    snprintf(h->path, sizeof(h->path), "%s", path);
    h->pending = 1;
}

static char out_buf[TERM_OUT_CAP + 1];

static const char *drain(roc *m) {
    size_t n = term_out_read(&m->t, (uint8_t *)out_buf, TERM_OUT_CAP);
    out_buf[n] = '\0';
    return out_buf;
}

static void send(roc *m, const char *s) {
    roc_input(m, (const uint8_t *)s, strlen(s));
}

/* help opened the pager on its first page (o, what it painted); q closes
   it, back at the prompt. */
static bool help_shown(roc *m, const char *o) {
    bool shown =
        m->t.napps == 1 && strstr(o, "a POSIX shell with its utilities written in Filo") != NULL;
    send(m, "q");
    drain(m);
    return shown && m->t.napps == 0;
}

static const char *canvas_row(const canvas *c, uint16_t row) {
    static char buf[TERM_COLS_MAX * 4 + 1];
    size_t n = 0;
    uint16_t x = 0;
    while (x < c->cols) {
        uint32_t cp = c->cells[row][x].cp;
        if (cp != 0) {
            n += utf8_encode((uint8_t *)buf + n, cp);
        }
        x++;
    }
    buf[n] = '\0';
    return buf;
}

static bool canvas_has(const canvas *c, const char *needle) {
    uint16_t y = 0;
    while (y < c->rows) {
        if (strstr(canvas_row(c, y), needle) != NULL) {
            return true;
        }
        y++;
    }
    return false;
}

static const char *test_index = "/pub/\t0\t2026-08-28 10:00\t\n"
                                "/jnl/\t0\t2026-08-28 10:00\t\n"
                                "/pub/kutta.md\t1234\t2026-08-16 10:30\tKutta no navegador\n"
                                "/pub/caf\xc3\xa9.md\t99\t2026-08-01 09:00\tCaf\xc3\xa9 com UTF-8\n"
                                "/readme.txt\t10\t2020-01-01 00:00\t\n"
                                "/undated.md\t20\t\tNo date at all\n"
                                "bogus line without tabs\n";

static roc M;

/* commands/bin/NAME, the source of a command, as the site serves it. */
static size_t command_source(const char *name, uint8_t *buf, size_t cap) {
    char path[256];
    (void)snprintf(path, sizeof(path), ROC_ROOT "/commands/bin/%s", name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    size_t n = fread(buf, 1, cap, f);
    (void)fclose(f);
    return n;
}

/* A test's own tree as the shell's is built: each bin/NAME.filo compiled
   into the program bin/NAME (the build's mkunits does this), the rest as
   it is. */
static void tree_programs(roc *m, const tree_file *files, size_t n) {
    static tree_file out[256];
    static char paths[256][64];
    static uint8_t units[256][8192];
    CHECK(n <= 256);
    n = n < 256 ? n : 256;
    for (size_t i = 0; i < n; i++) {
        out[i] = files[i];
        size_t pl = strlen(files[i].path);
        if (strncmp(files[i].path, "bin/", 4) == 0 && pl > 8 &&
            strcmp(files[i].path + pl - 4, ".fbb") == 0) {
            /* a program already: named as the command, as the build does */
            (void)snprintf(paths[i], sizeof(paths[i]), "%.*s", (int)(pl - 4), files[i].path);
            out[i].path = paths[i];
            continue;
        }
        if (strncmp(files[i].path, "bin/", 4) != 0 || pl < 10 ||
            strcmp(files[i].path + pl - 5, ".filo") != 0) {
            continue;
        }
        size_t len = 0;
        if (!script_build(m, files[i].data, files[i].len, units[i], sizeof(units[i]), &len)) {
            continue; /* a source that does not compile stays a source: no command */
        }
        (void)snprintf(paths[i], sizeof(paths[i]), "%.*s", (int)(pl - 5), files[i].path);
        out[i] = (tree_file){paths[i], units[i], len};
    }
    tree_set_source(out, n);
}

/* a line run with its stdout and stderr in files, compared (defined below) */
static void util_case(roc *m, const char *line, const char *want, int status,
                      const char *err); /* too large for the stack */

/* Boots to the BBS main menu (the natural landing screen). */
static void *mock_scratch(void *ctx, size_t need) {
    static uint8_t region[64U << 20U];
    (void)ctx;
    if (need > sizeof(region)) {
        return NULL;
    }
    return region;
}

static roc *boot_menu(mock_host *h) {
    memset(h, 0, sizeof(*h));
    roc_host host = {.filo_extend = roc_host_extend,
                     .ctx = h,
                     .host_name = "shell.test",
                     .site_base = "https://shell.test",
                     .request = mock_request,
                     .scratch = mock_scratch};
    roc_init(&M, &host, 80, 24, ROC_F_NO_SPLASH);
    CHECK(h->pending == 1);
    CHECK(strcmp(h->path, ROC_INDEX_PATH) == 0);
    h->pending = 0;
    roc_feed(&M, h->req_id, (const uint8_t *)test_index, strlen(test_index));
    roc_feed_eof(&M, h->req_id);
    drain(&M);
    return &M;
}

/* Boots and drops to the shell, at the root: most tests below were written
   against the site's tree, and the prompt's width matters to the line
   editor ones. The shell itself opens at home (test_shell_starts_at_home). */
static roc *boot(mock_host *h) {
    roc *m = boot_menu(h);
    if (roc_layer_spec.home != NULL) {
        send(m, "s\r"); /* a layer's front screen first: S is the shell */
    }
    drain(m);
    strcpy(m->cwd, "/"); /* quietly: no line in the history, no prompt printed */
    return m;
}

static void test_shell_starts_at_home(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    send(m, "s\r");
    const char *o = drain(m);
    CHECK(strcmp(m->cwd, "/home/guest") == 0);
    CHECK(strstr(o, "\x1b[1;34m~\x1b[0m$ ") != NULL); /* spelled ~ */
    send(m, "pwd\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n/home/guest\r\n") != NULL);
    send(m, "mkdir w\r");
    send(m, "cd w\r");
    o = drain(m);
    CHECK(strstr(o, "\x1b[1;34m~/w\x1b[0m$ ") != NULL);
    send(m, "cd /pub\r");
    o = drain(m);
    CHECK(strstr(o, "\x1b[1;34m/pub\x1b[0m$ ") != NULL);
    send(m, "cd\r"); /* bare cd goes home, as everywhere */
    o = drain(m);
    CHECK(strstr(o, "\x1b[1;34m~\x1b[0m$ ") != NULL && strcmp(m->cwd, "/home/guest") == 0);
}

/* A terminal app's shell (ROC_F_PROMPT): the prompt at once, at home, no
   board; exit ends the session instead of going back to the menu. */
static void test_prompt_flag(void) {
    mock_host h;
    memset(&h, 0, sizeof(h));
    roc_host host = {.filo_extend = roc_host_extend,
                     .ctx = &h,
                     .host_name = "fosforo",
                     .request = mock_request,
                     .scratch = mock_scratch};
    roc_init(&M, &host, 80, 24, ROC_F_PROMPT);
    CHECK(h.pending == 1);
    h.pending = 0;
    roc_feed(&M, h.req_id, (const uint8_t *)test_index, strlen(test_index));
    roc_feed_eof(&M, h.req_id);
    const char *o = drain(&M);
    CHECK(strstr(o, "\x1b[1;34m~\x1b[0m$ ") != NULL);
    CHECK(strcmp(M.cwd, "/home/guest") == 0);
    CHECK(strstr(o, "Your choice") == NULL);
    CHECK(strstr(o, "entries indexed") == NULL && strstr(o, "shell.test") == NULL);
    send(&M, "articles; type articles\r"); /* the shell's own: not an app's */
    send(&M, "ar\t");
    o = drain(&M);
    CHECK(strstr(o, "articles: command not found") != NULL &&
          strstr(o, "articles: not found") != NULL && strstr(o, "articles ") == NULL);
    send(&M, "\x15");
    drain(&M);
    send(&M, "exit\r");
    drain(&M);
    CHECK(roc_exited(&M));
}

static char hosted_cmd[64];
static int hosted_runs;

static bool mock_run(void *ctx, int argc, char *const argv[]) {
    (void)ctx;
    if (argc < 1 || strcmp(argv[0], "ssh") != 0) {
        return false;
    }
    hosted_runs++;
    snprintf(hosted_cmd, sizeof(hosted_cmd), "%s %s", argv[0], argc > 1 ? argv[1] : "");
    return true;
}

static void test_host_runs_commands(void) {
    mock_host h;
    memset(&h, 0, sizeof(h));
    roc_host host = {.filo_extend = roc_host_extend,
                     .ctx = &h,
                     .request = mock_request,
                     .scratch = mock_scratch,
                     .run = mock_run,
                     .commands = "ssh key"};
    roc_init(&M, &host, 80, 24, ROC_F_PROMPT);
    roc_feed(&M, h.req_id, (const uint8_t *)test_index, strlen(test_index));
    roc_feed_eof(&M, h.req_id);
    drain(&M);
    hosted_runs = 0;
    send(&M, "ssh m1; echo after $?\r");
    const char *o = drain(&M);
    CHECK(hosted_runs == 1 && strcmp(hosted_cmd, "ssh m1") == 0);
    CHECK(strstr(o, "\nafter") == NULL); /* the line waits for the host */
    send(&M, "x");
    roc_tick(&M, 50);
    CHECK(strstr(drain(&M), "x") == NULL); /* the keys are the host's */
    roc_run_done(&M, 3);
    roc_tick(&M, 50);
    o = drain(&M);
    CHECK(strstr(o, "after 3\r\n") != NULL && strstr(o, "$ ") != NULL);
    send(&M, "ssh m1 | cat\r");
    CHECK(strstr(drain(&M), "takes the whole terminal") != NULL && hosted_runs == 1);
    send(&M, "x=$(ssh m1); ssh m1 > ~/f; echo st=$?\r");
    o = drain(&M);
    CHECK(strstr(o, "takes the whole terminal") != NULL && strstr(o, "st=126") != NULL &&
          hosted_runs == 1);
    send(&M, "nope\r");
    CHECK(strstr(drain(&M), "not found") != NULL);
    send(&M, "type ssh\r");
    CHECK(strstr(drain(&M), "ssh is a command of the app") != NULL);
    send(&M, "help\r"); /* the pager, and nothing under it: help documents the app's */
    o = drain(&M);
    CHECK(strstr(o, "from the app") == NULL && M.t.napps == 1);
    send(&M, "q");
    drain(&M);
    send(&M, "ss\t");
    CHECK(strstr(drain(&M), "ssh") != NULL);
}

/* ---- utf8 ---- */

static void test_utf8_decode(void) {
    utf8_dec d;
    utf8_dec_init(&d);
    uint32_t cp = 0;
    int rs = 0;
    CHECK(utf8_dec_feed(&d, 0xC3, &cp, &rs) == UTF8_MORE);
    CHECK(utf8_dec_feed(&d, 0xA9, &cp, &rs) == UTF8_RUNE);
    CHECK(cp == 0xE9);

    /* 4-byte emoji */
    utf8_dec_init(&d);
    const uint8_t emoji[] = {0xF0, 0x9F, 0x98, 0x80};
    utf8_result r = UTF8_MORE;
    for (size_t i = 0; i < 4; i++) {
        r = utf8_dec_feed(&d, emoji[i], &cp, &rs);
    }
    CHECK(r == UTF8_RUNE);
    CHECK(cp == 0x1F600);

    /* stray continuation byte */
    utf8_dec_init(&d);
    CHECK(utf8_dec_feed(&d, 0x80, &cp, &rs) == UTF8_ERROR);
    CHECK(rs == 0);

    /* overlong lead is rejected */
    utf8_dec_init(&d);
    CHECK(utf8_dec_feed(&d, 0xC0, &cp, &rs) == UTF8_ERROR);

    /* broken sequence resyncs on the new lead byte */
    utf8_dec_init(&d);
    CHECK(utf8_dec_feed(&d, 0xC3, &cp, &rs) == UTF8_MORE);
    CHECK(utf8_dec_feed(&d, 'a', &cp, &rs) == UTF8_ERROR);
    CHECK(rs == 1);
    CHECK(utf8_dec_feed(&d, 'a', &cp, &rs) == UTF8_RUNE);
    CHECK(cp == 'a');

    /* surrogate half is rejected: ED A0 80 = U+D800 */
    utf8_dec_init(&d);
    utf8_dec_feed(&d, 0xED, &cp, &rs);
    utf8_dec_feed(&d, 0xA0, &cp, &rs);
    CHECK(utf8_dec_feed(&d, 0x80, &cp, &rs) == UTF8_ERROR);
}

static void test_utf8_width(void) {
    CHECK(utf8_width('a') == 1);
    CHECK(utf8_width(0xE9) == 1);    /* é */
    CHECK(utf8_width(0x65E5) == 2);  /* 日 */
    CHECK(utf8_width(0x1F600) == 2); /* 😀 */
    CHECK(utf8_width(0x0301) == 0);  /* combining acute */
    CHECK(utf8_width(0x200B) == 0);  /* zero width space */
    CHECK(utf8_swidth("S\xc3\xa3o") == 3);
    CHECK(utf8_swidth("\xe6\x97\xa5\xe6\x9c\xac") == 4); /* 日本 */
    CHECK(utf8_last_rune_len((const uint8_t *)"a\xc3\xa9", 3) == 2);
    CHECK(utf8_last_rune_len((const uint8_t *)"a", 1) == 1);
    CHECK(utf8_last_rune_len(NULL, 0) == 0);
}

/* ---- vfs ---- */

static void test_vfs_resolve(void) {
    char dst[VFS_PATH_MAX];
    CHECK(vfs_resolve("/pub", "..", dst, sizeof(dst)) && strcmp(dst, "/") == 0);
    CHECK(vfs_resolve("/", "../..", dst, sizeof(dst)) && strcmp(dst, "/") == 0);
    CHECK(vfs_resolve("/pub", "kutta.md", dst, sizeof(dst)) && strcmp(dst, "/pub/kutta.md") == 0);
    CHECK(vfs_resolve("/pub", "/jnl", dst, sizeof(dst)) && strcmp(dst, "/jnl") == 0);
    CHECK(vfs_resolve("/pub", ".", dst, sizeof(dst)) && strcmp(dst, "/pub") == 0);
    CHECK(vfs_resolve("/a", "b//c/./../d", dst, sizeof(dst)) && strcmp(dst, "/a/b/d") == 0);
    /* overflow is refused, not truncated */
    char big[VFS_PATH_MAX * 2];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    CHECK(!vfs_resolve("/", big, dst, sizeof(dst)));
}

static void test_vfs_parse(void) {
    static vfs v;
    vfs_init(&v);
    CHECK(vfs_append(&v, (const uint8_t *)test_index, strlen(test_index)));
    CHECK(vfs_parse(&v) == 6); /* bogus line skipped */
    const vfs_node *n = vfs_lookup(&v, "/pub");
    CHECK(n != NULL && n->dir);
    n = vfs_lookup(&v, "/pub/kutta.md");
    CHECK(n != NULL && !n->dir && n->size == 1234);
    CHECK(strcmp(n->title, "Kutta no navegador") == 0);
    CHECK(vfs_lookup(&v, "/nope") == NULL);
    CHECK(vfs_is_child(vfs_lookup(&v, "/pub/kutta.md"), "/pub"));
    CHECK(!vfs_is_child(vfs_lookup(&v, "/pub/kutta.md"), "/"));
    CHECK(vfs_is_child(vfs_lookup(&v, "/pub"), "/"));
}

/* ---- shell integration ---- */

static void test_boot_and_pwd(void) {
    mock_host h;
    roc *m = boot(&h);
    send(m, "pwd\r");
    const char *o = drain(m);
    CHECK(strstr(o, "/\r\n") != NULL);
    CHECK(strstr(o, "guest@shell.test") != NULL);
}

static void test_cd_ls(void) {
    mock_host h;
    roc *m = boot(&h);
    send(m, "cd pub\rpwd\r");
    const char *o = drain(m);
    CHECK(strstr(o, "/pub\r\n") != NULL);
    send(m, "ls\r");
    o = drain(m);
    CHECK(strstr(o, "kutta.md") != NULL);
    CHECK(strstr(o, "caf\xc3\xa9.md") != NULL);
    send(m, "cd nope\r");
    o = drain(m);
    CHECK(strstr(o, "No such file or directory") != NULL);
    send(m, "cd ..\rpwd\r");
    o = drain(m);
    CHECK(strstr(o, "/\r\n") != NULL);
}

static void test_cat_strips_front_matter(void) {
    mock_host h;
    roc *m = boot(&h);
    send(m, "cat /pub/kutta.md\r");
    drain(m);
    CHECK(h.pending == 1);
    CHECK(strcmp(h.path, "/pub/kutta.md") == 0);
    h.pending = 0;
    const char *body = "+++\ntitle = \"secret\"\n+++\n\nhello\nworld";
    roc_feed(m, h.req_id, (const uint8_t *)body, strlen(body));
    roc_feed_eof(m, h.req_id);
    const char *o = drain(m);
    CHECK(strstr(o, "hello") != NULL); /* styling now sits between the words */
    CHECK(strstr(o, "world") != NULL);
    CHECK(strstr(o, "secret") == NULL);
    CHECK(strstr(o, "$ ") != NULL); /* prompt is back */
}

static void test_cat_plain_body_and_bom(void) {
    mock_host h;
    roc *m = boot(&h);
    send(m, "cat /readme.txt\r");
    drain(m);
    h.pending = 0;
    const uint8_t body[] = {0xEF, 0xBB, 0xBF, 'h', 'i', '\n', 'x'};
    roc_feed(m, h.req_id, body, sizeof(body));
    roc_feed_eof(m, h.req_id);
    const char *o = drain(m);
    CHECK(strstr(o, "hi\r\nx\r\n") != NULL);
    CHECK(strchr(o, (char)0xEF) == NULL);
}

static void test_ctrl_c_aborts_cat(void) {
    mock_host h;
    roc *m = boot(&h);
    send(m, "cat /pub/kutta.md\r");
    drain(m);
    h.pending = 0;
    uint32_t old_id = h.req_id;
    send(m, "\x03"); /* Ctrl-C while reading */
    const char *o = drain(m);
    CHECK(strstr(o, "^C") != NULL);
    /* stale feed is ignored */
    roc_feed(m, old_id, (const uint8_t *)"late", 4);
    o = drain(m);
    CHECK(strstr(o, "late") == NULL);
    send(m, "pwd\r");
    o = drain(m);
    CHECK(strstr(o, "/\r\n") != NULL);
}

static void test_line_editing_multibyte(void) {
    mock_host h;
    roc *m = boot(&h);
    send(m, "caf\xc3\xa9");
    const char *o = drain(m);
    CHECK(strstr(o, "caf\xc3\xa9") != NULL); /* echo */
    send(m, "\x7f");                         /* backspace removes the whole é */
    o = drain(m);
    CHECK(strstr(o, "\b \b") != NULL);
    send(m, "\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: caf:") != NULL); /* é fully removed from the line */

    /* wide rune erases two columns */
    send(m, "\xe6\x97\xa5"); /* 日 */
    drain(m);
    send(m, "\x7f");
    o = drain(m);
    CHECK(strstr(o, "\b \b\b \b") != NULL);
}

static void test_esc_and_arrows(void) {
    mock_host h;
    roc *m = boot(&h);
    /* arrow keys are swallowed */
    send(m, "a\x1b[Ab\r");
    const char *o = drain(m);
    CHECK(strstr(o, "rocchetto: ab:") != NULL);
    /* bare ESC (via tick timeout) clears the line */
    send(m, "junk");
    send(m, "\x1b");
    roc_tick(m, ROC_ESC_TIMEOUT_MS);
    drain(m);
    send(m, "pwd\r");
    o = drain(m);
    CHECK(strstr(o, "junk") == NULL || strstr(o, "rocchetto: junk") == NULL);
    CHECK(strstr(o, "/\r\n") != NULL);
}

/* The prompt is a real line editor: the cursor moves with the arrows and
   Home/End, text goes in and out at the cursor, and nothing needs the whole
   line to be at the end. The wire is checked where the arithmetic matters —
   rune widths, and the wrap of a line longer than the row. */
static void test_line_editing(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);

    /* insert in the middle: "hlp", Left, Left, "e" -> help */
    send(m, "hlp\x1b[D\x1b[D"
            "e\r");
    const char *o = drain(m);
    CHECK(help_shown(m, o));

    /* backspace in the middle: "hellp", Left, Backspace -> help */
    send(m, "hellp\x1b[D\x7f\r");
    o = drain(m);
    CHECK(help_shown(m, o));

    /* Delete at the cursor: "hxelp", Home, Right, Delete -> help */
    send(m, "hxelp\x1b[H\x1b[C\x1b[3~\r");
    o = drain(m);
    CHECK(help_shown(m, o));

    /* Ctrl-A and Ctrl-E are Home and End under other names */
    send(m, "elp\x01h\x05\r");
    o = drain(m);
    CHECK(help_shown(m, o));

    /* Left steps back by the width of the rune: 1 column for é, 2 for 日 */
    send(m, "a\xc3\xa9\xe6\x97\xa5");
    drain(m);
    send(m, "\x1b[D");
    o = drain(m);
    CHECK(strstr(o, "\r\x1b[22C") != NULL); /* prompt 20 + a + é */
    send(m, "\x1b[D");
    o = drain(m);
    CHECK(strstr(o, "\r\x1b[21C") != NULL);
    send(m, "\x15");
    drain(m);

    /* a line longer than the row wraps: Home climbs to the prompt row, End
       comes back down to the column the line ends on */
    char many[71];
    memset(many, 'x', 70);
    many[70] = '\0';
    send(m, many); /* 20 + 70 = 90 columns on an 80-column row */
    drain(m);
    send(m, "\x1b[H");
    o = drain(m);
    CHECK(strstr(o, "\x1b[1A\r\x1b[20C") != NULL);
    send(m, "\x1b[F");
    o = drain(m);
    CHECK(strstr(o, "\x1b[1B\r\x1b[10C") != NULL);
    send(m, "\x15");
    drain(m);
}

/* Tab at the end of a word completes it: the first word from the command
   table, any other from the directory it names. One match is finished off,
   several share their common start, and when that adds nothing the choice is
   listed. Tab anywhere else does nothing. */
/* ls as POSIX's: columns down for the terminal, one a line for a |, the
   long row saying only what holds; the site's view is articles. */
static void test_ls_posix(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "cd ~; mkdir d; for f in one two three four five; do echo $f > $f; done\r");
    drain(m);
    send(m, "ls\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\nd      five   four   one    three  two\r\n") != NULL);
    send(m, "export COLUMNS=14; ls; ls -x; unset COLUMNS\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nd      one\r\nfive   three\r\nfour   two\r\n") != NULL &&
          strstr(o, "\r\nd      five\r\nfour   one\r\nthree  two\r\n") != NULL);
    send(m, "ls | head -n 2\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nd\r\nfive\r\n") != NULL);
    send(m, "ls -l one d\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n-rw-r--r-- 1 guest  guest  4 ") != NULL &&
          strstr(o, " one\r\n\r\nd:\r\ntotal 0\r\n") != NULL);
    send(m, "ls -lgo /bin/ls; ls -F /bin/ls /pub; ls -ldF /pub\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n-r-xr-xr-x 1 ") != NULL && strstr(o, "/bin/ls*\r\n") != NULL &&
          strstr(o, "tree") == NULL && strstr(o, "\r\ndr-xr-xr-x 1 site  site  ") != NULL);
    send(m, "ls -ls one; ls -lh /pub/kutta.md; ls -S1r ~ | head -n 1\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n1 -rw-r--r-- ") != NULL && strstr(o, " 1.3K ") != NULL &&
          strstr(o, "\r\nd\r\n") != NULL);
    send(m, "ls -1 d nowhere; echo st=$?\r");
    o = drain(m);
    CHECK(strstr(o, "ls: nowhere: No such file or directory\r\nd:\r\nst=1") != NULL);
}

static void test_tab_completion(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);

    /* a unique command gets its space and runs ("he" is head or help now) */
    send(m, "hel\t\r");
    const char *o = drain(m);
    CHECK(help_shown(m, o));

    /* several commands start with c: nothing to add, so they are listed */
    send(m, "c\t");
    o = drain(m);
    if (strstr(
            o,
            "cd  cat  corewar  chgrp  chmod  chown  cksum  clear  cmp  comm  cp  crontab  cut") ==
        NULL) {
        fprintf(stderr, "c<TAB>: %s\n", o);
    }
    CHECK(strstr(
              o,
              "cd  cat  corewar  chgrp  chmod  chown  cksum  clear  cmp  comm  cp  crontab  cut") !=
          NULL);
    send(m, "\x15");
    drain(m);

    /* the second word is a path: a directory completes with its slash and
       no space, so the next Tab keeps walking into it */
    send(m, "ls pu\t");
    o = drain(m);
    CHECK(strstr(o, "b/") != NULL);
    CHECK(m->line_len == 7 && memcmp(m->line, "ls pub/", 7) == 0);
    send(m, "\r");
    o = drain(m);
    CHECK(strstr(o, "kutta") != NULL);

    /* a file completes with a space; the command then asks the host for it */
    h.pending = 0;
    send(m, "cat pub/kutta.\t\r");
    CHECK(h.pending == 1);
    CHECK(strcmp(h.path, "/pub/kutta.md") == 0);
    roc_feed_fail(m, h.req_id);
    drain(m);

    /* not at the end of a word: Tab is inert */
    send(m, "help\x1b[H\t");
    CHECK(m->line_len == 4);
    send(m, "\x1b[F\r");
    o = drain(m);
    CHECK(help_shown(m, o));

    /* no match: nothing happens, the line runs as typed */
    send(m, "zzz\t\r");
    o = drain(m);
    CHECK(strstr(o, "command not found") != NULL);
}

static bool line_is(const roc *m, const char *want) {
    size_t n = strlen(want);
    return m->line_len == n && memcmp(m->line, want, n) == 0;
}

/* Up and Down walk the session's history the zsh way: only lines that begin
   with what is left of the cursor, and the cursor stays where it was. Down
   past the newest match brings the typed line back. Editing ends the walk. */
static void test_history(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "pwd\r");
    drain(m);
    send(m, "help\r");
    drain(m);
    send(m, "q");
    drain(m);
    send(m, "banner\r");
    drain(m);
    send(m, "banner\r"); /* twice in a row is one memory, not two */
    drain(m);

    /* empty line: Up walks back to the oldest and stops there */
    send(m, "\x1b[A");
    drain(m);
    CHECK(line_is(m, "banner"));
    send(m, "\x1b[A");
    drain(m);
    CHECK(line_is(m, "help"));
    send(m, "\x1b[A");
    drain(m);
    CHECK(line_is(m, "pwd"));
    send(m, "\x1b[A");
    drain(m);
    CHECK(line_is(m, "pwd"));
    /* Down comes forward and then returns the line as it was typed */
    send(m, "\x1b[B");
    drain(m);
    CHECK(line_is(m, "help"));
    send(m, "\x1b[B");
    drain(m);
    CHECK(line_is(m, "banner"));
    send(m, "\x1b[B");
    drain(m);
    CHECK(m->line_len == 0);
    send(m, "\x1b[B");
    drain(m);
    CHECK(m->line_len == 0);

    /* a prefix narrows the walk, and the cursor stays after it */
    send(m, "h\x1b[A");
    const char *o = drain(m);
    CHECK(line_is(m, "help"));
    CHECK(m->line_cur == 1);
    CHECK(strstr(o, "\r\x1b[21C") != NULL); /* prompt 20 + "h" */
    send(m, "\x1b[A");                      /* no older line starts with h */
    drain(m);
    CHECK(line_is(m, "help"));
    send(m, "\x1b[B");
    drain(m);
    CHECK(line_is(m, "h"));
    send(m, "\x15");
    drain(m);

    /* Enter on a recalled line runs it and makes it the newest */
    send(m, "p\x1b[A\r");
    o = drain(m);
    CHECK(strstr(o, "/\r\n") != NULL);
    send(m, "\x1b[A");
    drain(m);
    CHECK(line_is(m, "pwd"));
    send(m, "\x15");
    drain(m);

    /* an edit ends the walk: the next Up searches with the edited prefix */
    send(m, "\x1b[A");
    drain(m);
    CHECK(line_is(m, "pwd"));
    send(m, "\x7f\x7f");
    drain(m);
    CHECK(line_is(m, "p"));
    send(m, "\x1b[A");
    drain(m);
    CHECK(line_is(m, "pwd"));
    send(m, "\x15");
    drain(m);

    /* the ring keeps the last ROC_HIST_MAX lines */
    size_t i = 0;
    while (i < ROC_HIST_MAX + 5) {
        send(m, (i % 2) == 0 ? "pwd\r" : "help\r");
        drain(m);
        if (m->t.napps > 0) {
            send(m, "q");
            drain(m);
        }
        i++;
    }
    CHECK(m->hist_n == ROC_HIST_MAX);
}

static void test_ufs(void) {
    static ufs u;
    ufs_init(&u);
    CHECK(ufs_begin(&u, "/home/a/x", 3) == UFS_OK);
    CHECK(ufs_data(&u, (const uint8_t *)"abc", 3));
    CHECK(!ufs_data(&u, (const uint8_t *)"d", 1)); /* past the size announced */
    ufs_end(&u);
    CHECK(ufs_begin(&u, "/home/a/y", 2) == UFS_OK);
    CHECK(ufs_data(&u, (const uint8_t *)"de", 2));
    ufs_end(&u);
    CHECK(ufs_begin(&u, "/home/a/z", 1) == UFS_OK);
    CHECK(ufs_data(&u, (const uint8_t *)"f", 1));
    ufs_end(&u);
    CHECK(u.nfiles == 3 && u.used == 6);
    /* removing the middle one slides the last down */
    CHECK(ufs_remove(&u, "/home/a/y"));
    const uint8_t *d = NULL;
    size_t n = 0;
    CHECK(ufs_find(&u, "/home/a/z", &d, &n) && n == 1 && d[0] == 'f');
    CHECK(ufs_find(&u, "/home/a/x", &d, &n) && n == 3 && memcmp(d, "abc", 3) == 0);
    CHECK(!ufs_find(&u, "/home/a/y", &d, &n));
    CHECK(u.nfiles == 2 && u.used == 4);
    /* removing the first of three of different sizes moves both others */
    CHECK(ufs_begin(&u, "/home/a/y", 5) == UFS_OK);
    CHECK(ufs_data(&u, (const uint8_t *)"vwxyz", 5));
    ufs_end(&u);
    CHECK(ufs_remove(&u, "/home/a/x"));
    CHECK(ufs_find(&u, "/home/a/z", &d, &n) && n == 1 && d[0] == 'f');
    CHECK(ufs_find(&u, "/home/a/y", &d, &n) && n == 5 && memcmp(d, "vwxyz", 5) == 0);
    CHECK(u.used == 6);
    CHECK(ufs_begin(&u, "/home/a/x", 3) == UFS_OK);
    CHECK(ufs_data(&u, (const uint8_t *)"abc", 3));
    ufs_end(&u);
    CHECK(ufs_remove(&u, "/home/a/y"));
    CHECK(u.nfiles == 2 && u.used == 4);
    /* a transfer replaces its namesake; an aborted one leaves nothing */
    CHECK(ufs_begin(&u, "/home/a/x", 2) == UFS_OK);
    CHECK(!ufs_find(&u, "/home/a/x", &d, &n));
    ufs_abort(&u);
    CHECK(u.nfiles == 1 && u.used == 1);
    CHECK(ufs_begin(&u, "/home/a/big", UFS_FILE_MAX + 1) == UFS_TOO_LARGE);
    CHECK(ufs_begin(&u, "/home/a/x", 1) == UFS_OK);
    CHECK(ufs_begin(&u, "/home/a/w", 1) == UFS_BUSY);
    ufs_end(&u);
}

static void drop(roc *m, const char *name, const char *dest, const char *body) {
    if (!roc_upload_begin(m, name, dest, strlen(body))) {
        return;
    }
    roc_upload_data(m, (const uint8_t *)body, strlen(body));
    roc_upload_end(m);
}

/* Shift and Ctrl on the arrows reach a screen as KEY plus a modifier. */
static void test_key_modifiers(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    static const char init[] = "#t"; /* the hooks are the files named for them */
    static const char draw[] = "(print-at 0 0 (str-concat \"got:\" NOTE))";
    static const char key[] = "(set NOTE (cond ((= KEY (+ KEY_UP KEY_SHIFT)) \"shift-up\")"
                              " ((= KEY (+ KEY_DEL KEY_CTRL)) \"ctrl-del\")"
                              " ((= KEY KEY_HOME) \"home\") (else \"other\")))";
    static const tree_file tree[] = {
        {"t/init.filo", (const uint8_t *)init, sizeof(init) - 1},
        {"t/draw.filo", (const uint8_t *)draw, sizeof(draw) - 1},
        {"t/key.filo", (const uint8_t *)key, sizeof(key) - 1},
    };
    tree_programs(m, tree, 3);
    screen_enter(m, "t");
    drain(m);
    send(m, "\x1b[1;2A");
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "got:shift-up"));
    send(m, "\x1b[3;5~");
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "got:ctrl-del"));
    send(m, "\x1b[1;5H"); /* Ctrl+Home is not plain Home */
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "got:other"));
    send(m, "\x1b[H");
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "got:home"));
    tree_set_source(NULL, 0);
    /* the shell's line ignores what it does not know */
    m = boot(&h);
    drain(m);
    send(m, "echo hi");
    send(m, "\x1b[1;2D");
    send(m, "\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\nhi\r\n") != NULL);
}

/* ---- the kept home ---- */

static uint8_t kept[HOME_BLOB_CAP];
static size_t kept_len;
static int kept_fail;

static uint32_t mock_store_put(void *ctx, const uint8_t *data, size_t len) {
    (void)ctx;
    if (kept_fail) {
        return (uint32_t)kept_fail; /* 1 refused, ROC_STORE_OTHER another tab */
    }
    memcpy(kept, data, len);
    memset(kept + len, 0, sizeof(kept) - len); /* strstr below reads a string */
    kept_len = len;
    return 0;
}

/* Boots a host that keeps the home; the /.home request is answered with
   what was kept, or fails when nothing was. Lands in the shell. */
static roc *boot_kept(mock_host *h) {
    memset(h, 0, sizeof(*h));
    roc_host host = {.filo_extend = roc_host_extend,
                     .ctx = h,
                     .host_name = "shell.test",
                     .site_base = "https://shell.test",
                     .request = mock_request,
                     .store_put = mock_store_put};
    roc_init(&M, &host, 80, 24, ROC_F_NO_SPLASH);
    h->pending = 0;
    roc_feed(&M, h->req_id, (const uint8_t *)test_index, strlen(test_index));
    roc_feed_eof(&M, h->req_id);
    CHECK(h->pending == 1);
    CHECK(strcmp(h->path, ROC_HOME_PATH) == 0);
    h->pending = 0;
    if (kept_len == 0) {
        roc_feed_fail(&M, h->req_id);
    } else {
        roc_feed(&M, h->req_id, kept, kept_len);
        roc_feed_eof(&M, h->req_id);
    }
    return &M;
}

/* ~/.profile runs when the session opens, in the shell itself: what it
   sets is there at the prompt. A broken one says why and the session
   goes on. */
static void test_profile(void) {
    mock_host h;
    kept_len = 0;
    roc *m = boot_kept(&h);
    drain(m);
    send(m, "s\r");
    drain(m);
    const char *p = "greet=hello\nalias hi='echo $greet'\nPATH=~/bin:$PATH\necho profile ran\n";
    CHECK(roc_write_file(m, "test", "/home/guest/.profile", (const uint8_t *)p, strlen(p)));
    CHECK(kept_len > 0);
    m = boot_kept(&h);
    const char *o = drain(m);
    CHECK(strstr(o, "profile ran") != NULL);
    send(m, "s\r");
    drain(m);
    send(m, "hi; echo $PATH\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nhello\r\n/home/guest/bin:/bin\r\n") != NULL);
    CHECK(roc_write_file(m, "test", "/home/guest/.profile", (const uint8_t *)"if true\n", 8));
    m = boot_kept(&h);
    o = drain(m);
    CHECK(strstr(o, "Syntax error") != NULL);
    send(m, "s\r");
    drain(m);
    send(m, "echo still\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nstill\r\n") != NULL);
    kept_len = 0;
}

static void test_home_unpack(void) {
    static ufs u;
    static vfs v;
    const uint8_t *d = NULL;
    size_t n = 0;
    ufs_init(&u);
    vfs_init(&v);
    CHECK(vfs_add_path(&v, "/home/guest", 0, true));
    /* a good blob, then every way a record can be wrong: each is dropped
       without taking the good ones with it */
    static const char good[] = "msh-home 1\nw/imp.red\t9\nMOV 0, 1\n\nnote\t0\n\n";
    CHECK(home_unpack(&u, &v, "/home/guest", (const uint8_t *)good, sizeof(good) - 1, NULL) == 2);
    CHECK(ufs_find(&u, "/home/guest/w/imp.red", &d, &n) && n == 9);
    CHECK(vfs_lookup(&v, "/home/guest/w") != NULL && vfs_lookup(&v, "/home/guest/w")->dir);
    CHECK(ufs_find(&u, "/home/guest/note", &d, &n) && n == 0);
    ufs_init(&u);
    static const char bad[] = "msh-home 1\n../x\t1\na\n/abs\t1\nb\nok\t1\nc\nlie\t99\nd\n";
    CHECK(home_unpack(&u, &v, "/home/guest", (const uint8_t *)bad, sizeof(bad) - 1, NULL) == 1);
    CHECK(ufs_find(&u, "/home/guest/ok", &d, &n) && d[0] == 'c');
    ufs_init(&u);
    CHECK(home_unpack(&u, &v, "/home/guest", (const uint8_t *)"msh-home 2\nx\t1\na\n", 18, NULL) ==
          0);
    CHECK(home_unpack(&u, &v, "/home/guest", (const uint8_t *)"msh-home 1\nx\t1\n", 15, NULL) == 0);
    CHECK(home_unpack(&u, &v, "/home/guest", (const uint8_t *)"", 0, NULL) == 0);
    /* a file where the home has a directory is refused */
    static const char clash[] = "msh-home 1\nw\t1\na\n";
    CHECK(home_unpack(&u, &v, "/home/guest", (const uint8_t *)clash, sizeof(clash) - 1, NULL) == 0);

    /* version 2: directories, empty ones too, and the times of files */
    ufs_init(&u);
    vfs_init(&v);
    CHECK(vfs_add_path(&v, "/home/guest", 0, true));
    static const char v2[] = "msh-home 2\nd\tempty\nd\tw\nf\tw/imp.red\t9\t1759579200\nMOV 0, "
                             "1\n\nf\tnote\t0\t-\n\n";
    size_t dirs = 0;
    CHECK(home_unpack(&u, &v, "/home/guest", (const uint8_t *)v2, sizeof(v2) - 1, &dirs) == 2);
    CHECK(dirs == 2);
    CHECK(vfs_lookup(&v, "/home/guest/empty") != NULL && vfs_lookup(&v, "/home/guest/empty")->dir);
    const ufs_file *f = ufs_entry(&u, "/home/guest/w/imp.red");
    CHECK(f != NULL && f->len == 9 && f->has_mtime && f->mtime == 1759579200);
    f = ufs_entry(&u, "/home/guest/note");
    CHECK(f != NULL && f->len == 0 && !f->has_mtime);
    /* round trip: what was read packs back to the same bytes */
    static uint8_t again[HOME_BLOB_CAP];
    home_report rep;
    size_t alen = home_pack(&u, &v, "/home/guest", again, sizeof(again), &rep);
    CHECK(rep.files == 2 && rep.dirs == 2 && rep.skipped == 0);
    CHECK(alen == sizeof(v2) - 1 && memcmp(again, v2, alen) == 0);
    /* wrong records: a bad time, an unknown kind, a path out of the home,
       a file over a directory; what came before stays */
    ufs_init(&u);
    static const char v2bad[] = "msh-home 2\nd\t../up\nf\tok\t1\t5\nc\nf\tempty\t1\t-\nx\n"
                                "f\tt\t1\t12a\nz\nf\tafter\t1\t-\ny\n";
    CHECK(home_unpack(&u, &v, "/home/guest", (const uint8_t *)v2bad, sizeof(v2bad) - 1, &dirs) ==
          1);
    CHECK(dirs == 0 && vfs_lookup(&v, "/home/up") == NULL);
    CHECK(ufs_entry(&u, "/home/guest/ok") != NULL && ufs_entry(&u, "/home/guest/ok")->mtime == 5);
    CHECK(ufs_entry(&u, "/home/guest/after") == NULL); /* nothing after a bad record */
    static const char v2kind[] = "msh-home 2\nq\tx\nd\tlate\n";
    CHECK(home_unpack(&u, &v, "/home/guest", (const uint8_t *)v2kind, sizeof(v2kind) - 1, &dirs) ==
          0);
    CHECK(dirs == 0 && vfs_lookup(&v, "/home/guest/late") == NULL);
    static const char v2cut[] = "msh-home 2\nf\tcut\t5\t-\nab";
    CHECK(home_unpack(&u, &v, "/home/guest", (const uint8_t *)v2cut, sizeof(v2cut) - 1, NULL) == 0);
}

static int claims;
static void mock_store_claim(void *ctx) {
    (void)ctx;
    claims++;
    kept_fail = 0; /* this tab's save goes over the other's from now */
}

/* Another tab kept its home: said once, nothing more saved over it, home
   keep claims and saves. */
static void test_home_other_tab(void) {
    mock_host h;
    kept_len = 0;
    kept_fail = 0;
    roc *m = boot_kept(&h);
    m->host.store_claim = mock_store_claim;
    send(m, "s\r");
    drain(m);
    kept_fail = ROC_STORE_OTHER;
    send(m, "echo x > ~/x.txt\r");
    const char *o = drain(m);
    CHECK(strstr(o, "another tab kept its home since this one read it") != NULL);
    CHECK(roc_home_conflict(m));
    send(m, "echo y > ~/y.txt\r");
    o = drain(m);
    CHECK(strstr(o, "another tab") == NULL); /* once */
    send(m, "home\r");
    o = drain(m);
    CHECK(strstr(o, "home keep (this tab's wins)") != NULL);
    send(m, "home keep\r");
    o = drain(m);
    CHECK(claims == 1 && !roc_home_conflict(m) &&
          strstr(o, "This tab's home is the kept one now.") != NULL);
    CHECK(strstr((const char *)kept, "y.txt") != NULL &&
          strstr((const char *)kept, "x.txt") != NULL);
}

static char dl_name[64];
static uint8_t dl_data[HOME_BLOB_CAP];
static size_t dl_len;

static void mock_download(void *ctx, const char *name, const uint8_t *data, size_t len) {
    (void)ctx;
    snprintf(dl_name, sizeof(dl_name), "%s", name);
    memcpy(dl_data, data, len);
    dl_len = len;
}

static void test_home_carry(void) {
    mock_host h;
    kept_len = 0;
    kept_fail = 0;
    roc *m = boot_kept(&h);
    send(m, "s\r");
    drain(m);

    /* no way out on this host: said as a missing device */
    send(m, "download /bin/ver\r");
    const char *o = drain(m);
    CHECK(strstr(o, "rocchetto: download: No such device") != NULL);
    m->host.download = mock_download;
    dl_name[0] = '\0';
    send(m, "download /bin/ver\r"); /* a program of the shell's, whole */
    drain(m);
    CHECK(strcmp(dl_name, "ver") == 0);
    CHECK(dl_len > 16 && memcmp(dl_data,
                                "\x7f"
                                "FBC\x02",
                                5) == 0);
    send(m, "download /pub/kutta.md\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: download: /pub/kutta.md: Operation not supported") != NULL);
    send(m, "download /pub\r");
    o = drain(m);
    CHECK(strstr(o, "Is a directory") != NULL);
    send(m, "download\r");
    o = drain(m);
    CHECK(strstr(o, "usage: download <file>") != NULL);

    /* home: status, export as the blob, import from a file of the user's */
    send(m, "home\r");
    o = drain(m);
    CHECK(strstr(o, "Kept in this browser: 11 of 65536 bytes (16K per file at most), "
                    "directories too.") != NULL);
    CHECK(strstr(o, "In memory now: 0 of 2097152 bytes, 0 of 64 files (512K each at most).") !=
          NULL);
    send(m, "mkdir ~/w\r");
    drain(m);
    drop(m, "imp.red", "~/w", "MOV 0, 1\n");
    drain(m);
    send(m, "home export\r");
    drain(m);
    CHECK(strcmp(dl_name, "rocchetto-home.txt") == 0);
    CHECK(dl_len == kept_len && memcmp(dl_data, kept, dl_len) == 0);
    CHECK(memcmp(dl_data, "msh-home 2\n", 11) == 0);
    CHECK(memmem(dl_data, dl_len, "f\tw/imp.red\t9\t", 14) != NULL);
    /* a blob dropped on a fresh visit brings the files back */
    static uint8_t blob[HOME_BLOB_CAP];
    size_t blen = dl_len;
    memcpy(blob, dl_data, blen);
    kept_len = 0;
    m = boot_kept(&h);
    send(m, "s\r");
    drain(m);
    CHECK(vfs_lookup(&m->fs, "/home/guest/w") == NULL);
    CHECK(roc_upload_begin(m, "rocchetto-home.txt", "", blen));
    roc_upload_data(m, blob, blen);
    roc_upload_end(m);
    drain(m);
    send(m, "home import ~/rocchetto-home.txt\r"); /* dropped at home, where the shell opened */
    o = drain(m);
    CHECK(strstr(o, "1 file back in /home/guest.") != NULL);
    send(m, "cat ~/w/imp.red\r");
    o = drain(m);
    CHECK(strstr(o, "MOV 0, 1") != NULL);
    CHECK(strstr((const char *)kept, "w/imp.red\t9\t") != NULL); /* and it is kept again */
    send(m, "home import ~/w/imp.red\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: home: /home/guest/w/imp.red: Invalid argument") != NULL);
    send(m, "home import nothere\r");
    o = drain(m);
    CHECK(strstr(o, "No such file or directory") != NULL);
    send(m, "home what\r");
    o = drain(m);
    CHECK(strstr(o, "usage: home") != NULL);

    /* a host that keeps nothing says so instead of pretending */
    m = boot(&h);
    drain(m);
    send(m, "home\r");
    o = drain(m);
    CHECK(strstr(o, "keeps nothing between visits") != NULL);
}

static void test_home_kept(void) {
    mock_host h;
    kept_len = 0;
    kept_fail = 0;
    roc *m = boot_kept(&h);
    send(m, "s\r");
    drain(m);

    /* every change saves the whole home: files under it, relative paths */
    send(m, "mkdir ~/w\r");
    drain(m);
    static const char w[] = "(write-file \"~/w/imp.red\" \"MOV 0, 1\\n\")";
    static const tree_file tree[] = {{"bin/w.filo", (const uint8_t *)w, sizeof(w) - 1}};
    tree_programs(m, tree, 1);
    send(m, "w\r");
    drain(m);
    tree_set_source(NULL, 0);
    CHECK(kept_len > 0);
    CHECK(memcmp(kept, "msh-home 2\nd\tw\nf\tw/imp.red\t9\t-\nMOV 0, 1\n\n", 39) == 0);
    send(m, "cd ~\r");
    drain(m);
    drop(m, "hi.filo", "", "(echo \"hi\")");
    drain(m);
    CHECK(strstr((const char *)kept, "f\thi.filo\t11\t-\n") != NULL);

    /* too big for the blob: kept in the session, said once, not in the blob */
    static uint8_t big[HOME_FILE_MAX + 1];
    memset(big, 'b', sizeof(big));
    CHECK(roc_upload_begin(m, "big.txt", "", sizeof(big)));
    roc_upload_data(m, big, sizeof(big));
    roc_upload_end(m);
    const char *o = drain(m);
    CHECK(strstr(o, "uploaded /home/guest/big.txt") != NULL);
    CHECK(strstr(o, "rocchetto: home: big.txt: File too large; not kept between visits") != NULL);
    CHECK(strstr((const char *)kept, "big.txt") == NULL);
    CHECK(strstr((const char *)kept, "hi.filo") != NULL); /* the ones after still go */

    /* the host that cannot keep says so */
    kept_fail = 1;
    send(m, "rm big.txt\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: home: No space left on device; not kept") != NULL);
    kept_fail = 0;
    send(m, "rm hi.filo\r");
    drain(m);
    CHECK(strstr((const char *)kept, "hi.filo") == NULL);

    /* a new visit: the home comes back from the blob, empty dirs too */
    size_t saved = kept_len;
    send(m, "mkdir ~/empty\r");
    drain(m);
    m = boot_kept(&h);
    o = drain(m);
    CHECK(strstr(o, "1 file back in /home/guest.") != NULL);
    send(m, "s\r");
    drain(m);
    send(m, "cat ~/w/imp.red\r");
    o = drain(m);
    CHECK(strstr(o, "MOV 0, 1") != NULL);
    CHECK(vfs_lookup(&m->fs, "/home/guest/empty") != NULL);
    CHECK(vfs_lookup(&m->fs, "/home/guest/empty")->dir);
    CHECK(vfs_lookup(&m->fs, "/home/guest/w")->dir);
    CHECK(saved > 0);

    /* nothing kept: a first visit, silent */
    kept_len = 0;
    m = boot_kept(&h);
    o = drain(m);
    CHECK(strstr(o, "back in") == NULL);
    /* a host without store never asks */
    m = boot_menu(&h);
    CHECK(h.pending == 0);
}

/* The small commands that need no file: each one answers from what the
   shell already holds, and all of them come with help and Tab for free. */
static void test_small_commands(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "ver\r");
    const char *o = drain(m);
    CHECK(strstr(o, "rocchetto " ROC_VERSION "\r\n") != NULL);
    send(m, "whoami\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nguest\r\n") != NULL);
    send(m, "hostname\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nshell.test\r\n") != NULL);
    send(m, "echo  hello   there \r"); /* the words, one space apart, as sh's */
    o = drain(m);
    CHECK(strstr(o, "\r\nhello there\r\n") != NULL);
    /* history is a script reading the session's lines as a file */
    send(m, "history\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nver\r\nwhoami\r\nhostname\r\necho  hello   there\r\nhistory\r\n") != NULL);
    send(m, "hist\t\r"); /* Tab knows them; the repeat is one memory, not two */
    o = drain(m);
    CHECK(strstr(o, "there\r\nhistory\r\n") != NULL);
    CHECK(strstr(o, "history\r\nhistory") == NULL);
    /* dot files are hidden unless asked; -a also shows . and .. */
    send(m, "ls /home/guest\r");
    o = drain(m);
    CHECK(strstr(o, ".history") == NULL);
    send(m, "ls -a /home/guest\r");
    o = drain(m);
    CHECK(strstr(o, ".history") != NULL);
    CHECK(strstr(o, "\r\n.") != NULL && strstr(o, "..") != NULL);
    send(m, "tree /home\r");
    o = drain(m);
    CHECK(strstr(o, "guest/") != NULL && strstr(o, ".history") == NULL);
    send(m, "tree -a /home\r");
    o = drain(m);
    CHECK(strstr(o, "└── .history") != NULL);
    send(m, "cat /home/guest/.h\t"); /* Tab finds it once the dot is typed */
    drain(m);
    CHECK(line_is(m, "cat /home/guest/.history "));
    send(m, "\x15");
    drain(m);
    send(m, "cat /home/guest/.history\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nver\r\n") != NULL);
    send(m, "clear\r");
    o = drain(m);
    CHECK(strstr(o, "\x1b[H\x1b[2J") != NULL);

    /* tree is a recursive Filo script over dir-entries */
    send(m, "tree /lib\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n/lib\r\n└── roc/\r\n    ├── ") != NULL);
    CHECK(strstr(o, "    │   ├── ") != NULL || strstr(o, "    │   └── ") != NULL);
    CHECK(strstr(o, "    └── ") != NULL);
    send(m, "cd /bin\r");
    drain(m);
    send(m, "tree\r"); /* no argument: here */
    o = drain(m);
    CHECK(strstr(o, "\r\n/bin\r\n├── at\r\n├── ") != NULL);
    CHECK(strstr(o, "└── xargs\r\n") != NULL);
    send(m, "tree /nope\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: tree: ") != NULL);
    send(m, "cd /\r");
    drain(m);
    send(m, "pwd\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n/\r\n") != NULL);
}

/* A command the dispatcher does not know is a Filo program in the shell's
   tree, run with ARGS; help is the first one. The shell's own files are
   readable from it, through the same pipeline a fetched file uses. */
static void test_scripts(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);

    /* help is bin/help.filo showing bin/roc_help.md, rendered as markdown */
    send(m, "help\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\x1b[0;1;36;4mrocchetto") != NULL); /* headings and code came out styled */
    CHECK(help_shown(m, o));
    CHECK(m->mode == ROC_MODE_LINE); /* and the prompt is back */
    send(m, "help | grep -c 'Not here'; help > ~/h.txt; wc -l < ~/h.txt\r"); /* whole, not paged */
    o = drain(m);
    CHECK(m->t.napps == 0 && strstr(o, "\r\n1\r\n") != NULL);

    /* the shell's own files are in the tree it walks */
    send(m, "ls /bin\r");
    o = drain(m);
    CHECK(strstr(o, "help") != NULL && strstr(o, "edt") != NULL);
    CHECK(strstr(o, ".filo") == NULL && strstr(o, ".fbb") == NULL); /* programs, by their names */
    CHECK(strstr(o, "roc_help.md") != NULL);
    send(m, "cat /bin/roc_help.md\r");
    o = drain(m);
    CHECK(strstr(o, "edt [file]") != NULL);
    send(m, "less /bin/filo_api.md\r");
    drain(m);
    CHECK(m->t.napps == 1);
    send(m, "q");
    drain(m);
    CHECK(m->t.napps == 0);
    send(m, "cd /lib/roc\r");
    drain(m);
    send(m, "ls -F\r");
    o = drain(m);
    CHECK(strstr(o, "warriors/") != NULL);
    send(m, "cd /\r");
    drain(m);

    /* a tree of the test's own: a script with arguments, and one that fails */
    static const char hi[] = "(echo \"hi\" (nth ARGS 0) \"from\" VERSION)";
    static const char sum[] = "(write (+ 1 1) \" and \" (list 1 2) (= 1 1))";
    static const char bad[] = "(nth ARGS 9)";
    /* programs, as filo build makes them: a source in /bin is no command */
    static uint8_t hi_bc[4096];
    static uint8_t bad_bc[4096];
    static uint8_t sum_bc[4096];
    size_t hi_len = 0;
    size_t bad_len = 0;
    size_t sum_len = 0;
    CHECK(script_build(m, (const uint8_t *)hi, sizeof(hi) - 1, hi_bc, sizeof(hi_bc), &hi_len));
    CHECK(script_build(m, (const uint8_t *)bad, sizeof(bad) - 1, bad_bc, sizeof(bad_bc), &bad_len));
    CHECK(script_build(m, (const uint8_t *)sum, sizeof(sum) - 1, sum_bc, sizeof(sum_bc), &sum_len));
    static tree_file tree[4];
    tree[0] = (tree_file){"bin/hi", hi_bc, hi_len};
    tree[1] = (tree_file){"bin/bad", bad_bc, bad_len};
    tree[2] = (tree_file){"bin/sum", sum_bc, sum_len};
    tree[3] = (tree_file){"bin/src.filo", (const uint8_t *)hi, sizeof(hi) - 1};
    tree_set_source(tree, 4);
    send(m, "sum\r");
    o = drain(m);
    CHECK(strstr(o, "2 and (list 1 2)") != NULL); /* write spells values as echo does */
    send(m, "hi there friend\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nhi there from " ROC_VERSION "\r\n") != NULL);
    send(m, "bad\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: bad: ") != NULL);
    send(m, "nothere\r");
    o = drain(m);
    CHECK(strstr(o, "command not found") != NULL);
    send(m, "src\r"); /* a source is not a command, whatever its directory */
    o = drain(m);
    CHECK(strstr(o, "command not found") != NULL);
    send(m, "/bin/hi by-path\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nhi by-path from " ROC_VERSION "\r\n") != NULL);
    tree_set_source(NULL, 0);

    /* filo <file> and ./name run a file by path: the shell's own from
       memory, the site's after the host sends it */
    const char *note = "(echo \"a script of the home\")";
    CHECK(roc_write_file(m, "test", "/home/guest/hi.filo", (const uint8_t *)note, strlen(note)));
    send(m, "filo ~/hi.filo\r");
    o = drain(m);
    CHECK(strstr(o, "a script of the home") != NULL);
    send(m, "cd ~\r");
    drain(m);
    send(m, "./hi.filo; echo st=$?\r"); /* a source runs with filo, not by its path */
    o = drain(m);
    CHECK(strstr(o, "cannot execute: not a program") != NULL && strstr(o, "st=126") != NULL);
    send(m, "./hi; echo st=$?\r"); /* nothing is guessed: no .filo is added */
    o = drain(m);
    CHECK(strstr(o, "No such file or directory") != NULL && strstr(o, "st=127") != NULL);
    send(m, "cd /bin\r");
    drain(m);
    send(m, "filo nothere.filo\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: filo: /bin/nothere.filo: No such file or directory") != NULL);
    send(m, "cd /\r");
    drain(m);

    CHECK(vfs_add(&m->fs, "/pub/greet.filo", 40, false));
    h.pending = 0;
    send(m, "filo /pub/greet.filo Ada\r");
    CHECK(h.pending == 1);
    CHECK(strcmp(h.path, "/pub/greet.filo") == 0);
    static const char greet[] = "(echo \"hello,\" (nth ARGS 0))";
    roc_feed(m, h.req_id, (const uint8_t *)greet, sizeof(greet) - 1);
    roc_feed_eof(m, h.req_id);
    o = drain(m);
    CHECK(strstr(o, "hello, Ada\r\n") != NULL);
    CHECK(m->mode == ROC_MODE_LINE);
    CHECK(strstr(o, "guest@shell.test") != NULL); /* and the prompt came back */

    /* a program of the site: fetched, then a command runs from its bytes */
    const uint8_t *fbb = NULL;
    size_t len = 0;
    CHECK(tree_find_file("bin/ver", &fbb, &len));
    CHECK(vfs_add(&m->fs, "/pub/ver3.fbb", (uint32_t)len, false));
    h.pending = 0;
    send(m, "filo /pub/ver3.fbb\r");
    CHECK(h.pending == 1 && strcmp(h.path, "/pub/ver3.fbb") == 0);
    roc_feed(m, h.req_id, fbb, len);
    roc_feed_eof(m, h.req_id);
    o = drain(m);
    CHECK(strstr(o, "rocchetto " ROC_VERSION) != NULL && m->mode == ROC_MODE_LINE);
    send(m, "cd /pub\r");
    drain(m);
    h.pending = 0;
    send(m, "./ver3.fbb\r");
    CHECK(h.pending == 1 && strcmp(h.path, "/pub/ver3.fbb") == 0);
    roc_feed(m, h.req_id, fbb, len);
    roc_feed_eof(m, h.req_id);
    o = drain(m);
    CHECK(strstr(o, "rocchetto " ROC_VERSION) != NULL);
    /* an app opens from a copy: the site's is refused, saying so */
    CHECK(tree_find_file("bin/edt", &fbb, &len));
    CHECK(vfs_add(&m->fs, "/pub/ed2.fbb", (uint32_t)len, false));
    h.pending = 0;
    send(m, "filo /pub/ed2.fbb\r");
    CHECK(h.pending == 1);
    roc_feed(m, h.req_id, fbb, len);
    roc_feed_eof(m, h.req_id);
    o = drain(m);
    CHECK(
        strstr(o,
               "rocchetto: ed2: /pub/ed2.fbb: Operation not supported: an app runs from a copy, cp "
               "it home") != NULL);
    CHECK(m->t.napps == 0 && m->mode == ROC_MODE_LINE);
    send(m, "cd /\r");
    drain(m);
}

static void test_fs_commands(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);

    /* vfs_add_path makes the directories on the way */
    CHECK(vfs_add_path(&m->fs, "/home/guest/a/b/c.txt", 3, false));
    CHECK(vfs_lookup(&m->fs, "/home/guest/a") != NULL && vfs_lookup(&m->fs, "/home/guest/a")->dir);
    CHECK(vfs_lookup(&m->fs, "/home/guest/a/b")->dir);
    CHECK(vfs_has_children(&m->fs, "/home/guest/a/b"));
    CHECK(vfs_remove(&m->fs, "/home/guest/a/b/c.txt"));
    CHECK(!vfs_has_children(&m->fs, "/home/guest/a/b"));
    CHECK(vfs_remove(&m->fs, "/home/guest/a/b") && vfs_remove(&m->fs, "/home/guest/a"));

    /* mkdir and rmdir, in the home only, with the usual words */
    send(m, "mkdir ~/w\r");
    drain(m);
    CHECK(vfs_lookup(&m->fs, "/home/guest/w") != NULL);
    send(m, "mkdir ~/w\r");
    const char *o = drain(m);
    CHECK(strstr(o, "rocchetto: mkdir: /home/guest/w: File exists") != NULL);
    send(m, "mkdir /pub/x\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: mkdir: /pub/x: Read-only file system") != NULL);
    send(m, "mkdir ~/no/such\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: mkdir: /home/guest/no/such: No such file or directory") != NULL);
    send(m, "mkdir\r");
    o = drain(m);
    CHECK(strstr(o, "usage: mkdir [-p] dir...") != NULL);

    /* write-file and read-file from a script; cp and mv over what they made */
    static const char w[] = "(write-file \"~/w/imp.red\" \"MOV 0, 1\\n\")";
    static const char r[] = "(write (read-file (nth ARGS 0)))";
    static const tree_file tree[] = {
        {"bin/w.filo", (const uint8_t *)w, sizeof(w) - 1},
        {"bin/r.filo", (const uint8_t *)r, sizeof(r) - 1},
    };
    tree_programs(m, tree, 2);
    send(m, "w\r");
    drain(m);
    CHECK(vfs_lookup(&m->fs, "/home/guest/w/imp.red") != NULL);
    CHECK(vfs_lookup(&m->fs, "/home/guest/w/imp.red")->size == 9);
    send(m, "r ~/w/imp.red\r");
    o = drain(m);
    CHECK(strstr(o, "MOV 0, 1\n") != NULL);
    send(m, "r /pub/kutta.md\r");
    o = drain(m);
    CHECK(strstr(o, "Operation not supported") != NULL);
    tree_set_source(NULL, 0);

    send(m, "rmdir ~/w\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: rmdir: /home/guest/w: Directory not empty") != NULL);
    send(m, "cp ~/w/imp.red ~/w/dwarf.red\r");
    drain(m);
    send(m, "cat ~/w/dwarf.red\r");
    o = drain(m);
    CHECK(strstr(o, "MOV 0, 1") != NULL);
    send(m, "mkdir ~/old\r");
    send(m, "cp /bin/roc_help.md ~/old\r"); /* into a directory: keeps the name */
    drain(m);
    CHECK(vfs_lookup(&m->fs, "/home/guest/old/roc_help.md") != NULL);
    send(m, "cp ~/old/roc_help.md ~/old/roc_help.md\r");
    o = drain(m);
    CHECK(strstr(o, "Invalid argument") != NULL);
    h.pending = 0;
    send(m, "cp /pub/kutta.md ~/k.md\r"); /* the site's: asked of the host */
    CHECK(h.pending == 1 && strcmp(h.path, "/pub/kutta.md") == 0);
    roc_feed_fail(m, h.req_id);
    o = drain(m);
    CHECK(strstr(o, "read error") != NULL);
    send(m, "cp /bin/roc_help.md /pub/k.md\r");
    o = drain(m);
    CHECK(strstr(o, "Read-only file system") != NULL);

    /* mv: rename a file, move a directory with what is in it, refuse the rest */
    send(m, "mv ~/w/dwarf.red ~/w/stone.red\r");
    drain(m);
    CHECK(vfs_lookup(&m->fs, "/home/guest/w/dwarf.red") == NULL);
    const uint8_t *d = NULL;
    size_t n = 0;
    CHECK(ufs_find(&m->uf, "/home/guest/w/stone.red", &d, &n) && n == 9);
    send(m, "mv ~/w ~/old\r"); /* into a directory */
    drain(m);
    CHECK(vfs_lookup(&m->fs, "/home/guest/w") == NULL);
    CHECK(vfs_lookup(&m->fs, "/home/guest/old/w")->dir);
    CHECK(ufs_find(&m->uf, "/home/guest/old/w/imp.red", &d, &n) && n == 9);
    CHECK(vfs_lookup(&m->fs, "/home/guest/old/w/stone.red") != NULL);
    send(m, "mv ~/old ~/old/w/x\r");
    o = drain(m);
    CHECK(strstr(o, "Invalid argument") != NULL);
    send(m, "mv /bin/help ~/h\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: mv: /bin/help: Read-only file system") != NULL);
    send(m, "mv ~/old/roc_help.md ~/old/w\r"); /* onto a file: replaced */
    send(m, "mv ~/old/w/roc_help.md ~/old/w/imp.red\r");
    drain(m);
    CHECK(ufs_find(&m->uf, "/home/guest/old/w/imp.red", &d, &n) && n > 9);
    send(m, "ls ~/old/w\r");
    o = drain(m);
    CHECK(strstr(o, "roc_help.md") == NULL && strstr(o, "imp.red") != NULL);
    send(m, "rm ~/old\r");
    o = drain(m);
    CHECK(strstr(o, "rm: /home/guest/old: is a directory") != NULL);
    send(m, "cd /\r");
    drain(m);
}

/* A narrow terminal: ls keeps the name and the size, each row inside the
   width (a date that wraps breaks every line). */
static void test_narrow_ls(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    roc_resize(m, 30, 20);
    drain(m);
    send(m, "ls /pub\r");
    const char *o = drain(m);
    CHECK(strstr(o, "kutta.md") != NULL && strstr(o, "2026-") == NULL);
    const char *line = strstr(o, "\r\n");
    while (line != NULL) {
        const char *next = strstr(line + 2, "\r\n");
        if (next == NULL) {
            break;
        }
        size_t cols = 0;
        for (const char *p = line + 2; p < next; p++) {
            if (*p == '\x1b') {
                while (p < next && !((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) {
                    p++; /* an escape takes no column */
                }
                continue;
            }
            if (((unsigned char)*p & 0xC0U) != 0x80U) {
                cols++;
            }
        }
        CHECK(cols <= 30);
        line = next;
    }
    roc_resize(m, 80, 24);
    drain(m);
}

/* The host says who is at the keyboard: the prompt and its column
   arithmetic follow. No name is "guest". */
static void test_user_name(void) {
    mock_host h;
    memset(&h, 0, sizeof(h));
    roc_host host = {.filo_extend = roc_host_extend,
                     .ctx = &h,
                     .host_name = "shell.test",
                     .site_base = "https://shell.test",
                     .request = mock_request,
                     .user = "ana"};
    roc_init(&M, &host, 80, 24, ROC_F_NO_SPLASH);
    h.pending = 0;
    roc_feed(&M, h.req_id, (const uint8_t *)test_index, strlen(test_index));
    roc_feed_eof(&M, h.req_id);
    drain(&M);
    send(&M, roc_layer_spec.home != NULL ? "s\r" : "\r");
    const char *o = drain(&M);
    CHECK(strstr(o, "ana@shell.test") != NULL);
    send(&M, "x\x1b[D"); /* prompt is 18 columns now, not 20 */
    o = drain(&M);
    CHECK(strstr(o, "\r\x1b[18C") != NULL);

    /* a name the prompt cannot hold falls back to guest */
    roc_host bad = {
        .filo_extend = roc_host_extend, .ctx = &h, .request = mock_request, .user = "not a name"};
    roc_init(&M, &bad, 80, 24, ROC_F_NO_SPLASH);
    drain(&M);
    CHECK(strcmp(M.user, "guest") == 0);
}

static void test_index_failure(void) {
    mock_host h;
    memset(&h, 0, sizeof(h));
    roc_host host = {.filo_extend = roc_host_extend,
                     .ctx = &h,
                     .host_name = "shell.test",
                     .site_base = "https://shell.test",
                     .request = mock_request};
    roc_init(&M, &host, 80, 24, ROC_F_NO_SPLASH);
    roc_feed_fail(&M, h.req_id);
    const char *o = drain(&M);
    CHECK(strstr(o, "no index; ls/cat unavailable") != NULL);
    send(&M, "cd /pub\r");
    o = drain(&M);
    CHECK(strstr(o, "rocchetto: no index") != NULL);
}

/* ---- the editor ---- */

static void test_tbuf(void) {
    static tbuf t;
    tb_init(&t);
    CHECK(t.nlines == 1 && t.len == 0);
    CHECK(tb_load(&t, (const uint8_t *)"ab\tc\n\xc3\xa9x\n\xe4\xb8\xad y\n", 15));
    CHECK(t.nlines == 4); /* three lines and the empty one after the last newline */
    const uint8_t *p = NULL;
    size_t n = 0;
    tb_line(&t, 0, &p, &n);
    CHECK(n == 4 && memcmp(p, "ab\tc", 4) == 0);
    tb_line(&t, 3, &p, &n);
    CHECK(n == 0);
    /* columns: the tab reaches the next stop, é is one, 中 is two */
    CHECK(tb_col_of(&t, 3) == 4);     /* after "ab\t" */
    CHECK(tb_col_of(&t, 4) == 5);     /* after "ab\tc" */
    CHECK(tb_col_of(&t, 8) == 2);     /* after "éx" */
    CHECK(tb_col_of(&t, 13) == 3);    /* after "中 " */
    CHECK(tb_off_at(&t, 0, 3) == 2);  /* inside the tab: before it */
    CHECK(tb_off_at(&t, 0, 4) == 3);  /* at c */
    CHECK(tb_off_at(&t, 0, 99) == 4); /* the line's end */
    CHECK(tb_line_of(&t, 5) == 1 && tb_line_of(&t, 14) == 2 && tb_line_of(&t, 15) == 3);
    /* movement keeps the wanted column across lines */
    t.cur = 4; /* end of line 0, column 5 */
    t.goal = 5;
    tb_down(&t, 1, false);
    CHECK(t.cur == 8); /* line 1 ends at column 2 */
    tb_down(&t, 1, false);
    CHECK(t.cur == 14 && tb_col_of(&t, t.cur) == 4); /* "中 y" is 4 wide */
    tb_up(&t, 2, false);
    CHECK(t.cur == 4);
    tb_home(&t, false);
    CHECK(t.cur == 0);
    tb_end(&t, false);
    CHECK(t.cur == 4);
    /* selection with shift, copy, cut, paste */
    tb_left(&t, true);
    tb_left(&t, true);
    size_t a = 0;
    size_t b = 0;
    CHECK(tb_selection(&t, &a, &b) && a == 2 && b == 4);
    tb_copy(&t);
    CHECK(t.clip_len == 2 && memcmp(t.clip, "\tc", 2) == 0);
    tb_cut(&t);
    CHECK(t.len == 13 && t.cur == 2 && !t.anchored);
    tb_line(&t, 0, &p, &n);
    CHECK(n == 2 && memcmp(p, "ab", 2) == 0);
    CHECK(tb_paste(&t) && t.len == 15 && t.cur == 4);
    CHECK(t.dirty);
    /* typing over a selection replaces it; backspace and delete by rune */
    tb_doc_home(&t, false);
    tb_right(&t, true);
    CHECK(tb_insert(&t, (const uint8_t *)"Z", 1));
    tb_line(&t, 0, &p, &n);
    CHECK(n == 4 && memcmp(p, "Zb\tc", 4) == 0);
    tb_down(&t, 1, false);
    tb_end(&t, false);
    tb_backspace(&t); /* x */
    tb_backspace(&t); /* é, two bytes */
    tb_line(&t, 1, &p, &n);
    CHECK(n == 0);
    tb_delete(&t); /* the newline: joins the lines */
    CHECK(t.nlines == 3);
    tb_line(&t, 1, &p, &n);
    CHECK(n == 5 && memcmp(p, "\xe4\xb8\xad y", 5) == 0);
    tb_delete_line(&t);
    CHECK(t.nlines == 2);
    tb_line(&t, 1, &p, &n);
    CHECK(n == 0);
    /* find wraps and selects */
    CHECK(tb_load(&t, (const uint8_t *)"one two\nthree two\n", 18));
    CHECK(tb_find(&t, (const uint8_t *)"two", 3) && t.cur == 4 && t.anchor == 7);
    CHECK(tb_find(&t, (const uint8_t *)"two", 3) && t.cur == 14);
    CHECK(tb_find(&t, (const uint8_t *)"two", 3) && t.cur == 4); /* around */
    CHECK(!tb_find(&t, (const uint8_t *)"four", 4));
    /* the view follows the cursor */
    tb_doc_end(&t, false);
    tb_view(&t, 1, 5);
    CHECK(t.top == 2 && t.left == 0);
    t.cur = 7;
    tb_view(&t, 1, 5);
    CHECK(t.top == 0 && t.left == 3);
    /* limits */
    static uint8_t big[TB_CAP + 1];
    memset(big, 'x', sizeof(big));
    CHECK(!tb_load(&t, big, sizeof(big)));
    CHECK(tb_load(&t, big, TB_CAP));
    CHECK(!tb_insert(&t, (const uint8_t *)"y", 1));
}

/* edt with no file: a text with no name yet, and the first save asks for
   one; quitting with changes asks too, and saving ends it. */
static void test_editor_without_a_file(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "edt\r");
    drain(m);
    CHECK(m->t.napps == 1 && strcmp(m->scr.name, "edt") == 0);
    CHECK(canvas_has(&m->cmp.target, "(new)  Line 1  Col 1"));
    send(m, "draft");
    send(m, "\x13"); /* ^S */
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "Save as:"));
    send(m, "~/draft.txt\r");
    drain(m);
    const uint8_t *d = NULL;
    size_t n = 0;
    CHECK(ufs_find(&m->uf, "/home/guest/draft.txt", &d, &n) && n == 5 &&
          memcmp(d, "draft", 5) == 0);
    CHECK(canvas_has(&m->cmp.target, "Saved /home/guest/draft.txt"));
    send(m, "\x11"); /* ^Q: nothing to save, so out */
    drain(m);
    CHECK(m->t.napps == 0);

    send(m, "edit\r");
    drain(m);
    send(m, "x\x11y");
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "Save as:"));
    send(m, "~/left.txt\r");
    drain(m);
    CHECK(m->t.napps == 0);
    CHECK(ufs_find(&m->uf, "/home/guest/left.txt", &d, &n) && n == 1 && d[0] == 'x');
}

static void test_editor(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);

    /* refusals come before the screen */
    send(m, "edit /pub\r");
    const char *o = drain(m);
    CHECK(strstr(o, "Is a directory") != NULL);
    send(m, "edit /pub/new.txt\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: edt: /pub/new.txt: Read-only file system") != NULL);
    CHECK(m->t.napps == 0);

    /* a new file: type, save, see it in the home */
    send(m, "edit ~/n.txt\r");
    drain(m);
    CHECK(m->t.napps == 1 && strcmp(m->scr.name, "edt") == 0);
    CHECK(canvas_has(&m->cmp.target, "^S save  ^Q quit  ^F find  ^G next  Esc menu"));
    CHECK(canvas_has(&m->cmp.target, "/home/guest/n.txt  Line 1  Col 1"));
    send(m, "hello\rworld");
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "hello"));
    CHECK(canvas_has(&m->cmp.target, "world"));
    CHECK(canvas_has(&m->cmp.target, "/home/guest/n.txt *  Line 2  Col 6"));
    send(m, "\x13"); /* ^S */
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "Saved /home/guest/n.txt"));
    const uint8_t *d = NULL;
    size_t n = 0;
    CHECK(ufs_find(&m->uf, "/home/guest/n.txt", &d, &n) && n == 11 &&
          memcmp(d, "hello\nworld", 11) == 0);
    /* select with Shift+arrows, copy with Ctrl-Ins, paste with Shift-Ins */
    send(m, "\x1b[1;2D\x1b[1;2D\x1b[1;2D"); /* three left, selecting "rld" */
    drain(m);
    CHECK(m->ed.tb.anchored);
    send(m, "\x1b[2;5~"); /* Ctrl-Ins */
    send(m, "\x1b[F");    /* End */
    send(m, "\x1b[2;2~"); /* Shift-Ins */
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "worldrld"));
    /* ^Q on a changed file asks; N leaves without saving */
    send(m, "\x11");
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "Save changes?"));
    send(m, "n");
    o = drain(m);
    CHECK(m->t.napps == 0 && m->mode == ROC_MODE_LINE);
    CHECK(strstr(o, "guest@shell.test") != NULL);
    send(m, "cat ~/n.txt\r");
    o = drain(m);
    CHECK(strstr(o, "hello\r\nworld\r\n") != NULL);

    /* one of the shell's own opens and saves: the copy shadows the tree,
       and the tree itself is untouched underneath */
    send(m, "edit /lib/roc/warriors/imp.red\r");
    drain(m);
    CHECK(canvas_has(&m->cmp.target, ";name Imp"));
    send(m, "\x13");
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "Saved /lib/roc/warriors/imp.red"));
    CHECK(ufs_find(&m->uf, "/lib/roc/warriors/imp.red", &d, &n));
    send(m, "\x1b");
    roc_tick(m, 200); /* a lone ESC: the menu */
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "Save  As  Quit  Delete  Copy  Paste"));
    send(m, "a");
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "Save as: "));
    CHECK(m->scr.in.on);
    send(m, "~/w.red\r");
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "Saved /home/guest/w.red"));
    CHECK(ufs_find(&m->uf, "/home/guest/w.red", &d, &n) && n == 152);
    send(m, "\x11");
    drain(m);
    CHECK(m->t.napps == 0);
    /* find selects the match; ^L strips the chrome; ^Q leaves at once when clean */
    const char *ver = "(echo \"rocchetto\" VERSION)\n";
    CHECK(roc_write_file(m, "test", "/home/guest/v.filo", (const uint8_t *)ver, strlen(ver)));
    send(m, "edit ~/v.filo\r");
    drain(m);
    send(m, "\x06");
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "Find: "));
    send(m, "VERSION\r");
    drain(m);
    size_t a = 0;
    size_t b = 0;
    CHECK(tb_selection(&m->ed.tb, &a, &b) && b - a == 7);
    send(m, "\x0c");
    drain(m);
    CHECK(!canvas_has(&m->cmp.target, "Esc menu"));
    CHECK(strncmp(canvas_row(&m->cmp.target, 0), "(echo \"rocchetto\" VERSION)", 20) == 0);
    send(m, "\x0c");
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "Esc menu"));
    send(m, "\x11");
    drain(m);
    CHECK(m->t.napps == 0);
    send(m, "filo ~/v.filo\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto " ROC_VERSION) != NULL);

    /* the colon line: Esc, then :2, :w name, :q! */
    send(m, "edit ~/n.txt\r");
    drain(m);
    send(m, "\x1b");
    roc_tick(m, 200);
    send(m, ":2\r");
    drain(m);
    CHECK(tb_line_of(&m->ed.tb, m->ed.tb.cur) == 1);
    send(m, "\x1b");
    roc_tick(m, 200);
    send(m, ":w ~/n2.txt\r");
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "Saved /home/guest/n2.txt"));
    send(m, "\x1b");
    roc_tick(m, 200);
    send(m, ":frob\r");
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "edt: :frob: command not found"));
    send(m, "\x1b");
    roc_tick(m, 200);
    send(m, ":q!\r");
    drain(m);
    CHECK(m->t.napps == 0);

    /* a copy goes to the terminal's clipboard as OSC 52; ^P previews */
    send(m, "edit ~/v.filo\r");
    drain(m);
    send(m, "\x1b[1;2C\x1b[1;2C\x1b[1;2C"); /* select "(ec" */
    send(m, "\x1b[2;5~");                   /* Ctrl-Ins */
    o = drain(m);
    CHECK(strstr(o, "\x1b]52;c;KGVj\x1b\\") != NULL); /* base64 of "(ec" */
    send(m, "\x10");
    drain(m);
    CHECK(m->t.napps == 2); /* the pager over the editor */
    CHECK(canvas_has(&m->cmp.target, "(echo \"rocchetto\" VERSION)"));
    send(m, "q");
    drain(m);
    CHECK(m->t.napps == 1 && strcmp(m->scr.name, "edt") == 0);
    send(m, "\x11");
    drain(m);
    CHECK(m->t.napps == 0);
}

/* filo alone is the language at a prompt; site files reach cp and edit */
/* The commands of /bin are programs, compiled with the shell and named as
   the command, no extension: a file runs by its first bytes. A program of
   the user's own in /bin runs over the shell's, and removing it brings the
   board's back; a source there is no command. The programs are files
   anyone can open or download, as the shell's others are. */
static void test_commands_run_from_units(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    const uint8_t *fbb = NULL;
    size_t len = 0;
    CHECK(tree_find_file("bin/pwd", &fbb, &len) && roc_is_program(fbb, len));
    const uint8_t *unit = NULL;
    size_t ulen = 0;
    CHECK(filo_bundle_find(&m->sc.ctx, fbb, len, "pwd", &unit, &ulen) == FILO_OK);
    CHECK(!tree_find_file("bin/pwd.filo", &fbb, &len)); /* no source on board */
    send(m, "pwd\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\n/\r\n") != NULL); /* the cwd a session starts in */
    const char *mine = "(echo \"mine\")";
    CHECK(roc_write_file(m, "test", "/bin/pwd.filo", (const uint8_t *)mine, strlen(mine)));
    send(m, "pwd\r"); /* a source beside it changes nothing */
    o = drain(m);
    CHECK(strstr(o, "\r\n/\r\n") != NULL && strstr(o, "mine") == NULL);
    static uint8_t built[4096];
    size_t blen = 0;
    CHECK(script_build(m, (const uint8_t *)mine, strlen(mine), built, sizeof(built), &blen));
    CHECK(roc_write_file(m, "test", "/bin/pwd", built, blen));
    send(m, "pwd\r");
    o = drain(m);
    CHECK(strstr(o, "mine") != NULL);
    send(m, "rm /bin/pwd /bin/pwd.filo\r");
    drain(m);
    send(m, "pwd\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n/\r\n") != NULL && strstr(o, "mine") == NULL);
    /* the sources the programs came from: Filo to read, copy and run */
    /* the sources the programs came from are the site's, /pub/filo/examples: not in
       the shell's tree, fetched to read or run */
    CHECK(!tree_find_file("examples/ver.filo", &fbb, &len));
    static uint8_t src[4096];
    size_t src_len = command_source("ver.filo", src, sizeof(src));
    CHECK(src_len > 0 && vfs_add(&m->fs, "/pub/filo/examples/ver.filo", (uint32_t)src_len, false));
    h.pending = 0;
    send(m, "cat /pub/filo/examples/ver.filo\r");
    CHECK(h.pending == 1 && strcmp(h.path, "/pub/filo/examples/ver.filo") == 0);
    roc_feed(m, h.req_id, src, src_len);
    roc_feed_eof(m, h.req_id);
    o = drain(m);
    CHECK(strstr(o, "(echo \"rocchetto\" VERSION)") != NULL);
    h.pending = 0;
    send(m, "filo /pub/filo/examples/ver.filo\r");
    CHECK(h.pending == 1);
    roc_feed(m, h.req_id, src, src_len);
    roc_feed_eof(m, h.req_id);
    o = drain(m);
    CHECK(strstr(o, "rocchetto " ROC_VERSION) != NULL);
    /* a program of the user's own in /bin is a command too: the shell's
       ver, copied under another name, still runs (its one member is "ver");
       the name is the whole file's, nothing taken off it */
    CHECK(tree_find_file("bin/ver", &fbb, &len));
    CHECK(roc_write_file(m, "test", "/bin/ver2", fbb, len));
    CHECK(roc_write_file(m, "test", "/bin/ver3.fbb", fbb, len));
    send(m, "ver2\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto " ROC_VERSION) != NULL);
    send(m, "ver3\r");
    o = drain(m);
    CHECK(strstr(o, "command not found") != NULL);
    /* PATH says where: a directory of the user's, before /bin */
    send(m, "mkdir ~/bin\r");
    drain(m);
    CHECK(roc_write_file(m, "test", "/home/guest/bin/ver4", fbb, len));
    send(m, "ver4; PATH=~/bin:/bin; ver4; type ver4; echo $PATH\r");
    o = drain(m);
    CHECK(strstr(o, "command not found") != NULL && strstr(o, "rocchetto " ROC_VERSION) != NULL &&
          strstr(o, "ver4 is /home/guest/bin/ver4") != NULL &&
          strstr(o, "/home/guest/bin:/bin") != NULL);
    send(m, "PATH=/nothing; pwd; PATH=/bin\r"); /* no PATH, no programs */
    o = drain(m);
    CHECK(strstr(o, "pwd: command not found") != NULL);
    /* built here, as the help says: filo build, then by its path and by name */
    const char *hello = "(echo \"hello\" (nth ARGS 0))";
    CHECK(
        roc_write_file(m, "test", "/home/guest/hello.filo", (const uint8_t *)hello, strlen(hello)));
    send(m,
         "cd; filo build -o ~/bin/hello hello.filo; ./bin/hello one; PATH=~/bin:/bin; hello two; "
         "PATH=/bin\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nhello one\r\n") != NULL && strstr(o, "\r\nhello two\r\n") != NULL);
    /* and by its path, from the home: filo x.fbb, ./x.fbb */
    CHECK(roc_write_file(m, "test", "/home/guest/v.fbb", fbb, len));
    send(m, "filo ~/v.fbb\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto " ROC_VERSION) != NULL);
    send(m, "cd ~\r");
    drain(m);
    send(m, "./v.fbb\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto " ROC_VERSION) != NULL);
    /* an app by its path: the editor copied home opens as itself */
    CHECK(tree_find_file("bin/edt", &fbb, &len));
    CHECK(roc_write_file(m, "test", "/home/guest/mine.fbb", fbb, len));
    send(m, "/home/guest/mine.fbb ~/note.txt\r");
    drain(m);
    CHECK(m->t.napps == 1 && strcmp(m->scr.name, "mine") == 0 && m->scr.app);
    CHECK(canvas_has(&m->cmp.target, "/home/guest/note.txt  Line 1  Col 1"));
    send(m, "\x11");
    drain(m);
    CHECK(m->t.napps == 0);
}

/* filo's tools on the shell's files, as the desktop has them: what a
   program of /bin is, where its bytes go, its Filo back; and a program of
   the user's own built, run, decompiled and built again the same. */
static void test_filo_tools(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "filo -h\r");
    const char *o = drain(m);
    CHECK(strstr(o, "filo decompile [-o DIR] FILE [MEMBER]") != NULL);
    send(m, "filo dump -h\r");
    o = drain(m);
    CHECK(strstr(o, "usage: filo dump FILE") != NULL);
    send(m, "filo size /bin/ver\r");
    o = drain(m);
    CHECK(strstr(o, "ver  ") != NULL && strstr(o, "  header ") != NULL &&
          strstr(o, "  code ") != NULL);
    send(m, "filo check /bin/ver\r");
    o = drain(m);
    CHECK(strstr(o, "ver  runs: ") != NULL);
    send(m, "filo check /bin/edt\r"); /* an app: held to what a screen has */
    o = drain(m);
    CHECK(strstr(o, "edt  runs: ") != NULL);
    const char *vm = "# a VM of two names\necho\nglobal VERSION\n";
    CHECK(roc_write_file(m, "test", "/home/guest/two.vm", (const uint8_t *)vm, strlen(vm)));
    send(m, "filo check -vm ~/two.vm /bin/ver\r"); /* a value is listed as one */
    o = drain(m);
    CHECK(strstr(o, "ver  runs: ") != NULL);
    send(m, "filo dump /bin/ver\r");
    o = drain(m);
    CHECK(strstr(o, "unit: ") != NULL && strstr(o, "CALLB") != NULL && strstr(o, "echo") != NULL);
    send(m, "filo decompile /bin/ver\r");
    o = drain(m);
    CHECK(strstr(o, "(echo \"rocchetto\" VERSION)") != NULL);
    static uint8_t ver_src[4096];
    size_t ver_len = command_source("ver.filo", ver_src, sizeof(ver_src));
    CHECK(roc_write_file(m, "test", "/home/guest/ver.filo", ver_src, ver_len));
    send(m, "filo fmt ~/ver.filo\r"); /* formatted already: as it is */
    o = drain(m);
    CHECK(strstr(o, "(echo \"rocchetto\" VERSION)") != NULL);
    const char *messy = "(def  f (fn (x)\n\n  (* x x)))";
    CHECK(
        roc_write_file(m, "test", "/home/guest/messy.filo", (const uint8_t *)messy, strlen(messy)));
    send(m, "filo fmt -w ~/messy.filo\r");
    drain(m);
    const char *tidy_want = "(def f\n  (fn (x)\n\n    (* x x)))\n";
    const uint8_t *tidy = NULL;
    size_t tidy_len = 0;
    CHECK(roc_find_file(m, "/home/guest/messy.filo", &tidy, &tidy_len));
    CHECK(tidy_len == strlen(tidy_want) && memcmp(tidy, tidy_want, tidy_len) == 0);
    send(m, "filo show ir ~/ver.filo\r");
    o = drain(m);
    CHECK(strstr(o, "callb echo") != NULL);
    /* the user's own: main.filo is a command, built, run, and back again */
    const char *hi = "(def greet (fn (x) (str-concat \"hi \" x)))\n(echo (greet \"there\"))\n";
    CHECK(roc_write_file(m, "test", "/home/guest/main.filo", (const uint8_t *)hi, strlen(hi)));
    send(m, "filo build -o ~/hi.fbb ~/main.filo\r");
    o = drain(m);
    CHECK(strstr(o, "filo:") == NULL);
    send(m, "filo ~/hi.fbb\r");
    o = drain(m);
    CHECK(strstr(o, "hi there") != NULL);
    send(m, "mkdir ~/back\r");
    drain(m);
    send(m, "filo decompile -o ~/back ~/hi.fbb\r");
    o = drain(m);
    CHECK(strstr(o, "/home/guest/back/main.filo") != NULL);
    send(m, "filo build -o ~/again.fbb ~/back/main.filo\r");
    drain(m);
    const uint8_t *a = NULL;
    const uint8_t *b = NULL;
    size_t alen = 0;
    size_t blen = 0;
    CHECK(roc_find_file(m, "/home/guest/hi.fbb", &a, &alen));
    CHECK(roc_find_file(m, "/home/guest/again.fbb", &b, &blen));
    static uint8_t sa[1U << 16U];
    static uint8_t sb[1U << 16U];
    size_t na = 0;
    size_t nb = 0;
    CHECK(filo_bc_strip(&m->sc.ctx, a, alen, sa, sizeof(sa), &na) == FILO_OK);
    CHECK(filo_bc_strip(&m->sc.ctx, b, blen, sb, sizeof(sb), &nb) == FILO_OK);
    CHECK(na == nb && memcmp(sa, sb, na) == 0); /* the same unit, but for positions */
    /* a program built from a file of any name: its one entry is the command */
    CHECK(roc_write_file(m, "test", "/home/guest/greet.filo", (const uint8_t *)hi, strlen(hi)));
    send(m, "filo build -o ~/greet.fbc ~/greet.filo\r");
    drain(m);
    send(m, "filo ~/greet.fbc\r");
    o = drain(m);
    CHECK(strstr(o, "hi there") != NULL);
    /* a file the tools cannot find, and one that is not a unit */
    send(m, "filo size ~/nothere.fbb\r");
    o = drain(m);
    CHECK(strstr(o, "No such file or directory") != NULL);
    send(m, "filo decompile ~/main.filo\r");
    o = drain(m);
    CHECK(strstr(o, "a source is Filo already") != NULL);
}

/* filo debug: a program stepped on the screen as the desktop's debugger
   steps it, its source on the left, its code on the right, its calls
   below; going back and a breakpoint; the bytes and the help. */
static void test_filo_debug(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    const canvas *c = &m->cmp.target;
    const char *src = "(def sq (fn (x)\n  (* x x)))\n(echo \"out\" (sq 3))\n(sq 7)\n";
    CHECK(roc_write_file(m, "test", "/home/guest/sq.filo", (const uint8_t *)src, strlen(src)));
    send(m, "filo debug ~/sq.filo\r");
    drain(m);
    CHECK(m->mode == ROC_MODE_APP);
    CHECK(canvas_has(c, "sq.filo"));
    CHECK(canvas_has(c, "(def sq (fn (x)"));
    CHECK(canvas_has(c, "at fn 0 (entry \"sq\")"));
    CHECK(canvas_has(c, "CLOSURE  1         fn 1"));
    CHECK(canvas_has(c, "sq  step 0  1:"));
    CHECK(canvas_has(c, "#0 fn 0 \"sq\"  sq.filo 1:"));
    send(m, "i");
    CHECK(canvas_has(c, "sq  step 1"));
    CHECK(canvas_has(c, "STORE_G takes 1"));
    send(m, "s"); /* to line 3 */
    CHECK(canvas_has(c, "  3:"));
    send(m, "s"); /* into sq, from (sq 3) */
    CHECK(canvas_has(c, "#0 fn 1  sq.filo 2:"));
    CHECK(canvas_has(c, "slots (3)"));
    CHECK(canvas_has(c, "#1 fn 0 \"sq\"  sq.filo 3:"));
    send(m, "c");
    CHECK(canvas_has(c, "returned 49"));
    CHECK(canvas_has(c, " out: out 9 "));
    send(m, "c");
    CHECK(canvas_has(c, "the run has ended"));
    send(m, "b"); /* before the continue */
    CHECK(canvas_has(c, "#0 fn 1  sq.filo 2:"));
    CHECK(!canvas_has(c, "returned 49"));
    /* a breakpoint on line 4 stops the run as it comes to it */
    send(m, "r");
    send(m, "\x1b[B\x1b[B\x1b[B");
    send(m, " ");
    send(m, "c");
    CHECK(canvas_has(c, "breakpoint at sq.filo:4"));
    send(m, "x");
    CHECK(canvas_has(c, ", byte "));
    CHECK(canvas_has(c, " code       00"));
    CHECK(canvas_has(c, "x code"));
    send(m, "x");
    send(m, "h");
    CHECK(canvas_has(c, "help  1-"));
    CHECK(canvas_has(c, "A program runs here one step at a time."));
    send(m, "h");
    CHECK(canvas_has(c, "sq  step "));
    send(m, "q");
    drain(m);
    CHECK(m->mode == ROC_MODE_LINE);
    /* a board program: no source beside it, the code all the same */
    send(m, "filo debug /bin/ver\r");
    drain(m);
    CHECK(canvas_has(c, "(no source: main.filo is not beside"));
    CHECK(canvas_has(c, "main  step 0"));
    send(m, "c");
    CHECK(canvas_has(c, "returned"));
    send(m, "q");
    const char *o = drain(m);
    send(m, "filo debug ~/sq.filo nope\r");
    o = drain(m);
    CHECK(strstr(o, "No entry point named nope") != NULL);
    send(m, "filo debug ~/nothere.filo\r");
    o = drain(m);
    CHECK(strstr(o, "No such file or directory") != NULL);
    send(m, "filo debug -h\r");
    o = drain(m);
    CHECK(strstr(o, "usage: filo debug [-src DIR] FILE [MEMBER] [ENTRY]") != NULL);
}

static const char *fake_vars(void *user, const char *name, size_t len) {
    (void)user;
    if (len == 1 && name[0] == 'x') {
        return "a  b";
    }
    if (len == 1 && name[0] == 'e') {
        return "";
    }
    if (len == 4 && memcmp(name, "HOME", 4) == 0) {
        return "/home/t";
    }
    if (len == 1 && name[0] == '?') {
        return "3";
    }
    return NULL;
}

/* $ and ~ as sh expands them: unquoted, split into fields; quoted, whole;
   in single quotes, not at all; and NAME=value before the command. */
static void test_shell_expansions(void) {
    static sh_line sl;
    char why[64];
    CHECK(sh_read("echo $x \"$x\" ${x}c $e \"$e\" '$x' \\$x $ $? $none", &sl, fake_vars, NULL, why,
                  sizeof(why)));
    const char *want[] = {"echo", "a", "b", "a  b", "a", "bc", "", "$x", "$x", "$", "3"};
    CHECK(sl.argc == 11);
    for (int i = 0; i < 11 && i < sl.argc; i++) {
        CHECK(strcmp(sl.argv[i], want[i]) == 0);
    }
    CHECK(sh_read("A=1 B=$x _c= echo D=2 'E=3'", &sl, fake_vars, NULL, why, sizeof(why)));
    CHECK(sl.nassign == 3 && strcmp(sl.assign[1], "B=a  b") == 0 &&
          strcmp(sl.assign[2], "_c=") == 0);
    CHECK(sl.argc == 3 && strcmp(sl.argv[1], "D=2") == 0 && strcmp(sl.argv[2], "E=3") == 0);
    CHECK(sh_read("'A'=1 1A=2 A-B=3", &sl, fake_vars, NULL, why, sizeof(why)));
    CHECK(sl.nassign == 0 && sl.argc == 3);
    CHECK(sh_read("ls ~ ~/p a~ '~' ~x > ~/o", &sl, fake_vars, NULL, why, sizeof(why)));
    CHECK(sl.argc == 6 && strcmp(sl.argv[1], "/home/t") == 0 &&
          strcmp(sl.argv[2], "/home/t/p") == 0);
    CHECK(strcmp(sl.argv[3], "a~") == 0 && strcmp(sl.argv[4], "~") == 0 &&
          strcmp(sl.argv[5], "~x") == 0);
    CHECK(sl.out != NULL && strcmp(sl.out, "/home/t/o") == 0);
    CHECK(sh_read("echo > $x", &sl, fake_vars, NULL, why, sizeof(why)) &&
          strcmp(sl.out, "a  b") == 0);
    CHECK(!sh_read("echo ${x", &sl, fake_vars, NULL, why, sizeof(why)));
    CHECK(strcmp(why, "Syntax error: Missing '}'") == 0);
    CHECK(!sh_read("echo ${1a}", &sl, fake_vars, NULL, why, sizeof(why)));
    CHECK(strcmp(why, "Bad substitution") == 0);
    /* sh_split reads words back as they are: nothing expands there */
    CHECK(sh_split("A=1 $x ~", &sl, why, sizeof(why)) && sl.nassign == 0 && sl.argc == 3);
    CHECK(strcmp(sl.argv[1], "$x") == 0 && strcmp(sl.argv[2], "~") == 0);

    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "x='a  b'\r");
    drain(m);
    send(m, "echo $x \"[$x]\" ${x}!\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\na b [a  b] a b!\r\n") != NULL);
    send(m, "set\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nx='a  b'\r\n") != NULL);
    send(m, "unset x\r");
    drain(m);
    send(m, "echo \"[$x]\" $USER ~ $PWD\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n[] guest /home/guest /\r\n") != NULL); /* the tests start at / */
    send(m, "HOME=/tmp\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: HOME: readonly variable") != NULL);
    send(m, "echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n1\r\n") != NULL);
    send(m, "nosuchcommand\r");
    drain(m);
    send(m, "echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n127\r\n") != NULL);
    send(m, "echo a; ; echo b\r");
    drain(m);
    send(m, "echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n2\r\n") != NULL);
    send(m, "echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n0\r\n") != NULL);
    send(m, "d=/pub\r");
    drain(m);
    send(m, "cd $d\r");
    drain(m);
    send(m, "pwd\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n/pub\r\n") != NULL);
}

/* A line of commands: ; runs the next anyway, && when the last ended with
   0, || when it did not; the whole line is read before any of it runs,
   and a command that waits for the site has the rest wait with it. */
static void test_shell_lists(void) {
    static sh_line sl;
    char why[64];
    CHECK(sh_read("a; b", &sl, fake_vars, NULL, why, sizeof(why)));
    CHECK(sl.argc == 1 && sl.op == SH_OP_SEMI && sl.next == 2);
    CHECK(sh_read("echo 'a;b' c&&d", &sl, fake_vars, NULL, why, sizeof(why)));
    CHECK(sl.argc == 3 && strcmp(sl.argv[1], "a;b") == 0 && sl.op == SH_OP_AND);
    CHECK(sh_read("a||b", &sl, fake_vars, NULL, why, sizeof(why)) && sl.op == SH_OP_OR &&
          sl.next == 3);
    CHECK(sh_read("a|b c&d", &sl, fake_vars, NULL, why, sizeof(why)) && sl.argc == 1);
    CHECK(sl.op == SH_OP_PIPE && sl.next == 2);
    CHECK(sh_read("b c&d", &sl, fake_vars, NULL, why, sizeof(why)) && sl.argc == 2);
    CHECK(sl.op == SH_OP_BG && sl.next == 4); /* & ends it: run, not in the background */
    CHECK(sh_read("echo a # b; c", &sl, fake_vars, NULL, why, sizeof(why)) && sl.op == SH_OP_END);
    CHECK(sh_split("a;b", &sl, why, sizeof(why)) && sl.argc == 1 && sl.op == SH_OP_END);
    CHECK(sh_check("a; b && c || d;", why, sizeof(why)));
    CHECK(sh_check("", why, sizeof(why)) && sh_check("x=1; > f", why, sizeof(why)));
    CHECK(!sh_check("a;; b", why, sizeof(why)) &&
          strcmp(why, "Syntax error: \";;\" unexpected") == 0);
    CHECK(!sh_check("&& a", why, sizeof(why)) &&
          strcmp(why, "Syntax error: \"&&\" unexpected") == 0);
    CHECK(!sh_check("a ||", why, sizeof(why)));
    CHECK(strcmp(why, "Syntax error: end of file unexpected") == 0);
    CHECK(!sh_check("a; b 'c", why, sizeof(why)));

    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "echo a; echo b\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\na\r\nb\r\n") != NULL);
    send(m, "x=5; echo $x\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n5\r\n") != NULL);
    send(m, "ls /nothere || echo fallback $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nfallback 1\r\n") != NULL);
    send(m, "ls /nothere && echo never; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nnever\r\n") == NULL && strstr(o, "\r\n1\r\n") != NULL);
    send(m, "echo x && echo y || echo z\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nx\r\ny\r\n") != NULL && strstr(o, "\r\nz\r\n") == NULL);
    send(m, "echo first; echo 'open\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nfirst\r\n") == NULL && strstr(o, "\r\n> ") != NULL);
    send(m, "'\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nfirst\r\n") != NULL);
    send(m, "echo one > ~/l1.txt; echo two\r");
    o = drain(m);
    CHECK(strstr(o, "\r\ntwo\r\n") != NULL);
    const uint8_t *d = NULL;
    size_t n = 0;
    CHECK(ufs_find(&m->uf, "/home/guest/l1.txt", &d, &n) && n == 4 && memcmp(d, "one\n", 4) == 0);
    /* a file from the site: the rest of the line waits for it */
    h.pending = 0;
    send(m, "cat /pub/kutta.md; echo after\r");
    o = drain(m);
    CHECK(h.pending == 1 && strstr(o, "\r\nafter\r\n") == NULL);
    const char *body = "hello\n";
    roc_feed(m, h.req_id, (const uint8_t *)body, strlen(body));
    roc_feed_eof(m, h.req_id);
    o = drain(m);
    CHECK(strstr(o, "hello") != NULL && strstr(o, "\r\nafter\r\n") != NULL);
    CHECK(strstr(strstr(o, "\r\nafter\r\n"), "$ ") != NULL); /* the prompt after the last */
    /* ^C while it waits: the whole line goes */
    h.pending = 0;
    send(m, "cat /pub/kutta.md; echo never\r");
    send(m, "\x03");
    o = drain(m);
    CHECK(strstr(o, "\r\nnever\r\n") == NULL);
    send(m, "echo still here\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nstill here\r\n") != NULL);
    /* a screen and the REPL: the rest waits until they are done */
    send(m, "edit ~/new.txt; echo edited\r");
    o = drain(m);
    CHECK(m->mode == ROC_MODE_APP && strstr(o, "\r\nedited\r\n") == NULL);
    send(m, "\x11"); /* ^Q, nothing to save */
    o = drain(m);
    CHECK(m->mode == ROC_MODE_LINE && strstr(o, "edited\r\n") != NULL);
    send(m, "filo; echo left\r");
    o = drain(m);
    CHECK(strstr(o, "filo> ") != NULL && strstr(o, "\r\nleft\r\n") == NULL);
    send(m, "(+ 1 2)\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n3\r\n") != NULL && strstr(o, "\r\nleft\r\n") == NULL);
    send(m, "\x04"); /* ^D: out of the REPL */
    o = drain(m);
    CHECK(strstr(o, "\r\nleft\r\n") != NULL);
}

/* a | b: what a writes, b reads — cat and less with no file, the
   filters, a program as STDIN; a pipeline runs or is skipped whole, and
   its $? is its last command's. */
static const char *arith_vars(void *user, const char *name, size_t len) {
    (void)user;
    if (len == 1 && name[0] == 'n') {
        return "7";
    }
    if (len == 1 && name[0] == 'h') {
        return "0x10";
    }
    if (len == 1 && name[0] == 's') {
        return "abc";
    }
    if (len == 1 && name[0] == '?') {
        return "1";
    }
    return NULL;
}

/* What $((expr)) comes to, or the error, as the one word it makes. */
static const char *arith(const char *expr) {
    static sh_line sl;
    static char out[128];
    char line[96];
    (void)snprintf(line, sizeof(line), "echo $((%s))", expr);
    if (!sh_read(line, &sl, arith_vars, NULL, out, sizeof(out))) {
        return out;
    }
    return sl.argc == 2 ? sl.argv[1] : "?";
}

static void test_shell_arith(void) {
    CHECK(strcmp(arith("1 + 2 * 3"), "7") == 0);
    CHECK(strcmp(arith("(1 + 2) * 3"), "9") == 0);
    CHECK(strcmp(arith("n * 2 - $n"), "7") == 0);
    CHECK(strcmp(arith("${n} % 4 + h"), "19") == 0);
    CHECK(strcmp(arith("unset + 1"), "1") == 0);
    CHECK(strcmp(arith("-n / 2"), "-3") == 0); /* toward zero, as C */
    CHECK(strcmp(arith("1 < 2 && 2 <= 2 || 0"), "1") == 0);
    CHECK(strcmp(arith("!0 + ~0 + (5 == 5) + (5 != 5)"), "1") == 0);
    CHECK(strcmp(arith("1 << 4 | 3 & 1 ^ 2"), "19") == 0);
    CHECK(strcmp(arith("n > 5 ? 10 : 20"), "10") == 0);
    CHECK(strcmp(arith("0 ? 1 : 0 ? 2 : 3"), "3") == 0);
    CHECK(strcmp(arith("010 + 0x1f"), "39") == 0);
    CHECK(strcmp(arith("$? + $((n + 1))"), "9") == 0);
    CHECK(strcmp(arith("9223372036854775807 + 1"), "-9223372036854775808") == 0);
    CHECK(strcmp(arith("-9223372036854775807 - 1"), "-9223372036854775808") == 0);
    CHECK(strcmp(arith("1 / 0"), "arithmetic expression: division by zero") == 0);
    CHECK(strcmp(arith("1 +"), "arithmetic expression: expecting primary") == 0);
    CHECK(strcmp(arith("(1 2)"), "arithmetic expression: expecting ')'") == 0);
    CHECK(strcmp(arith("1 2"), "arithmetic expression: expecting EOF") == 0);
    CHECK(strcmp(arith("s"), "Illegal number") == 0);
    CHECK(strcmp(arith("09"), "arithmetic expression: bad number") == 0);
    char why[96];
    CHECK(sh_check("echo $((10 / (x - 1)))", why, sizeof(why))); /* x is 1 only while checking */
    CHECK(!sh_check("echo $((1 +))", why, sizeof(why)));

    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "i=0; while [ $i -lt 4 ]; do echo \"i=$i\"; i=$((i + 1)); done; echo $((i*i))\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\ni=0\r\ni=1\r\ni=2\r\ni=3\r\n16\r\n") != NULL);
}

static void test_shell_case(void) {
    char why[96];
    CHECK(sh_check("case $x in a|b) echo ab;; (c*) ;; *) echo other; esac", why, sizeof(why)));
    CHECK(sh_check("case x in esac; case x\nin\n  if) echo;;\nesac", why, sizeof(why)));
    CHECK(!sh_check("case x in a) echo", why, sizeof(why)));
    CHECK(sh_needs_more(why) && strstr(why, "expecting \"esac\"") != NULL);
    CHECK(!sh_check("case x a) echo;; esac", why, sizeof(why)));
    CHECK(strstr(why, "expecting \"in\"") != NULL);
    CHECK(!sh_check("case x in a echo;; esac", why, sizeof(why)));
    CHECK(!sh_check("echo a;; echo b", why, sizeof(why)));
    CHECK(!sh_check("case x in a) echo && ;; esac", why, sizeof(why)));
    CHECK(sh_match("a*c", "abbc") && sh_match("[!x]?", "yz") && !sh_match("a\\*", "ab"));

    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "for w in apple Bob 'a b' x.txt '*' zz; do case $w in\r");
    drain(m);
    send(m, "  a*) echo \"$w: a\";; [A-Z]*) echo $w: upper ;;\r");
    drain(m);
    send(m, "  *.txt | *.md) echo $w: text;; '*') echo star;;\r");
    drain(m);
    send(m, "  *) echo $w: other\r");
    drain(m);
    send(m, "esac; done\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\napple: a\r\nBob: upper\r\na b: a\r\nx.txt: text\r\nstar\r\nzz: "
                    "other\r\n") != NULL);
    send(m, "false; case q in a) echo no;; esac; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n0\r\n") != NULL && strstr(o, "\r\nno\r\n") == NULL);
    send(m, "p='?'; case ab in $p) echo one;; \"$p$p\") echo quoted;; $p$p) echo two;; esac\r");
    o = drain(m);
    CHECK(strstr(o, "\r\ntwo\r\n") != NULL && strstr(o, "\r\nquoted\r\n") == NULL);
    send(m, "for i in 1 2 3; do case $i in 2) continue;; 3) break;; esac; echo i$i; done\r");
    o = drain(m);
    CHECK(strstr(o, "\r\ni1\r\n") != NULL && strstr(o, "\r\ni2\r\n") == NULL &&
          strstr(o, "\r\ni3\r\n") == NULL);
    send(m, "for i in 1; do case $i in 1) break;; if) echo;; esac; echo no; done; echo out\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nout\r\n") != NULL && strstr(o, "\r\nno\r\n") == NULL);
    send(m, "case x in x) case y in y) echo inner;; esac;; *) echo never;; esac | wc\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n1 1 6\r\n") != NULL);
    send(m, "case $(echo hi) in h?) if true; then echo yes; fi;; esac\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nyes\r\n") != NULL);
}

static const char *param_vars(void *user, const char *name, size_t len) {
    (void)user;
    if (len == 1 && name[0] == '#') {
        return *(const bool *)user ? "3" : "0";
    }
    if (len == 1 && name[0] == '@') {
        return "a b\x1f\x1f c";
    }
    if (len == 1 && name[0] == '1') {
        return "a b";
    }
    return NULL;
}

static void test_shell_functions(void) {
    static sh_line sl;
    char why[96];
    bool some = true;
    const sh_env env = {param_vars, NULL, NULL, &some, NULL};
    CHECK(sh_expand("echo \"$@\"", &sl, &env, why, sizeof(why)) && sl.argc == 4);
    CHECK(strcmp(sl.argv[1], "a b") == 0 && sl.argv[2][0] == '\0' && strcmp(sl.argv[3], " c") == 0);
    CHECK(sh_expand("echo x\"$@\"y", &sl, &env, why, sizeof(why)) && sl.argc == 4);
    CHECK(strcmp(sl.argv[1], "xa b") == 0 && strcmp(sl.argv[3], " cy") == 0);
    CHECK(sh_expand("echo $@", &sl, &env, why, sizeof(why)) && sl.argc == 4); /* a b c */
    CHECK(sh_expand("echo ${1}x $#", &sl, &env, why, sizeof(why)) && sl.argc == 4);
    some = false;
    CHECK(sh_expand("echo \"$@\" x", &sl, &env, why, sizeof(why)) && sl.argc == 2);
    CHECK(sh_check("f() { echo; }; g ( ) {\n echo\n}\nh() if true; then :; fi", why, sizeof(why)));
    CHECK(!sh_check("f() echo", why, sizeof(why)));
    CHECK(!sh_check("f()", why, sizeof(why)) && sh_needs_more(why));
    CHECK(!sh_check("{ }", why, sizeof(why)));
    CHECK(!sh_check("{ echo; ", why, sizeof(why)) && strstr(why, "expecting \"}\"") != NULL);
    CHECK(!sh_check("{ echo; done", why, sizeof(why)));

    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "greet() {\r");
    drain(m);
    send(m, "  echo \"hi $1, $# of them: $*\"\r");
    drain(m);
    send(m, "}\r");
    drain(m);
    send(m, "greet 'you all' two; echo $1-$#\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\nhi you all, 2 of them: you all two\r\n-0\r\n") != NULL);
    send(m, "count() { n=0; for a in \"$@\"; do n=$((n+1)); done; echo $n; }; count; count '' b 'c "
            "d'\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n0\r\n3\r\n") != NULL);
    send(m, "fact() { if [ $1 -le 1 ]; then echo 1; return; fi; echo $(($1 * $(fact $(($1 - "
            "1))))); }\r");
    drain(m);
    send(m, "fact 5; fact 6\r"); /* each call a $(...) deeper: 4 deep at most */
    o = drain(m);
    CHECK(strstr(o, "\r\n120\r\n") != NULL && strstr(o, "nested too deep") != NULL);
    send(m, "odd() { return $(($1 % 2)); }; odd 3 && echo even || echo odd; odd 4 && echo even\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nodd\r\neven\r\n") != NULL);
    send(m, "first() { for a in 1 2 3; do if [ $a = 2 ]; then return 7; fi; echo a$a; done; echo "
            "never; }; first; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\na1\r\n7\r\n") != NULL && strstr(o, "never\r\n") == NULL);
    send(m, "args() { shift; echo $1 $#; shift 5; }; args a b c\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nb 2\r\n") != NULL && strstr(o, "can't shift") != NULL);
    send(m,
         "lines() { echo b; echo a; }; lines | sort; lines > ~/l.txt; { echo c; lines; } | wc\r");
    o = drain(m);
    CHECK(strstr(o, "\r\na\r\nb\r\n3 3 6\r\n") != NULL);
    const uint8_t *d = NULL;
    size_t n = 0;
    CHECK(ufs_find(&m->uf, "/home/guest/l.txt", &d, &n) && n == 4 && memcmp(d, "b\na\n", 4) == 0);
    send(m, "up() { echo $(echo $1 | wc); }; up x\r"); /* $1 inside $(...) is the function's */
    o = drain(m);
    CHECK(strstr(o, "\r\n1 1 2\r\n") != NULL);
    send(m, "loop() { break; }; for i in 1 2; do loop; echo i$i; done\r");
    o = drain(m);
    CHECK(strstr(o, "only in a loop") != NULL && strstr(o, "\r\ni2\r\n") != NULL);
    send(m, "deep() { deep; }; deep; echo after\r");
    o = drain(m);
    CHECK(strstr(o, "Too deeply nested") != NULL);
    send(m, "exit() { :; }; return\r");
    o = drain(m);
    CHECK(strstr(o, "special builtin") != NULL && strstr(o, "only in a function") != NULL);
    send(m, "ls() { echo mine; }; ls; unset -f ls; ls /nothere\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nmine\r\n") != NULL && strstr(o, "No such file") != NULL);
    h.pending = 0;
    send(m, "show() { cat /pub/kutta.md; echo shown $1; }; show it | wc; echo end\r");
    o = drain(m);
    CHECK(h.pending == 1 && strstr(o, "\r\nend\r\n") == NULL);
    roc_feed(m, h.req_id, (const uint8_t *)"hi\n", 3);
    roc_feed_eof(m, h.req_id);
    o = drain(m);
    CHECK(strncmp(o, "2 3 12\r\nend\r\n", 13) == 0);
}

static void test_shell_read(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    CHECK(roc_write_file(m, "test", "/home/guest/people.txt",
                         (const uint8_t *)"ana 31 Rio\n  bo  7   S\\ Paulo  \nlast", 34));
    send(m, "while read name age city; do echo \"[$name|$age|$city]\"; done < ~/people.txt\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\n[ana|31|Rio]\r\n[bo|7|S Paulo]\r\n") != NULL &&
          strstr(o, "last]") == NULL);
    send(m, "read -r a b < ~/people.txt; echo \"$a/$b\"; ls /pub | while read f; do n=$((n+1)); "
            "done; echo $n\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nana/31 Rio\r\n") != NULL && strstr(o, "\r\n2\r\n") != NULL);
    send(m,
         "echo b a | { read x y; echo $y $x; }; printf() { while read l; do echo \"<$l>\"; done; "
         "}; ls /pub | printf | head -n 1\r");
    o = drain(m);
    CHECK(strstr(o, "\r\na b\r\n") != NULL && strstr(o, "\r\n<caf\xc3\xa9.md>\r\n") != NULL);
    /* at the prompt: the line waits for one typed */
    send(m, "echo 'your name?'; read who rest; echo \"hello $who ($rest)\"\r");
    o = drain(m);
    CHECK(strstr(o, "your name?\r\n") != NULL && strstr(o, "\r\nhello") == NULL &&
          strstr(o, "$ ") == NULL);
    send(m, "Ada Lovelace King\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nhello Ada (Lovelace King)\r\n") != NULL && strstr(o, "$ ") != NULL);
    send(m, "read x || echo eof $?\r");
    drain(m);
    send(m, "\x04");
    o = drain(m);
    CHECK(strstr(o, "\r\neof 1\r\n") != NULL);
    send(m, "read x; echo never\r");
    drain(m);
    send(m, "\x03");
    o = drain(m);
    CHECK(strstr(o, "never") == NULL || strstr(o, "\r\nnever\r\n") == NULL);
    send(m, "echo $?; history | grep -c Ada\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n130\r\n") != NULL);
    send(m, "read 1x\r");
    o = drain(m);
    CHECK(strstr(o, "bad variable name") != NULL);
}

static void test_shell_printf(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "printf '%s=%d|%5s|%-4s|%.2s|%03d|%+d|%x|%#o|%c|%%\\n' a 42 r l abc 7 5 255 8 xyz\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\na=42|    r|l   |ab|007|+5|ff|010|x|%\r\n") != NULL);
    send(m, "printf '[%s]\\n' one two three; printf '%d %d\\n' 1\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n[one]\r\n[two]\r\n[three]\r\n1 0\r\n") != NULL);
    send(m, "printf '%*d|%.*s|%b|\\101\\n' 4 9 2 abcdef 'x\\ty'\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n   9|ab|x\ty|A\r\n") != NULL);
    send(m, "printf '%d\\n' 0x10 010 -3 \"'A\"; printf '%d' zz; echo \" $?\"\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n16\r\n8\r\n-3\r\n65\r\n") != NULL && strstr(o, "Illegal number") != NULL &&
          strstr(o, "0 1\r\n") != NULL);
    send(m, "printf '%b and more' 'stop\\chere'; echo; echo -n no newline; echo .\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nstop\r\nno newline.\r\n") != NULL);
    send(m, "printf '%q'\r");
    o = drain(m);
    CHECK(strstr(o, "bad conversion") != NULL);
}

static void test_shell_scripts(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    const char *hello = "#!/bin/sh\n"
                        "# greets each\n"
                        "who=nobody\n"
                        "for n in \"$@\"; do\n"
                        "  echo \"$0 greets $n\"\n"
                        "done\n"
                        "cd /pub\n"
                        "[ $# -gt 2 ] && exit 3\n"
                        "echo end\n";
    CHECK(roc_write_file(m, "test", "/home/guest/hello.sh", (const uint8_t *)hello, strlen(hello)));
    send(m, "who=me; cd ~; sh hello.sh ana 'bo c'; echo \"$? $who $PWD\"\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\nhello.sh greets ana\r\nhello.sh greets bo c\r\nend\r\n0 me "
                    "/home/guest\r\n") != NULL);
    send(m, "./hello.sh a b c | wc; ./hello.sh a b c > /dev/null; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n3 9 60\r\n3\r\n") != NULL);       /* exit 3 leaves the script only */
    send(m, ". ./hello.sh x; echo \"$who $PWD\"; cd ~\r"); /* . runs here: its changes stay */
    o = drain(m);
    CHECK(strstr(o, "\r\nnobody /pub\r\n") != NULL);
    send(m, "sh -c 'echo $0: $1 $#' me one; sh -c 'exit 5'; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nme: one 1\r\n5\r\n") != NULL);
    CHECK(roc_write_file(m, "test", "/home/guest/bad.sh", (const uint8_t *)"if true\necho\n", 13));
    send(m, "sh bad.sh; sh nothere.sh; echo $?; sh /pub/kutta.md\r");
    o = drain(m);
    CHECK(strstr(o, "bad.sh: Syntax error: end of file unexpected (expecting \"then\")") != NULL &&
          strstr(o, "\r\n127\r\n") != NULL && strstr(o, "cp it home first") != NULL);
    CHECK(roc_write_file(m, "test", "/home/guest/lib.sh",
                         (const uint8_t *)"twice() { echo $1$1; }\n", 23));
    send(m, ". ~/lib.sh; twice ab; echo $0\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nabab\r\nrocchetto\r\n") != NULL);
    /* a script runs by its first line, not by its name: ./lib.sh has no #! */
    send(m, "./lib.sh; echo st=$?\r");
    o = drain(m);
    CHECK(strstr(o, "cannot execute") != NULL && strstr(o, "st=126") != NULL);
    const char *env = "#!/usr/bin/env sh\necho via env $1\n";
    CHECK(roc_write_file(m, "test", "/home/guest/e2", (const uint8_t *)env, strlen(env)));
    send(m, "~/e2 one\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nvia env one\r\n") != NULL);
    /* in PATH: the user's go over the shell's table (cat), never over the
       shell's own (cd), nor over a function */
    send(m, "mkdir ~/bin\r");
    drain(m);
    const char *cat = "#!/bin/sh\necho my cat $#\n";
    const char *cd = "#!/bin/sh\necho my cd\n";
    CHECK(roc_write_file(m, "test", "/home/guest/bin/cat", (const uint8_t *)cat, strlen(cat)));
    CHECK(roc_write_file(m, "test", "/home/guest/bin/cd", (const uint8_t *)cd, strlen(cd)));
    CHECK(
        roc_write_file(m, "test", "/home/guest/bin/greet", (const uint8_t *)hello, strlen(hello)));
    send(m, "PATH=~/bin:/bin; cat a b; type cat; cd /; pwd; greet x; type cd\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nmy cat 2\r\n") != NULL &&
          strstr(o, "cat is /home/guest/bin/cat") != NULL && strstr(o, "my cd") == NULL &&
          strstr(o, "\r\n/\r\n") != NULL && strstr(o, "\r\ngreet greets x\r\n") != NULL &&
          strstr(o, "cd is a shell builtin") != NULL);
    send(m, "greet() { echo fn; }; greet; unset -f greet; PATH=/bin; cat ~/lib.sh\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nfn\r\n") != NULL && strstr(o, "twice()") != NULL);
    /* what POSIX has and this shell cannot: said, and a failure for $? */
    send(m, "chmod 644 lib.sh; echo st=$?; ps || echo no ps\r");
    o = drain(m);
    CHECK(strstr(o, "chmod: not supported by rocchetto on ") != NULL && strstr(o, "st=1") != NULL &&
          strstr(o, "ps: not supported by rocchetto on ") != NULL && strstr(o, "no ps") != NULL);
}

static void test_shell_braces(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "f=/home/guest/notes.old.txt; e=; echo ${f##*/} ${f%.*} ${f%%.*} ${f#*/} ${#f}\r");
    const char *o = drain(m);
    CHECK(
        strstr(o,
               "\r\nnotes.old.txt /home/guest/notes.old /home/guest/notes home/guest/notes.old.txt "
               "25\r\n") != NULL);
    send(m, "echo [${u-unset}] [${e-set}] [${e:-empty}] [${f:+has}] [${u+no}] [${e+set}]\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n[unset] [] [empty] [has] [] [set]\r\n") != NULL);
    send(m, "echo \"${u:-a  b}\" ${u:-$f} ${u:-'q*'} ${#} ${u:=kept}; echo $u\r");
    o = drain(m);
    CHECK(strstr(o, "\r\na  b /home/guest/notes.old.txt q* 0 kept\r\nkept\r\n") != NULL);
    send(m, "echo ${e:?nothing here}; echo never\r");
    o = drain(m);
    CHECK(strstr(o, "e: nothing here") != NULL && strstr(o, "\r\nnever\r\n") == NULL);
    send(m, "x='a*b'; echo ${x%\"*b\"} \"${x%*b}\" \"${x#$(echo a)}\" ${#x} ${HOME:=no}\r");
    o = drain(m);
    CHECK(strstr(o, "\r\na a* *b 3 /home/guest\r\n") != NULL);
    send(m, "s=caf\xc3\xa9; echo ${#s}\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n4\r\n") != NULL);
    send(m, "echo ok; echo ${f:x}\r"); /* read first: none of the line runs */
    o = drain(m);
    CHECK(strstr(o, "Bad substitution") != NULL && strstr(o, "\r\nok\r\n") == NULL);
    send(m, "PWD=x; echo ${PWD:=y}\r");
    o = drain(m);
    CHECK(strstr(o, "readonly") != NULL);
}

static void test_shell_block_errors(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "for f in /x /y; do ls $f; done 2> ~/e.txt; echo after\r");
    const char *o = drain(m);
    CHECK(strstr(o, "No such file") == NULL && strstr(o, "\r\nafter\r\n") != NULL);
    const uint8_t *d = NULL;
    size_t n = 0;
    CHECK(ufs_find(&m->uf, "/home/guest/e.txt", &d, &n) && n > 0 && memmem(d, n, "/x", 2) != NULL &&
          memmem(d, n, "/y", 2) != NULL);
    send(m, "{ ls /z; echo out; } 2>&1 | wc -l; { ls /z; } 2> /dev/null; echo quiet\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n2\r\n") != NULL && strstr(o, "\r\nquiet\r\n") != NULL &&
          strstr(o, "No such file") == NULL);
    send(m, "oops() { ls /nope; }; oops 2>> ~/e.txt; oops 2>> ~/e.txt\r");
    drain(m);
    CHECK(ufs_find(&m->uf, "/home/guest/e.txt", &d, &n) && memmem(d, n, "/nope", 5) != NULL);
    const char *first = memmem(d, n, "/nope", 5);
    CHECK(first != NULL &&
          memmem(first + 5, n - (size_t)(first + 5 - (const char *)d), "/nope", 5) != NULL);
    send(m, "if true; then ls /q 2>&1; fi 2> /dev/null | wc -l; ls /inner 2> /dev/null; echo ok\r");
    o = drain(m); /* a command's own 2> wins over its block's */
    CHECK(strstr(o, "\r\n1\r\n") != NULL && strstr(o, "\r\nok\r\n") != NULL);
    CHECK(roc_write_file(m, "test", "/home/guest/e.sh", (const uint8_t *)"ls /s\n", 6));
    send(m, "printf 'caf\xc3\xa9 a\\nb\\n' | wc -l; printf 'caf\xc3\xa9 a\\n' | wc -wm; printf 'x' "
            "| wc -c\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n2\r\n2 7\r\n1\r\n") != NULL);
    send(m, "for d in /nope /pub; do ls $d | wc -l; done 2>&1 | head -n 3\r");
    o = drain(m); /* the error where the loop writes, not into wc */
    CHECK(strstr(o, "No such file") != NULL && strstr(o, "\r\n0\r\n2\r\n") != NULL);
    send(m, "sh ~/e.sh 2>&1 | wc -l\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n1\r\n") != NULL);
}

static void test_shell_backquotes(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(
        m,
        "echo `echo a b` \"`echo 'c  d'`\" x`echo \\`echo in\\``y; n=`ls /pub | wc -l`; echo $n\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\na b c  d xiny\r\n2\r\n") != NULL);
    send(m, "echo `echo unclosed\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n> ") != NULL);
    send(m, "`\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nunclosed\r\n") != NULL);
}

static void test_shell_eval(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "i=3; echo $((i += 4)) $((j = i * 2)) $((i <<= 1)) $i $j; echo $((k == 0))\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\n7 14 14 14 14\r\n1\r\n") != NULL);
    send(m,
         "c='echo \"$1-$#\"'; f() { eval \"$c\"; eval return 4; echo never; }; f a b; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\na-2\r\n4\r\n") != NULL && strstr(o, "\r\nnever\r\n") == NULL);
    send(
        m,
        "for i in 1 2 3; do eval 'if [ $i = 2 ]; then break; fi'; echo i$i; done; eval; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\ni1\r\n0\r\n") != NULL && strstr(o, "\r\ni2\r\n") == NULL);
    send(m, "v=x; eval \"$v=5\"; eval \"w=\\$$v\"; echo $x $w; eval 'echo a;' 'echo b'\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n5 5\r\na\r\nb\r\n") != NULL);
    send(m, "readonly r=1; r=2; unset r; echo $r; export e=9 f; echo $e; readonly 9x\r");
    o = drain(m);
    CHECK(strstr(o, "r: readonly variable") != NULL && strstr(o, "\r\n1\r\n9\r\n") != NULL &&
          strstr(o, "bad variable name") != NULL);
    send(m, "echo $((r += 1))\r");
    o = drain(m);
    CHECK(strstr(o, "readonly") != NULL);
}

/* ( list ) and $(...) are subshells: what they set, cd to, define or trap
   stays in them, exit leaves only them; >&2 goes where the errors go. */
static void test_shell_subshells(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "x=1; (x=2; echo in $x); echo out $x; (cd /pub; pwd); pwd\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\nin 2\r\nout 1\r\n/pub\r\n/\r\n") != NULL);
    send(m, "x=$(exit 3); echo $?; if x=$(false); then echo yes; else echo no $?; fi\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n3\r\nno 1\r\n") != NULL);
    send(m, "x=1; y=$(x=9; echo $x); echo $y $x; ( exit 4 ); echo $?; ! (false) && echo negated\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n9 1\r\n4\r\nnegated\r\n") != NULL);
    send(m, "sh -c 'g() { echo inner; }'; g\r");
    o = drain(m);
    CHECK(strstr(o, "g: command not found") != NULL);
    send(m, "x=1; f() { local x=5; y=$(g2); echo x=$x; }; g2() { echo hi; }; f; echo x=$x\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nx=5\r\nx=1\r\n") != NULL);
    send(m, "(echo b; echo a) | sort; f3() { (set -- a b c; echo $#); echo $#; }; f3 1\r");
    o = drain(m);
    CHECK(strstr(o, "\r\na\r\nb\r\n3\r\n1\r\n") != NULL);
    send(m, "f4() ( echo sub $1 ); f4 z; ( (echo nested) ); (trap 'echo bye' EXIT; echo "
            "body); echo after\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nsub z\r\nnested\r\nbody\r\nbye\r\nafter\r\n") != NULL);
    send(m, "echo a)\r");
    o = drain(m);
    CHECK(strstr(o, "Syntax error: \")\" unexpected") != NULL);
    send(m, "(cd /pub; x=7; echo $((1 / 0)))\r"); /* the line stops inside it */
    o = drain(m);
    CHECK(strstr(o, "division by zero") != NULL && strstr(o, "/pub\x1b[0m$") == NULL);
    send(m, "pwd; echo $x\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n/\r\n1\r\n") != NULL);

    send(m, "x=$(echo toerr >&2; echo out); echo \"[$x]\"\r");
    o = drain(m);
    CHECK(strstr(o, "\r\ntoerr\r\n[out]\r\n") != NULL);
    send(m, "f() { echo ferr >&2; echo fout; }; f 2>/dev/null; { echo b1; } >&2 2>/dev/null\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nfout\r\n") != NULL && strstr(o, "\nferr") == NULL &&
          strstr(o, "\nb1") == NULL);
    send(m, "echo e1 >&2 2> ~/e.txt; cat ~/e.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\ne1\r\n") != NULL);
    send(m, "echo x >&3\r");
    o = drain(m);
    CHECK(strstr(o, "Bad fd number") != NULL);

    /* set -- where a for or a case of the function keeps words after its own */
    send(m, "f() { for i in 1 2; do set -- x y z; echo $# $i $3; done; case a in a) set -- q; "
            "echo $# $1;; esac; for j in u v; do set -- \"$@\" $j; done; echo \"$@\"; }; f a b\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n3 1 z\r\n3 2 z\r\n1 q\r\nq u v\r\n") != NULL);
}

/* IFS where fields split and read reads; NAME=v before a builtin for it
   only; for name; do; cmd & run at once; alias as the line is read. */
static void test_shell_ifs_alias(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "IFS=:; x='a::b'; set -- $x; echo $# \"[$2]\"; x=':b'; set -- $x; echo $#; "
            "set -- p q; echo \"$*\"; unset IFS; echo \"$*\"\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\n3 []\r\n2\r\np:q\r\np q\r\n") != NULL);
    send(m, "echo 'k=v w' | { IFS='=' read -r k v; echo \"$k/$v\"; }; echo \"[${IFS-unset}]\"\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nk/v w\r\n[unset]\r\n") != NULL);
    send(m, "printf '  lead  \\n' | while IFS= read -r l; do echo \"[$l]\"; done; echo ' x  y  z ' "
            "| { read a b; echo \"[$a][$b]\"; }\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n[  lead  ]\r\n[x][y  z]\r\n") != NULL);
    send(m, "set -- 1 2 3; for a; do echo -n $a; done; echo; X=1 echo hi; echo \"[$X]\"\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n123\r\nhi\r\n[]\r\n") != NULL);

    send(m, "true & echo bg $?; false & p=$!; wait $p; echo st=$?; wait; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nbg 0\r\nst=1\r\n0\r\n") != NULL);
    send(m, "echo a && & b\r");
    o = drain(m);
    CHECK(strstr(o, "Syntax error: \"&\" unexpected") != NULL);

    send(m, "alias hi='echo hello' e='echo ' w=world\r");
    drain(m);
    send(m, "hi there; if hi; then e w; fi; echo w; alias hi; type hi; command -v e\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nhello there\r\nhello\r\nworld\r\nw\r\nhi='echo hello'\r\n"
                    "hi is an alias for echo hello\r\nalias e='echo '\r\n") != NULL);
    send(m, "alias r1=r2 r2=r1; unalias hi; alias nope\r");
    o = drain(m);
    CHECK(strstr(o, "nope: not found") != NULL);
    send(m, "r1; hi\r");
    o = drain(m);
    CHECK(strstr(o, "r1: command not found") != NULL && strstr(o, "hi: command not found") != NULL);
    send(m, "cd -; cd /pub; cd /; cd -; echo $OLDPWD; (cd /jnl); pwd\r");
    o = drain(m);
    CHECK(strstr(o, "OLDPWD not set") != NULL && strstr(o, "\r\n/pub\r\n/\r\n/pub\r\n") != NULL);
    send(m, "expr 1 + 2 \\* 3; expr \\( 1 + 2 \\) \\* 3; expr file.txt : '\\(.*\\)\\.txt'; "
            "expr abc : b; echo $?; expr '' \\| ''; expr a + 1; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n7\r\n9\r\nfile\r\n0\r\n1\r\n0\r\n") != NULL &&
          strstr(o, "non-integer argument\r\n2\r\n") != NULL);
    send(m, "alias ll='ls -l'\r");
    drain(m);
    send(m, "ll -a /pub | wc -l; ls -la /pub | wc -l; ls -j\r"); /* total, . .., two */
    o = drain(m);
    CHECK(strstr(o, "\r\n5\r\n5\r\n") != NULL && strstr(o, "ls: bad option -j") != NULL);
    send(m, "alias hi='echo hello' up='tr a-z A-Z'\r");
    drain(m);
    send(m, "x=$(hi there | up); echo \"[$x]\"; echo \"$(hi; echo \"$(hi in)\")\"; "
            "y=$(case a in a) hi c;; (b) echo b;; esac); echo $y\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n[HELLO THERE]\r\nhello") != NULL &&
          strstr(o, "hello in\r\nhello c\r\n") != NULL);
    send(m, "for i in 1 2; do (break; echo no); echo i$i; done; g() { for a do if true; then "
            "echo -n \"<$a>\"; fi; done; echo; }; g x y\r");
    o = drain(m);
    CHECK(strstr(o, "\r\ni1\r\ni2\r\n<x><y>\r\n") != NULL && strstr(o, "\nno") == NULL);
    send(m, "y=$(case a in a) echo hit;; ho bc;; esac)\r"); /* hung the alias pass once */
    o = drain(m);
    CHECK(strstr(o, "word unexpected (expecting \")\")") != NULL);
    send(m, "unalias -a; alias\r");
    o = drain(m);
    CHECK(strstr(o, "r2") == NULL || strstr(o, "r2=") == NULL);
}

static void test_shell_heredocs(void) {
    static char out[SH_SCRIPT_MAX + 1];
    char why[96];
    CHECK(sh_heredocs("echo $((1 << 2)) '<<' x", out, sizeof(out), why, sizeof(why)) &&
          strcmp(out, "echo $((1 << 2)) '<<' x") == 0);
    CHECK(!sh_heredocs("cat <<EOF\nline\n", out, sizeof(out), why, sizeof(why)));
    CHECK(sh_needs_more(why) && strstr(why, "\"EOF\"") != NULL);

    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "who=you; cat <<EOF | wc -l; echo after\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\n> ") != NULL && strstr(o, "\r\nafter\r\n") == NULL);
    send(m, "  hi $who, $((1 + 1)) \\$HOME `echo x`\r");
    drain(m);
    send(m, "\r");
    drain(m);
    send(m, "EOF\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n2\r\nafter\r\n") != NULL);
    send(m, "cat <<EOF\r");
    drain(m);
    send(m, "  hi $who, $((1 + 1)) \\$HOME `echo x` \"q\" 'r'\r");
    drain(m);
    send(m, "EOF\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n  hi you, 2 $HOME x \"q\" 'r'\r\n") != NULL);
    const char *script = "cat <<'END' ; cat <<-TABS\n"
                         "raw $who `x`\n"
                         "END\n"
                         "\tthis $who\n"
                         "\tTABS\n"
                         "while read a b; do echo \"[$b|$a]\"; done <<L\n"
                         "1 one\n"
                         "2 two\n"
                         "L\n"
                         "echo done\n";
    CHECK(roc_write_file(m, "test", "/home/guest/h.sh", (const uint8_t *)script, strlen(script)));
    send(m, "who=me; . ~/h.sh\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nraw $who `x`\r\nthis me\r\n[one|1]\r\n[two|2]\r\ndone\r\n") != NULL);
    send(m, "f() { cat <<X\r");
    drain(m);
    send(m, "in f: $1\r");
    drain(m);
    send(m, "X\r");
    drain(m);
    send(m, "}; f a; f b | wc -c\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nin f: a\r\n8\r\n") != NULL);
    send(m, "cat <<E\r");
    drain(m);
    send(m, "\x03");
    o = drain(m);
    CHECK(strstr(o, "^C") != NULL && strstr(o, "$ ") != NULL);
}

static void test_shell_options(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "a=1 b=$a c=$((b + 1)); echo $a $b $c\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\n1 1 2\r\n") != NULL);
    send(
        m,
        "! false; echo $?; ! true | wc -l > /dev/null; echo $?; ! if false; then :; fi; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n0\r\n1\r\n1\r\n") != NULL);
    send(m, "set -- x 'y z'; echo $# $2; f() { set -- in; echo $#:$1; }; f a b; echo $1\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n2 y z\r\n1:in\r\nx\r\n") != NULL);
    send(m, "set -eu; echo [$-]; echo $nope; echo never\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n[eu]\r\n") != NULL && strstr(o, "nope: parameter not set") != NULL &&
          strstr(o, "\r\nnever\r\n") == NULL);
    send(m, "echo ${nope-ok}; if false; then :; fi; false || echo or; ! false; false; echo stop\r");
    o = drain(m); /* the unlooked-at false ends the line */
    CHECK(strstr(o, "\r\nok\r\nor\r\n") != NULL && strstr(o, "\r\nstop\r\n") == NULL);
    CHECK(roc_write_file(m, "test", "/home/guest/e.sh",
                         (const uint8_t *)"echo one\nls /nothere\necho two\n", 30));
    send(m, "sh ~/e.sh; echo back $?\r"); /* -e leaves the script, not the line */
    o = drain(m);
    CHECK(strstr(o, "\r\none\r\n") != NULL && strstr(o, "\r\ntwo\r\n") == NULL &&
          strstr(o, "\r\nback 1\r\n") != NULL);
    send(m, "set +eu -x; v=2 w=3; echo $v >/dev/null; set +x; echo quiet\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n+ v=2 w=3\r\n+ echo 2\r\n+ set +x\r\nquiet\r\n") != NULL);
    send(m, "! sh -c 'exit 3'; echo $?; ! eval true; echo $?; ! eval; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n0\r\n1\r\n1\r\n") != NULL);
    send(m, "set -q\r");
    o = drain(m);
    CHECK(strstr(o, "bad option") != NULL);
}

static void test_shell_local_getopts(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "x=out; f() { local x=in y; y=made; echo $x $y; g; }; g() { echo g:$x; }; f; echo $x "
            "[$y]\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\nin made\r\ng:in\r\nout []\r\n") != NULL);
    send(m, "local z\r");
    o = drain(m);
    CHECK(strstr(o, "only in a function") != NULL);
    send(
        m,
        "[ -n a -a -z '' ] && echo and; [ a = b -o \\( 1 -lt 2 \\) ] && echo or; [ ! -d /pub -o -f "
        "x ] || "
        "echo neither\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nand\r\nor\r\nneither\r\n") != NULL);
    CHECK(roc_write_file(m, "test", "/home/guest/t.sh", (const uint8_t *)"x\n", 2));
    send(m, "cd ~; [ -s t.sh -a -x t.sh -a -w t.sh ] && echo sxw; [ -w /pub ] || echo ro; [ a = ]; "
            "echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nsxw\r\nro\r\n") != NULL);
    send(m, "opts() { OPTIND=1; while getopts ab:c o; do echo \"$o[${OPTARG-}]\"; done; shift "
            "$((OPTIND - 1)); "
            "echo rest $*; }; opts -a -b 1 -cb2 -- -x y\r");
    o = drain(m);
    CHECK(strstr(o, "\r\na[]\r\nb[1]\r\nc[]\r\nb[2]\r\nrest -x y\r\n") != NULL);
    send(m, "opts -q; OPTIND=1; getopts :b: o -b; echo \"$o $OPTARG\"\r");
    o = drain(m);
    CHECK(strstr(o, "illegal option") != NULL && strstr(o, "\r\n: b\r\n") != NULL);
}

static void test_shell_textutils(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "basename /a/b/c.txt; basename /a/b/c.txt .txt; basename /a/b//; basename /; basename "
            "x .x\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\nc.txt\r\nc\r\nb\r\n/\r\nx\r\n") != NULL);
    send(m, "dirname /a/b/c; dirname /a; dirname a/b/; dirname a; dirname /; dirname a//b\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n/a/b\r\n/\r\na\r\n.\r\n/\r\na\r\n") != NULL);
    send(m, "printf 'one\\ntwo\\n' | tee ~/t1.txt | wc -l; echo 3 | tee -a ~/t1.txt; echo 4 | tee "
            "-a ~/new.txt\r");
    o = drain(m);
    const uint8_t *d = NULL;
    size_t n = 0;
    CHECK(ufs_find(&m->uf, "/home/guest/t1.txt", &d, &n) && n == 10 &&
          memcmp(d, "one\ntwo\n3\n", 10) == 0);
    CHECK(strstr(o, "\r\n2\r\n3\r\n4\r\n") != NULL);
    send(m, "echo 'Hello, World' | tr a-z A-Z; echo aabbccdd | tr -s abcd; echo 'a1b2' | tr -d "
            "'[:digit:]'\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nHELLO, WORLD\r\nabcd\r\nab\r\n") != NULL);
    send(m, "echo 'x y  z' | tr -cs '[:alpha:]' '_'; printf 'a\\tb\\n' | tr '\\t' ' '\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nx_y_z_a b\r\n") != NULL); /* the newline is no letter either */
    CHECK(roc_write_file(m, "test", "/home/guest/p.txt",
                         (const uint8_t *)"ana:31:rio\nbo:7:sp\nnone\n", 24));
    send(m, "cut -d: -f1,3 ~/p.txt; cut -d : -f 2- -s ~/p.txt; echo caf\xc3\xa9s | cut -c 3-4; "
            "echo abcdef | cut "
            "-b -2,5-\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nana:rio\r\nbo:sp\r\nnone\r\n31:rio\r\n7:sp\r\nf\xc3\xa9\r\nabef\r\n") !=
          NULL);
    send(m, "cut -f 0 ~/p.txt; echo x | tr\r");
    o = drain(m);
    CHECK(strstr(o, "cut: a list is") != NULL && strstr(o, "usage: tr") != NULL);
    send(m, "echo before; sleep 1.5; echo after\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nbefore\r\n") != NULL && strstr(o, "\r\nafter\r\n") == NULL &&
          strstr(o, "$ ") == NULL);
    roc_tick(m, 1000);
    send(m, "typed");
    o = drain(m);
    CHECK(strstr(o, "after") == NULL && strstr(o, "typed") == NULL);
    roc_tick(m, 600);
    o = drain(m);
    CHECK(strncmp(o, "after\r\n", 7) == 0 && strstr(o, "$ ") != NULL);
    send(m, "sleep 5; echo never\r");
    drain(m);
    send(m, "\x03");
    o = drain(m);
    CHECK(strstr(o, "^C") != NULL && strstr(o, "never\r\n") == NULL);
    send(m, "sleep x; x=$(sleep 1); echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "usage: sleep") != NULL && strstr(o, "cannot wait") != NULL);
}

/* 2026-09-27 14:05:09 UTC, a Sunday; the zone three hours west. */
static bool test_clock(void *ctx, int64_t *secs, int32_t *tz_minutes) {
    (void)ctx;
    *secs = 1790517909;
    *tz_minutes = -180;
    return true;
}

static void test_shell_trap_type_date(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    const char *script = "trap 'echo bye $?' EXIT\necho work\nexit 4\necho never\n";
    CHECK(roc_write_file(m, "test", "/home/guest/tr.sh", (const uint8_t *)script, strlen(script)));
    send(m, "sh ~/tr.sh; echo $?; trap\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\nwork\r\nbye 4\r\n4\r\n") != NULL && strstr(o, "never\r\n") == NULL);
    send(m, "trap 'echo stopped' INT; trap; sleep 3\r");
    o = drain(m);
    CHECK(strstr(o, "trap -- 'echo stopped' INT") != NULL);
    send(m, "\x03");
    o = drain(m);
    CHECK(strstr(o, "^C\r\nstopped\r\n") != NULL);
    send(m, "trap - INT; echo $$ [$!]; f() { echo mine; }; command -v f cd tree nope; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n1 []\r\nf\r\ncd\r\n/bin/tree\r\n") != NULL &&
          strstr(o, "\r\n1\r\n") != NULL);
    send(m, "type if f echo nope; ls() { echo fake; }; ls; command ls /pub | wc -l\r");
    o = drain(m);
    CHECK(strstr(o,
                 "if is a shell keyword\r\nf is a function\r\necho is a shell builtin\r\nnope: not "
                 "found\r\nfake\r\n2\r\n") != NULL);
    send(m, "date\r");
    o = drain(m);
    CHECK(strstr(o, "no clock") != NULL);
    m->host.clock = test_clock;
    send(m, "date; date -u '+%F %T %a %j %s %z'; date +%d/%m/%y%n%B\r");
    o = drain(m);
    CHECK(
        strstr(
            o,
            "\r\nSun Sep 27 11:05:09 local 2026\r\n2026-09-27 14:05:09 Sun 270 1790517909 +0000\r\n"
            "27/09/26\r\nSeptember\r\n") != NULL);
    util_case(m, "date '+%I %p %u %w %R %h %C %D %e %j %z'",
              "11 AM 7 0 11:05 Sep 20 09/27/26 27 270 -0300\n", 0, NULL);
    util_case(m, "date -x", "", 2, "usage: date");
    util_case(m, "date -u -r 86399 '+%F %T'", "1970-01-01 23:59:59\n", 0, NULL);
    util_case(m, "date -r 1e3", "", 2, "usage: date");
    util_case(m, "seq 3 | pr -l 12",
              "\n\nSep 27 11:05 2026  Page 1\n\n\n1\n2\n\n\n\n\n\n"
              "\n\nSep 27 11:05 2026  Page 2\n\n\n3\n\n\n\n\n\n\n",
              0, NULL);
    util_case(m, "seq 1 | pr -l 11 -h HDR", "\n\nSep 27 11:05 2026 HDR Page 1\n\n\n1\n\n\n\n\n\n",
              0, NULL);
    util_case(m, "seq 25 | pr -l 12 +13", "\n\nSep 27 11:05 2026  Page 13\n\n\n25\n\n\n\n\n\n\n", 0,
              NULL);
    util_case(m, "seq 3 | pr -t -n -d", "    1\t1\n\n    2\t2\n\n    3\t3\n\n", 0, NULL);
    util_case(m, "seq 7 | pr -t -3 -w 20", "1      4      7\n2      5\n3      6\n", 0, NULL);
    util_case(m, "printf 'abcdefghijklmnop\\nb\\n' | pr -t -2 -w 20", "abcdefghi b\n", 0, NULL);
    util_case(m, "printf 'a\\nb\\nc\\nd\\ne\\n' | pr -t -a -2 -w 10", "a    b\nc    d\ne\n", 0,
              NULL);
    util_case(m, "seq 2 | pr -t -o 3", "   1\n   2\n", 0, NULL);
    util_case(m, "pr -t nofile", "", 1, "pr: nofile:");
    util_case(m, "pr -3 -w 1", "", 2, "usage: pr");
    /* a line of 310 KB: wc counts it without a list of its characters */
    static const char wide[] =
        "(out-write (str-join \"\" (map (fn (i) \"0123456789\") (range 0 31000))))";
    CHECK(roc_write_file(m, "t", "/home/guest/wide.filo", (const uint8_t *)wide, sizeof(wide) - 1));
    util_case(m, "filo ~/wide.filo > ~/wide.out; wc ~/wide.out",
              "0 1 310000 /home/guest/wide.out\n", 0, NULL);
    /* a 2> larger than its capture goes to the file as it fills: nothing cut */
    static const char loud[] = "(map (fn (i) (err-write \"error number \" i \" of many, said on "
                               "stderr\\n\")) (range 0 400))";
    CHECK(roc_write_file(m, "t", "/home/guest/loud.filo", (const uint8_t *)loud, sizeof(loud) - 1));
    util_case(m, "filo ~/loud.filo 2> ~/e1; wc -l ~/e1", "400 /home/guest/e1\n", 0, NULL);
    util_case(m, "tail -n 1 ~/e1", "error number 399 of many, said on stderr\n", 0, NULL);
    util_case(m, "{ filo ~/loud.filo; filo ~/loud.filo; } 2> ~/e2; wc -l ~/e2",
              "800 /home/guest/e2\n", 0, NULL);
    util_case(m, "echo x 2>> ~/e1; filo ~/loud.filo 2>> ~/e1; wc -l ~/e1", "800 /home/guest/e1\n",
              0, NULL);
    /* sort holds every line: past 25000 it says so, before running out */
    util_case(m, "seq 20001 | sort", "", 2, "sort: more than 20000 lines");
    util_case(m, "seq 20000 | sort -n | tail -n 1", "20000\n", 0, NULL);
    /* wc by blocks: a character cut by the block's end is carried over */
    static const char u8[] =
        "(out-write \"a\" (str-join \"\" (map (fn (i) \"\303\251\") (range 0 40000))) \" b\")";
    CHECK(roc_write_file(m, "t", "/home/guest/u8.filo", (const uint8_t *)u8, sizeof(u8) - 1));
    util_case(m, "filo ~/u8.filo > ~/u8; wc -m ~/u8", "40003 /home/guest/u8\n", 0, NULL);
    util_case(m, "wc ~/u8", "0 2 80003 /home/guest/u8\n", 0, NULL);
    /* and the filters that go a character at a time, a window of it at once */
    util_case(m, "fold -w 80 ~/wide.out > ~/x; wc ~/x", "3874 3875 313874 /home/guest/x\n", 0,
              NULL);
    util_case(m, "fold -s -w 80 ~/wide.out > ~/x; wc ~/x", "3874 3875 313874 /home/guest/x\n", 0,
              NULL);
    util_case(m, "expand ~/wide.out > ~/x; wc ~/x", "0 1 310000 /home/guest/x\n", 0, NULL);
    util_case(m, "unexpand ~/wide.out > ~/x; wc ~/x", "0 1 310000 /home/guest/x\n", 0, NULL);
    util_case(m, "cut -c 300000- ~/wide.out > ~/x; wc ~/x", "1 1 10002 /home/guest/x\n", 0, NULL);
    util_case(m, "cut -d 5 -f 30000- ~/wide.out > ~/x; wc ~/x", "1 1 10015 /home/guest/x\n", 0,
              NULL);
    util_case(m, "seq 3 | tee ~/te1 | wc -l", "3\n", 0, NULL);
    util_case(m, "echo 4 | tee -a ~/te1", "4\n", 0, NULL);
    util_case(m, "cat ~/te1", "1\n2\n3\n4\n", 0, NULL);
    /* the home in memory writes one file at a time: the second is said */
    util_case(m, "echo x | tee ~/te1 ~/te2", "x\n", 1, "tee: /home/guest/te2: Device busy");
    util_case(m, "echo x | tee ~/nodir/x", "x\n", 1, "tee: /home/guest/nodir/x:");
    util_case(m, "seq 3000 | pr | wc -l", "3564\n", 0, NULL);
    util_case(m, "seq 199 > ~/ta; seq 2 200 > ~/tb; paste -d ' ' ~/ta ~/tb | tsort | tail -n 1",
              "200\n", 0, NULL);
    static const struct {
        const char *line, *want;
        int status;
    } exprs[] = {
        {"expr -7 / 2", "-3\n", 0},
        {"expr -7 % 2", "-1\n", 0},
        {"expr 7 % -2", "1\n", 0},
        {"expr abc : 'a\\(b\\)c'", "b\n", 0},
        {"expr abc : abc", "3\n", 0},
        {"expr abc : x", "0\n", 1},
        {"expr 10 \\< 9", "0\n", 1},
        {"expr a \\< b", "1\n", 0},
        {"expr 10 = 010", "1\n", 0},
        {"expr ab != ab", "0\n", 1},
        {"expr 0 \\& 1", "0\n", 1},
        {"expr 3 \\| 0", "3\n", 0},
        {"expr 2 \\* \\( 3 + 4 \\) - 1", "13\n", 0},
    };
    for (size_t i = 0; i < sizeof(exprs) / sizeof(exprs[0]); i++) {
        util_case(m, exprs[i].line, exprs[i].want, exprs[i].status, NULL);
    }
    util_case(m, "expr 1 / 0", "", 2, "expr: division by zero");
    util_case(m, "expr \\( 1 + 2", "", 2, "expr: syntax error");
    util_case(m, "expr 1 2", "", 2, "expr: syntax error");
    util_case(m, "expr", "", 2, "usage: expr");
    util_case(m, "uname", "rocchetto\n", 0, NULL);
    util_case(m, "uname -n", "shell.test\n", 0, NULL);
    util_case(m, "uname -sr", "rocchetto " ROC_VERSION "\n", 0, NULL);
    util_case(m, "uname -a", "rocchetto shell.test " ROC_VERSION " " ROC_VERSION " " UNAME_M "\n",
              0, NULL);
    util_case(m, "uname -x", "", 2, "usage: uname");
    util_case(m, "hostname", "shell.test\n", 0, NULL);
    util_case(m, "logname", "guest\n", 0, NULL);
    util_case(m, "printf 'a b\nb c\na d\nd c\nx x\n' | tsort", "a\nb\nd\nc\nx\n", 0, NULL);
    util_case(m, "printf 'a b b a c d\n' | tsort", "c\nd\na\nb\n", 1, "tsort: cycle in data");
    util_case(m, "printf 'a b c\n' | tsort", "", 1, "tsort: odd data count");
    util_case(m, "printf '' | tsort", "", 0, NULL);
    util_case(m, "seq 5 > ~/sp; cd; split -l 2 sp; cat xaa xab xac", "1\n2\n3\n4\n5\n", 0, NULL);
    util_case(m, "cat xac", "5\n", 0, NULL);
    util_case(m, "split -b 3 -a 3 sp p; cat paad", "\n", 0, NULL);
    util_case(m, "cat paaa", "1\n2", 0, NULL);
    util_case(m, "seq 30 | split -l 1 -a 1 - t", "", 1, "split: too many files");
    util_case(m, "cat tz", "26\n", 0, NULL);
    util_case(m, "split -l 0 sp", "", 2, "usage: split");
    util_case(m, "split -l 2k sp", "", 2, "usage: split");
    send(m, "trap 'echo leaving' EXIT; exit\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nleaving\r\n") != NULL);
}

/* The match of pattern in s, as "so-eo" (and \1's after a :), or "no". */
static const char *re_find(const char *pattern, bool ere, const char *s) {
    static sh_regex re;
    static char out[64];
    char why[64];
    if (!sh_regex_compile(&re, pattern, ere, false, why, sizeof(why))) {
        (void)snprintf(out, sizeof(out), "err:%s", why);
        return out;
    }
    long so[RE_GROUPS];
    long eo[RE_GROUPS];
    int r = sh_regex_exec(&re, s, strlen(s), 0, false, so, eo);
    if (r <= 0) {
        return r < 0 ? "slow" : "no";
    }
    if (so[1] >= 0) {
        (void)snprintf(out, sizeof(out), "%ld-%ld:%ld-%ld", so[0], eo[0], so[1], eo[1]);
    } else {
        (void)snprintf(out, sizeof(out), "%ld-%ld", so[0], eo[0]);
    }
    return out;
}

static void test_regex(void) {
    CHECK(strcmp(re_find("b.d", false, "abcde"), "1-4") == 0);
    CHECK(strcmp(re_find("a*", false, "xaaa"), "0-0") == 0);      /* leftmost first */
    CHECK(strcmp(re_find("xa*", false, "xaaay"), "0-4") == 0);    /* then longest */
    CHECK(strcmp(re_find("a|ab|abc", true, "abcd"), "0-3") == 0); /* POSIX: longest */
    CHECK(strcmp(re_find("\\(ab\\)*c", false, "ababc"), "0-5:2-4") == 0);
    CHECK(strcmp(re_find("(ab)+", true, "xababx"), "1-5:3-5") == 0);
    CHECK(strcmp(re_find("a+", false, "a+b"), "0-2") == 0);    /* + is itself in a BRE */
    CHECK(strcmp(re_find("a\\+", false, "xaab"), "1-3") == 0); /* GNU's \+ */
    CHECK(strcmp(re_find("^ab$", false, "ab"), "0-2") == 0);
    CHECK(strcmp(re_find("^b", false, "ab"), "no") == 0);
    CHECK(strcmp(re_find("a^b", false, "xa^b"), "1-4") == 0); /* ^ inside: itself */
    CHECK(strcmp(re_find("[[:digit:]]\\{2,3\\}", false, "a12345"), "1-4") == 0);
    CHECK(strcmp(re_find("[0-9]{2}", true, "a1b22"), "3-5") == 0);
    CHECK(strcmp(re_find("[^a-c]+", true, "abcxyzab"), "3-6") == 0);
    CHECK(strcmp(re_find("[]a]+", true, "x]a]y"), "1-4") == 0);
    CHECK(strcmp(re_find("\\(.\\)\\1", false, "abccd"), "2-4:2-3") == 0);
    CHECK(strcmp(re_find("(a*)*b", true, "aab"), "0-3:0-2") == 0); /* a star of what may be empty */
    CHECK(strcmp(re_find("x{2,}", true, "xxxxy"), "0-4") == 0);
    CHECK(strcmp(re_find("colou?r", true, "the color"), "4-9") == 0);
    CHECK(strcmp(re_find("a{", true, "a{"), "0-2") == 0);
    CHECK(strncmp(re_find("(a", true, "a"), "err:unmatched (", 15) == 0);
    CHECK(strncmp(re_find("[a", false, "a"), "err:unmatched [", 15) == 0);
    CHECK(strncmp(re_find("\\2\\(a\\)", false, "a"), "err:", 4) == 0);
    CHECK(strcmp(re_find("(a|b)*c", true, "ababababababababababx"), "no") == 0);
    CHECK(strcmp(re_find("(a*)*(a*)*(a*)*(a*)*z", true, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
                 "slow") == 0);
    static sh_regex re;
    char why[64];
    long so[RE_GROUPS];
    long eo[RE_GROUPS];
    CHECK(sh_regex_compile(&re, "HELLO", false, true, why, sizeof(why)));
    CHECK(sh_regex_exec(&re, "say hello", 9, 0, false, so, eo) == 1 && so[0] == 4);
    CHECK(sh_regex_compile(&re, "^a", false, false, why, sizeof(why)));
    CHECK(sh_regex_exec(&re, "aa", 2, 1, true, so, eo) == 0); /* not a line's start any more */
}

static void test_grep_sed(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    const char *text = "alpha 1\nbeta 22\ngamma 333\ndelta\nAlpha 4\n";
    CHECK(roc_write_file(m, "test", "/home/guest/g.txt", (const uint8_t *)text, strlen(text)));
    send(m, "cd ~; grep '[0-9]\\{2,\\}' g.txt; grep -E '^(a|d)' g.txt; grep -ic alpha g.txt\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\nbeta 22\r\ngamma 333\r\nalpha 1\r\ndelta\r\n2\r\n") != NULL);
    send(m, "grep -vn a g.txt; grep -x delta g.txt; grep -F 'a.p' g.txt; echo $?; grep -E '(' "
            "g.txt; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\ndelta\r\n1\r\n") != NULL && strstr(o, "unmatched (") != NULL &&
          strstr(o, "\r\n2\r\n") != NULL);
    send(m, "grep -vn '[ae]' g.txt; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n1\r\n") != NULL); /* every line has an a or an e: none, $? 1 */

    send(m, "sed 's/a/A/' g.txt | head -n 2; sed -n '/^[bg]/p' g.txt; sed -n '2,3=' g.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nAlpha 1\r\nbetA 22\r\nbeta 22\r\ngamma 333\r\n2\r\n3\r\n") != NULL);
    send(m, "sed -E 's/([a-z]+) ([0-9]+)/\\2:\\1/g; /delta/d; $s/.*/[&]/' g.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n1:alpha\r\n22:beta\r\n333:gamma\r\n[A4:lpha]\r\n") !=
          NULL); /* [a-z]: no A */
    send(m, "echo hello world | sed 's/o/0/2; y/hl/HL/'; echo aaa | sed 's/a*/x/g'; echo abc | sed "
            "'s/x*/-/g'\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nHeLLo w0rLd\r\nx\r\n-a-b-c-\r\n") != NULL);
    send(m,
         "printf '1\\n2\\n3\\n4\\n' | sed -n 'h;n;G;p'; printf 'a\\nb\\n' | sed '1i top' | sed '$a "
         "end' | sed '2c two'\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n2\r\n1\r\n4\r\n3\r\n") != NULL &&
          strstr(o, "\r\ntop\r\ntwo\r\nb\r\nend\r\n") != NULL);
    send(m,
         "sed -i 's/[0-9]//g' g.txt; head -n 2 g.txt; sed -n '/beta/,/delta/{/gamma/!p}' g.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nalpha \r\nbeta \r\nbeta \r\ndelta\r\n") != NULL);
    send(m, "sed 's/a/b' g.txt; sed 'k' g.txt; sed\r");
    o = drain(m);
    CHECK(strstr(o, "unterminated") != NULL && strstr(o, "unknown command") != NULL &&
          strstr(o, "usage: sed") != NULL);
}

static void test_seq_touch_cmp(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "seq 3; seq 2 4; seq 10 -5 1; seq 0.5 0.25 1; seq 3 1; echo end\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\n1\r\n2\r\n3\r\n2\r\n3\r\n4\r\n10\r\n5\r\n0.5\r\n0.75\r\n1\r\nend\r\n") !=
          NULL);
    send(m, "cd ~; touch a.txt b.txt; echo x > b.txt; touch b.txt; wc -c a.txt; wc -c b.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n0 a.txt\r\n2 b.txt\r\n") != NULL);
    send(m, "printf 'ab\\ncd\\n' > p; printf 'ab\\ncx\\n' > q; cmp p q; echo $?; cmp p p; echo $?; "
            "printf 'ab\\n' | cmp p -; "
            "cmp -s p q; echo $?; cmp p nope; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\np q differ: char 5, line 2\r\n1\r\n0\r\ncmp: EOF on -\r\n1\r\n") != NULL &&
          strstr(o, "\r\n2\r\n") != NULL);
}

static void test_shell_compound(void) {
    char why[96];
    CHECK(sh_check("if true; then echo a; elif false; then echo b; else echo c; fi", why,
                   sizeof(why)));
    CHECK(sh_check("for f in a b; do while false; do :; done; done && echo ok", why, sizeof(why)));
    CHECK(!sh_check("echo a; fi", why, sizeof(why)));
    CHECK(strcmp(why, "Syntax error: \"fi\" unexpected") == 0);
    CHECK(!sh_check("while true; do echo x", why, sizeof(why)));
    CHECK(sh_needs_more(why) && strstr(why, "expecting \"done\"") != NULL);
    CHECK(!sh_check("if true; then; fi", why, sizeof(why)));
    CHECK(!sh_needs_more(why));

    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "if test a = b; then echo yes; elif [ -d /pub ]; then echo dir; else echo no; fi\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\ndir\r\n") != NULL && strstr(o, "\r\nyes\r\n") == NULL &&
          strstr(o, "\r\nno\r\n") == NULL);
    send(m, "if false; then echo x; fi; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n0\r\n") != NULL);
    send(m, "n=; while [ \"$n\" != xxx ]; do n=x$n; echo -$n-; done\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n-x-\r\n-xx-\r\n-xxx-\r\n") != NULL);
    send(m, "for w in one 'two three' four; do echo [$w]; done\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n[one]\r\n[two three]\r\n[four]\r\n") != NULL);
    send(m, "for n in 1 2 3 4; do if [ $n -eq 2 ]; then continue; fi; if [ $n -gt 3 ]; then "
            "break; fi; echo n$n; done; echo end\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nn1\r\nn3\r\nend\r\n") != NULL);
    send(m, "for a in x y; do for b in 1 2; do echo $a$b; break 2; done; done\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nx1\r\n") != NULL && strstr(o, "\r\ny1\r\n") == NULL);
    send(m, "until true; do echo never; done; false || for z in; do echo no; done; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nnever\r\n") == NULL && strstr(o, "\r\n0\r\n") != NULL);
    send(m, "true && if false; then :; fi || echo or\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nor\r\n") == NULL); /* the if ended 0 */
    send(m, "false && while true; do echo loop; done; echo past\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nloop\r\n") == NULL && strstr(o, "\r\npast\r\n") != NULL);
    send(m, "break\r");
    o = drain(m);
    CHECK(strstr(o, "only in a loop") != NULL);

    /* lines that want more: "> " until the if closes */
    send(m, "if true\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n> ") != NULL);
    send(m, "then\r");
    drain(m);
    send(m, "  echo inside # a comment\r");
    o = drain(m);
    CHECK(strstr(o, "inside\r\n") == NULL || strstr(o, "\r\ninside\r\n") == NULL);
    send(m, "fi\r");
    o = drain(m);
    CHECK(strstr(o, "\r\ninside\r\n") != NULL && strstr(o, "$ ") != NULL);
    send(m, "echo a &&\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n> ") != NULL);
    send(m, "echo b\r");
    o = drain(m);
    CHECK(strstr(o, "\r\na\r\nb\r\n") != NULL);
    send(m, "echo one \\\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n> ") != NULL);
    send(m, "two \"th\\\r");
    drain(m);
    send(m, "ree\"\r");
    o = drain(m);
    CHECK(strstr(o, "\r\none two three\r\n") != NULL);
    send(m, "for x in 1\r");
    drain(m);
    send(m, "\x03");
    o = drain(m);
    CHECK(strstr(o, "^C") != NULL && strstr(o, "$ ") != NULL);
    send(m, "echo after\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nafter\r\n") != NULL);
    send(m, "while true\r");
    drain(m);
    send(m, "\x04");
    o = drain(m);
    CHECK(strstr(o, "end of file unexpected") != NULL);

    /* a block's output: | and > after its fi or done */
    CHECK(
        sh_check("for i in a; do echo; done | sort > f; if :; then :; fi >> g", why, sizeof(why)));
    CHECK(!sh_check("if :; then :; fi x", why, sizeof(why)));
    CHECK(strcmp(why, "Syntax error: word unexpected") == 0);
    CHECK(sh_check("for i in a; do :; done 2> f 2>&1", why, sizeof(why)));
    send(m, "for w in b c a; do echo $w; done | sort\r");
    o = drain(m);
    CHECK(strstr(o, "\r\na\r\nb\r\nc\r\n") != NULL);
    send(m, "if true; then echo in; fi > ~/if.txt; for i in 1 2; do echo $i; done >> ~/if.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nin\r\n") == NULL);
    const uint8_t *fd = NULL;
    size_t fn = 0;
    CHECK(ufs_find(&m->uf, "/home/guest/if.txt", &fd, &fn) && fn == 7 &&
          memcmp(fd, "in\n1\n2\n", 7) == 0);
    send(m, "for a in x y; do for b in 1 2; do echo $a$b | sort; done | sort -r; done | sort -r | "
            "head -n 3\r");
    o = drain(m);
    CHECK(strstr(o, "\r\ny2\r\ny1\r\nx2\r\n") != NULL && strstr(o, "\r\nx1\r\n") == NULL);
    send(m, "false && for i in 1; do echo no; done | wc; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nno\r\n") == NULL && strstr(o, "\r\n1 1") == NULL &&
          strstr(o, "\r\n1\r\n") != NULL);
    send(m, "x=$(for i in b a; do echo $i; done | sort); echo [$x]\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n[a b]\r\n") != NULL);
    send(m, "for i in 1 2; do echo $i; if [ $i = 2 ]; then break; fi; done > /dev/null; echo ok\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n1\r\n") == NULL && strstr(o, "\r\nok\r\n") != NULL);
    h.pending = 0;
    send(m, "for i in 1; do cat /pub/kutta.md; echo two; done | wc\r");
    o = drain(m);
    CHECK(h.pending == 1);
    roc_feed(m, h.req_id, (const uint8_t *)"hello\n", 6);
    roc_feed_eof(m, h.req_id);
    o = drain(m);
    CHECK(strstr(o, "hello") == NULL && strncmp(o, "2 2 10\r\n", 8) == 0);

    /* a loop that does not end: the tick comes back to it, ^C ends it */
    send(m, "while true; do :; done\r");
    o = drain(m);
    CHECK(strstr(o, "$ ") == NULL);
    for (int i = 0; i < 5; i++) {
        roc_tick(m, 16);
    }
    send(m, "echo typed\x03");
    o = drain(m);
    CHECK(strstr(o, "^C") != NULL && strstr(o, "typed") == NULL && strstr(o, "$ ") != NULL);
    send(m, "echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n130\r\n") != NULL);
    send(m, "x=$(while true; do :; done)\r");
    o = drain(m);
    CHECK(strstr(o, "$(...) ran too long") != NULL);

    /* a file from the site in a loop: the loop waits for it */
    h.pending = 0;
    send(m, "for i in 1 2; do cat /pub/kutta.md; done; echo last\r");
    o = drain(m);
    CHECK(h.pending == 1 && strstr(o, "\r\nlast\r\n") == NULL);
    const char *body = "hello\n";
    h.pending = 0;
    roc_feed(m, h.req_id, (const uint8_t *)body, strlen(body));
    roc_feed_eof(m, h.req_id);
    o = drain(m);
    CHECK(h.pending == 1 && strstr(o, "\r\nlast\r\n") == NULL);
    roc_feed(m, h.req_id, (const uint8_t *)body, strlen(body));
    roc_feed_eof(m, h.req_id);
    o = drain(m);
    CHECK(strstr(o, "\r\nlast\r\n") != NULL);
}

static void test_shell_pipes(void) {
    char why[64];
    CHECK(sh_check("a | b | c && d || e", why, sizeof(why)));
    CHECK(!sh_check("a |", why, sizeof(why)) &&
          strcmp(why, "Syntax error: end of file unexpected") == 0);
    CHECK(!sh_check("| a", why, sizeof(why)) && strcmp(why, "Syntax error: \"|\" unexpected") == 0);

    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "echo hello | cat\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\nhello\r\n") != NULL);
    send(m, "ls /pub | grep kutta\r");
    o = drain(m);
    CHECK(strstr(o, "kutta.md") != NULL && strstr(o, "café") == NULL);
    send(m, "echo one two three | wc\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n1 3 14\r\n") != NULL); /* no name: it read no file */
    const char *lines = "a1\nb2\na3\n";
    CHECK(roc_write_file(m, "test", "/home/guest/l.txt", (const uint8_t *)lines, strlen(lines)));
    send(m, "cat ~/l.txt | grep a | head -n 1\r");
    o = drain(m);
    CHECK(strstr(o, "\r\na1\r\n") != NULL && strstr(o, "a3") == NULL);
    send(m, "cat ~/l.txt | grep -n b\r");
    o = drain(m);
    CHECK(strstr(o, "2:b2") != NULL);
    const char *up = "(echo (str-upper STDIN))\n";
    CHECK(roc_write_file(m, "test", "/home/guest/up.filo", (const uint8_t *)up, strlen(up)));
    send(m, "echo quiet | filo ~/up.filo\r");
    o = drain(m);
    CHECK(strstr(o, "QUIET") != NULL);
    send(m, "filo ~/up.filo\r"); /* no |: STDIN is empty */
    o = drain(m);
    CHECK(strstr(o, "QUIET") == NULL);
    send(m, "echo x | ls /pub\r"); /* a command that reads nothing */
    o = drain(m);
    CHECK(strstr(o, "kutta.md") != NULL);
    send(m, "echo a | cat > ~/p.txt\r");
    drain(m);
    const uint8_t *d = NULL;
    size_t n = 0;
    CHECK(ufs_find(&m->uf, "/home/guest/p.txt", &d, &n) && n == 2 && memcmp(d, "a\n", 2) == 0);
    send(m, "echo a > ~/q.txt | cat\r"); /* > wins: the next reads nothing */
    o = drain(m);
    CHECK(strstr(o, "\r\na\r\n") == NULL);
    send(m, "ls /nothere && echo skipped | cat\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nskipped\r\n") == NULL);
    send(m, "ls /nothere | cat; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n0\r\n") != NULL);
    /* a file from the site on the left: the right waits for all of it */
    h.pending = 0;
    send(m, "cat /pub/kutta.md | grep -n lift\r");
    o = drain(m);
    CHECK(h.pending == 1);
    const char *body = "lift here\nnothing\nmore lift\n";
    roc_feed(m, h.req_id, (const uint8_t *)body, strlen(body));
    roc_feed_eof(m, h.req_id);
    o = drain(m);
    CHECK(strstr(o, "1:lift here") != NULL && strstr(o, "3:more lift") != NULL);
    CHECK(strstr(o, "nothing") == NULL);
}

/* < reads a file as the input, 2> sends the refusals to a file, 2>&1 where
   the output goes, and /dev/null takes what it is given. */
static void test_shell_redirections(void) {
    static sh_line sl;
    char why[64];
    CHECK(sh_read("grep a < f > o 2> e", &sl, fake_vars, NULL, why, sizeof(why)));
    CHECK(sl.argc == 2 && strcmp(sl.in, "f") == 0 && strcmp(sl.out, "o") == 0);
    CHECK(strcmp(sl.err, "e") == 0 && !sl.err_append && !sl.err_to_out);
    CHECK(sh_read("cmd 2>&1 | x", &sl, fake_vars, NULL, why, sizeof(why)));
    CHECK(sl.argc == 1 && sl.err_to_out && sl.err == NULL && sl.op == SH_OP_PIPE);
    CHECK(sh_read("echo 2>>f", &sl, fake_vars, NULL, why, sizeof(why)));
    CHECK(sl.argc == 1 && strcmp(sl.err, "f") == 0 && sl.err_append && sl.out == NULL);
    CHECK(sh_read("echo a2>f '2'>g 1>h", &sl, fake_vars, NULL, why, sizeof(why)));
    CHECK(sl.argc == 3 && strcmp(sl.argv[1], "a2") == 0 && strcmp(sl.argv[2], "2") == 0);
    CHECK(sl.err == NULL && strcmp(sl.out, "h") == 0);
    CHECK(sh_read("echo 2 >f", &sl, fake_vars, NULL, why, sizeof(why)) && sl.argc == 2);
    CHECK(!sh_read("echo 2>&3", &sl, fake_vars, NULL, why, sizeof(why)));
    CHECK(strcmp(why, "Syntax error: Bad fd number") == 0);
    CHECK(!sh_read("cat < > f", &sl, fake_vars, NULL, why, sizeof(why)));
    CHECK(!sh_read("cat <", &sl, fake_vars, NULL, why, sizeof(why)));
    CHECK(sh_check("< f; 2> e; 2>&1", why, sizeof(why)));

    mock_host h;
    roc *m = boot(&h);
    drain(m);
    const char *lines = "a1\nb2\na3\n";
    CHECK(roc_write_file(m, "test", "/home/guest/l.txt", (const uint8_t *)lines, strlen(lines)));
    send(m, "grep a < ~/l.txt\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\na1\r\n") != NULL && strstr(o, "\r\na3\r\n") != NULL &&
          strstr(o, "b2") == NULL);
    send(m, "wc < ~/l.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n3 3 9\r\n") != NULL);
    send(m, "cat < ~/nothere; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "/home/guest/nothere: No such file or directory") != NULL);
    CHECK(strstr(o, "\r\n1\r\n") != NULL);
    send(m, "cat < /pub/kutta.md\r");
    o = drain(m);
    CHECK(strstr(o, "On the site, not here") != NULL);
    send(m, "ls /nothere 2> ~/e.txt\r");
    o = drain(m);
    CHECK(strstr(o, "No such file") == NULL);
    send(m, "ls /nowhere 2>> ~/e.txt\r");
    drain(m);
    const uint8_t *d = NULL;
    size_t n = 0;
    const char *want = "ls: /nothere: No such file or directory\n"
                       "ls: /nowhere: No such file or directory\n";
    CHECK(ufs_find(&m->uf, "/home/guest/e.txt", &d, &n) && n == strlen(want) &&
          memcmp(d, want, n) == 0);
    send(m, "ls /nothere 2> /dev/null; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "No such file") == NULL && strstr(o, "\r\n1\r\n") != NULL);
    send(m, "ls /pub > /dev/null\r");
    o = drain(m);
    CHECK(strstr(o, "kutta") == NULL);
    send(m, "ls /nothere 2>&1 | wc\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n1 7 40\r\n") != NULL); /* the refusal, as a line of output */
    const char *bad = "(+ 1 \"a\")\n";
    CHECK(roc_write_file(m, "test", "/home/guest/bad.filo", (const uint8_t *)bad, strlen(bad)));
    send(m, "filo ~/bad.filo 2> ~/e2.txt\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: bad") == NULL);
    CHECK(ufs_find(&m->uf, "/home/guest/e2.txt", &d, &n) && n > 0 &&
          memcmp(d, "rocchetto: /home/guest/bad.filo: in builtin", 36) == 0);
    send(m, "cat < ~/l.txt > ~/l.txt\r"); /* > empties it first, as in sh */
    drain(m);
    CHECK(ufs_find(&m->uf, "/home/guest/l.txt", &d, &n) && n == 0);
}

static char subst_seen[256];

static const char *fake_subst(void *user, const char *cmd, size_t len, size_t *out_len, char *why,
                              size_t cap) {
    (void)user;
    (void)why;
    (void)cap;
    (void)snprintf(subst_seen, sizeof(subst_seen), "%.*s", (int)len, cmd);
    *out_len = 5;
    return "a b\n\n";
}

/* $(command): what it writes, its last newlines dropped, split like $x
   unless quoted; the line that called it goes on where it was. */
static void test_shell_substitution(void) {
    static sh_line sl;
    char why[96];
    const sh_env env = {fake_vars, fake_subst, NULL, NULL, NULL};
    CHECK(sh_expand("echo $(x) \"$(x)\" pre$(x)post", &sl, &env, why, sizeof(why)));
    const char *want[] = {"echo", "a", "b", "a b", "prea", "bpost"};
    CHECK(sl.argc == 6);
    for (int i = 0; i < 6 && i < sl.argc; i++) {
        CHECK(strcmp(sl.argv[i], want[i]) == 0);
    }
    CHECK(sh_expand("echo $(echo ')' \"(\" (a)) z", &sl, &env, why, sizeof(why)));
    CHECK(strcmp(subst_seen, "echo ')' \"(\" (a)") == 0 && sl.argc == 4);
    CHECK(!sh_expand("echo $(echo", &sl, &env, why, sizeof(why)));
    CHECK(strcmp(why, "Syntax error: end of file unexpected (expecting \")\")") == 0);
    CHECK(sh_split("echo $(x)", &sl, why, sizeof(why)) && strcmp(sl.argv[1], "$(x)") == 0);
    CHECK(sh_check("echo $(ls | wc) && echo ok", why, sizeof(why)));
    CHECK(!sh_check("echo $(a;;b)", why, sizeof(why)) &&
          strcmp(why, "Syntax error: \";;\" unexpected") == 0);
    CHECK(!sh_check("echo $(echo 'a)", why, sizeof(why)));

    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "echo [$(echo hi)]\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\n[hi]\r\n") != NULL);
    send(m, "x=$(echo a   b); echo \"[$x]\"\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n[a b]\r\n") != NULL);
    send(m, "echo \"$(ls /pub | grep kutta)\"\r");
    o = drain(m);
    CHECK(strstr(o, "kutta.md") != NULL);
    send(m, "cd $(echo /pub); pwd\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n/pub\r\n") != NULL);
    send(m, "echo zzz | grep $(echo zzz)\r"); /* the | input is still there after */
    o = drain(m);
    CHECK(strstr(o, "\r\nzzz\r\n") != NULL);
    send(m, "echo $(echo a > ~/s.txt; echo b)\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nb\r\n") != NULL);
    const uint8_t *d = NULL;
    size_t n = 0;
    CHECK(ufs_find(&m->uf, "/home/guest/s.txt", &d, &n) && n == 2 && memcmp(d, "a\n", 2) == 0);
    send(m, "echo [$(ls /nothere)]\r"); /* refusals reach the terminal, not the words */
    o = drain(m);
    CHECK(strstr(o, "No such file") != NULL && strstr(o, "\r\n[]\r\n") != NULL);
    send(m, "echo a; echo $(echo b); echo c\r");
    o = drain(m);
    CHECK(strstr(o, "\r\na\r\nb\r\nc\r\n") != NULL);
    send(m, "echo \"$(echo a $(echo b $(echo c | wc)) d)\" | wc; echo $(echo $(echo $(echo $(echo "
            "4))))\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n1 6 12\r\n4\r\n") != NULL);
    send(m, "echo $(echo $(echo $(echo $(echo $(echo 5)))))\r");
    o = drain(m);
    CHECK(strstr(o, "Too deeply nested") != NULL);
    h.pending = 0;
    send(m, "echo $(cat /pub/kutta.md); echo next\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: $(...) cannot wait for the site or a screen\r\n") != NULL);
    CHECK(strstr(o, "\r\nnext\r\n") == NULL);
    CHECK(m->mode == ROC_MODE_LINE && m->req_kind == ROC_REQ_NONE);
    send(m, "echo still\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nstill\r\n") != NULL);
}

/* The line as sh reads it: quotes, backslashes and comments make the
   words, and the words reach every command whole. */
static void test_shell_words(void) {
    static sh_line sl;
    char why[64];
    CHECK(sh_split("a 'b  c' \"d \\\"e\\\" \\$f\" g\\ h i'j'\"k\" '' # l", &sl, why, sizeof(why)));
    CHECK(sl.argc == 6 && sl.out == NULL);
    CHECK(strcmp(sl.argv[0], "a") == 0 && strcmp(sl.argv[1], "b  c") == 0);
    CHECK(strcmp(sl.argv[2], "d \"e\" $f") == 0 && strcmp(sl.argv[3], "g h") == 0);
    CHECK(strcmp(sl.argv[4], "ijk") == 0 && strcmp(sl.argv[5], "") == 0);
    CHECK(sh_split("echo a>b c >> 'd e'", &sl, why, sizeof(why)));
    CHECK(sl.argc == 3 && strcmp(sl.argv[2], "c") == 0 && sl.append && strcmp(sl.out, "d e") == 0);
    CHECK(sh_split("echo a#b '>' \\#", &sl, why, sizeof(why)) && sl.argc == 4 && sl.out == NULL);
    CHECK(strcmp(sl.argv[1], "a#b") == 0 && strcmp(sl.argv[2], ">") == 0);
    CHECK(!sh_split("echo 'abc", &sl, why, sizeof(why)));
    CHECK(strcmp(why, "Syntax error: Unterminated quoted string") == 0);
    CHECK(!sh_split("echo \"abc", &sl, why, sizeof(why)));
    CHECK(!sh_split("echo > > x", &sl, why, sizeof(why)));
    /* joined back, the same words */
    char *words[] = {"plain", "two words", "it's", "", "~/a-b_c.d", "$x", "#"};
    char line[256];
    CHECK(sh_join(words, 7, line, sizeof(line)));
    CHECK(strcmp(line, "plain 'two words' 'it'\\''s' '' ~/a-b_c.d '$x' '#'") == 0);
    CHECK(sh_split(line, &sl, why, sizeof(why)) && sl.argc == 7);
    for (int i = 0; i < 7 && i < sl.argc; i++) {
        CHECK(strcmp(sl.argv[i], words[i]) == 0);
    }
    CHECK(!sh_join(words, 7, line, 10));

    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "echo 'a  b' \"it's\" c\\ d '' x\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\na  b it's c d  x\r\n") != NULL);
    send(m, "echo '\xf0\x9f\x98\x80x'\r"); /* past 0xFFFF: a rune, not a key */
    o = drain(m);
    CHECK(strstr(o, "\r\n\xf0\x9f\x98\x80x\r\n") != NULL);
    CHECK(!ft_key_is_code(0x1F600) && ft_key_is_code(FT_KEY_UP) && ft_key_is_code(FT_KEY_F1 + 11));
    CHECK(!ft_key_is_code('a') && ft_key_is_code(FT_KEY_UP | FT_KEY_CTRL | FT_KEY_SHIFT));
    send(m, "echo 'x > y' # not written\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nx > y\r\n") != NULL);
    send(m, "echo 'open\r"); /* an open quote asks for more, as sh */
    o = drain(m);
    CHECK(strstr(o, "Unterminated") == NULL && strstr(o, "\r\n> ") != NULL);
    send(m, "shut'\r");
    o = drain(m);
    CHECK(strstr(o, "shut") != NULL && strstr(o, "open") != NULL);
    send(m, "echo 'one  two'>~/q.txt\r");
    drain(m);
    const uint8_t *d = NULL;
    size_t n = 0;
    CHECK(ufs_find(&m->uf, "/home/guest/q.txt", &d, &n) && n == 9 &&
          memcmp(d, "one  two\n", 9) == 0);
    /* a program's ARGS, a path with a space, grep's pattern: each word whole */
    const char *args = "(echo (length ARGS) (nth ARGS 0))\n";
    CHECK(roc_write_file(m, "test", "/home/guest/args.filo", (const uint8_t *)args, strlen(args)));
    send(m, "filo ~/args.filo 'a b' c\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n2 a b\r\n") != NULL);
    send(m, "filo run ~/args.filo \"x y z\"\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n1 x y z\r\n") != NULL);
    send(m, "mkdir ~/'my dir'\r");
    drain(m);
    send(m, "cd '$HOME'\r"); /* single quotes: no $ */
    o = drain(m);
    CHECK(strstr(o, "rocchetto: cd: $HOME: No such file or directory") != NULL);
    send(m, "cd ~/my\\ dir\r");
    drain(m);
    send(m, "pwd\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n/home/guest/my dir\r\n") != NULL);
    send(m, "cd a b\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: cd: b: too many arguments") != NULL);
    const char *text = "one two\nonetwo\n";
    CHECK(roc_write_file(m, "test", "/home/guest/g.txt", (const uint8_t *)text, strlen(text)));
    send(m, "grep -n 'one two' ~/g.txt\r");
    o = drain(m);
    CHECK(strstr(o, "1:one two") != NULL && strstr(o, "onetwo") == NULL);
    send(m, "ls -aF ~\r");
    o = drain(m);
    CHECK(strstr(o, "my dir/") != NULL);
}

/* Tab at the REPL completes Filo names: the forms, the builtins and the
   globals defined, the REPL's own too; not inside a string. */
static void test_repl_tab(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "filo\r");
    drain(m);
    send(m, "(str-upp\t");
    drain(m);
    CHECK(m->line_len == 11 && memcmp(m->line, "(str-upper ", 11) == 0);
    send(m, "\"x\")\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\"X\"") != NULL);
    send(m, "(def my-long-name 42)\r");
    drain(m);
    send(m, "my-lo\t");
    drain(m);
    CHECK(m->line_len == 13 && memcmp(m->line, "my-long-name ", 13) == 0);
    send(m, "\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n42\r\n") != NULL);
    send(m, "(le\t"); /* let, letv, length...: shared, then listed */
    o = drain(m);
    send(m, "\t");
    o = drain(m);
    CHECK(strstr(o, "let") != NULL && strstr(o, "letv") != NULL && strstr(o, "length") != NULL);
    CHECK(strstr(o, "\r\x1b[9C") != NULL); /* back after "filo> (le", the REPL's prompt counted */
    send(m, "\x03");
    drain(m);
    send(m, "(echo \"str-upp\t");
    drain(m);
    CHECK(m->line_len == 14); /* inside a string: nothing added */
    send(m, "\x03");
    drain(m);
    /* the line in the edt's colours as it is typed; a string open on the
       line before, in the same form, colours this one */
    send(m, "(def c 12) ; note");
    o = drain(m);
    CHECK(strstr(o, "\x1b[38;5;74mdef\x1b[39m") != NULL);
    CHECK(strstr(o, "\x1b[38;5;139m12\x1b[39m") != NULL);
    CHECK(strstr(o, "\x1b[38;5;244m; note\x1b[39m") != NULL);
    send(m, "\x03(str-len \"ab\r");
    drain(m);
    send(m, "cd\")");
    o = drain(m);
    CHECK(strstr(o, "\x1b[38;5;108mcd\"\x1b[39m") != NULL);
    send(m, "\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n5\r\n") != NULL);
}

/* Ctrl-R searches the history backwards as the text is typed, as readline
   does: Enter runs what it found, Esc keeps it to edit, Ctrl-R again goes
   further back, Ctrl-G goes back to the line typed before. */
static void test_history_search(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "echo beta\r");
    send(m, "echo alpha\r");
    send(m, "echo gamma\r");
    drain(m);
    send(m, "\x12"
            "al");
    const char *o = drain(m);
    CHECK(m->srch_on && strstr(o, "(reverse-i-search)`al': echo alpha") != NULL);
    send(m, "\r");
    o = drain(m);
    CHECK(!m->srch_on && strstr(o, "\r\nalpha\r\n") != NULL);
    send(m, "\x12"
            "echo");
    drain(m);
    CHECK(m->line_len == 10 && memcmp(m->line, "echo alpha", 10) == 0); /* the newest */
    send(m, "\x12");
    drain(m);
    CHECK(m->line_len == 10 && memcmp(m->line, "echo gamma", 10) == 0);
    send(m, "\x12"); /* the alpha typed before the one Ctrl-R ran */
    drain(m);
    CHECK(m->line_len == 10 && memcmp(m->line, "echo alpha", 10) == 0);
    send(m, "\x12");
    drain(m);
    CHECK(m->line_len == 9 && memcmp(m->line, "echo beta", 9) == 0);
    send(m, "\x12"); /* nothing older */
    o = drain(m);
    CHECK(strstr(o, "failed reverse-i-search") != NULL);
    send(m, "\x1b[D"); /* an arrow: out, on the line found, to edit */
    drain(m);
    CHECK(!m->srch_on && m->line_len == 9 && memcmp(m->line, "echo beta", 9) == 0);
    send(m, "\x03");
    drain(m);
    send(m, "typed"
            "\x12"
            "gam");
    drain(m);
    CHECK(m->line_len == 10 && memcmp(m->line, "echo gamma", 10) == 0);
    send(m, "\x7f\x7f\x7f"); /* the text gone: the line before is on show */
    drain(m);
    CHECK(m->srch_on && m->srch_qlen == 0 && m->line_len == 5);
    send(m, "zzz");
    o = drain(m);
    CHECK(strstr(o, "(failed reverse-i-search)`zzz'") != NULL);
    send(m, "\x07"); /* Ctrl-G: back to what was typed */
    drain(m);
    CHECK(!m->srch_on && m->line_len == 5 && memcmp(m->line, "typed", 5) == 0);
    send(m, "\x03\x12"
            "beta");
    drain(m);
    send(m, "\x1b");
    roc_tick(m, 1000); /* Esc alone is told apart by time */
    drain(m);
    CHECK(!m->srch_on && m->line_len == 9 && memcmp(m->line, "echo beta", 9) == 0);
}

/* less looks for text as less does: /text Enter, n, N, the text reversed
   where it is, and a word when there is none. */
static void test_pager_search(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    static char text[4096];
    size_t n = 0;
    for (int i = 0; i < 60; i++) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "line %d%s\n", i,
                              i == 30 || i == 50 ? " has Beta in it" : "");
    }
    CHECK(roc_write_file(m, "test", "/home/guest/p.txt", (const uint8_t *)text, n));
    send(m, "less ~/p.txt\r");
    drain(m);
    CHECK(m->pg.top == 0);
    send(m, "/beta");
    const char *o = drain(m);
    CHECK(strstr(o, "/beta") != NULL); /* typed on the status line */
    send(m, "\r");
    o = drain(m);
    CHECK(m->pg.top == 30);
    CHECK(strstr(o, "\x1b[7mBeta\x1b[27m") != NULL); /* lower case finds either */
    send(m, "n");
    drain(m);
    CHECK(m->pg.top == 50 ||
          m->pg.top == m->pg.nrows - (m->t.rows - 1U)); /* at the end, as far as it goes */
    send(m, "N");
    drain(m);
    CHECK(m->pg.top == 30);
    send(m, "/\r"); /* / and Enter alone: the last pattern again */
    drain(m);
    CHECK(m->pg.top == 50 || m->pg.top == m->pg.nrows - (m->t.rows - 1U));
    send(m, "N");
    drain(m);
    send(m, "/BETA\r"); /* a capital: only itself */
    o = drain(m);
    CHECK(strstr(o, "Pattern not found") != NULL && m->pg.top == 30);
    send(m, "g"); /* the message goes, and the key counts */
    drain(m);
    CHECK(m->pg.top == 0);
    send(m, "/x\x1b");
    roc_tick(m, 1000);
    drain(m);
    CHECK(!m->pg.typing && m->pg.top == 0); /* Esc gives up, and does not close */
    CHECK(m->pg.patlen == 4 && memcmp(m->pg.pat, "BETA", 4) == 0); /* the last one stays */
    send(m, "q");
    drain(m);
    CHECK(m->mode == ROC_MODE_LINE);
}

/* diff: the normal format and -u as POSIX diff writes them (checked
   against the system's diff), a last line with no newline, an empty file,
   - for the input of a |, and $? 0 same, 1 different, 2 trouble. */
static void test_diff(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    const char *a = "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\n";
    const char *b = "a\nB\nc\nd\ne\nf\ng\nh\nj\nk\nl\nm\nn\n";
    CHECK(roc_write_file(m, "test", "/home/guest/a.txt", (const uint8_t *)a, strlen(a)));
    CHECK(roc_write_file(m, "test", "/home/guest/b.txt", (const uint8_t *)b, strlen(b)));
    CHECK(roc_write_file(m, "test", "/home/guest/c.txt", (const uint8_t *)"x\ny", 3));
    CHECK(roc_write_file(m, "test", "/home/guest/d.txt", (const uint8_t *)"x\ny\n", 4));
    CHECK(roc_write_file(m, "test", "/home/guest/e.txt", (const uint8_t *)"", 0));
    send(m, "diff ~/a.txt ~/b.txt; echo $?\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\n2c2\r\n< b\r\n---\r\n> B\r\n9d8\r\n< i\r\n13a13\r\n> n\r\n1\r\n") != NULL);
    send(m, "diff -u ~/a.txt ~/b.txt\r");
    o = drain(m);
    CHECK(strstr(o, "--- /home/guest/a.txt\r\n+++ /home/guest/b.txt\r\n@@ -1,13 +1,13 @@\r\n a\r\n"
                    "-b\r\n+B\r\n c\r\n d\r\n e\r\n f\r\n g\r\n h\r\n-i\r\n j\r\n k\r\n l\r\n"
                    " m\r\n+n\r\n") != NULL);
    send(m, "diff ~/c.txt ~/d.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n2c2\r\n< y\r\n\\ No newline at end of file\r\n---\r\n> y\r\n") != NULL);
    send(m, "diff -u ~/c.txt ~/d.txt\r");
    o = drain(m);
    CHECK(strstr(o, "@@ -1,2 +1,2 @@\r\n x\r\n-y\r\n\\ No newline at end of file\r\n+y\r\n") !=
          NULL);
    send(m, "diff ~/e.txt ~/d.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n0a1,2\r\n> x\r\n> y\r\n") != NULL);
    send(m, "diff -u ~/e.txt ~/d.txt\r");
    o = drain(m);
    CHECK(strstr(o, "@@ -0,0 +1,2 @@\r\n+x\r\n+y\r\n") != NULL);
    send(m, "diff ~/a.txt ~/a.txt; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "diff ~/a.txt ~/a.txt; echo $?\r\n0\r\n") != NULL);
    send(m, "cat ~/b.txt | diff ~/a.txt - | head -n 1\r"); /* - is the input */
    o = drain(m);
    CHECK(strstr(o, "\r\n2c2\r\n") != NULL);
    send(m, "diff ~/a.txt ~/nothere; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: diff: /home/guest/nothere: No such file or directory") != NULL);
    CHECK(strstr(o, "\r\n2\r\n") != NULL);
    send(m, "diff ~/a.txt\r");
    o = drain(m);
    CHECK(strstr(o, "usage: diff [-u] <file> <file>") != NULL);
}

/* sort and uniq, as the system's (C locale) order and keep them, from a
   file or from a |. */
static void test_sort_uniq(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    const char *s = "pear\napple\n10\n9\nbanana\napple\n-3\n";
    CHECK(roc_write_file(m, "test", "/home/guest/s.txt", (const uint8_t *)s, strlen(s)));
    send(m, "sort ~/s.txt\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\n-3\r\n10\r\n9\r\napple\r\napple\r\nbanana\r\npear\r\n") != NULL);
    send(m, "sort -n ~/s.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n-3\r\napple\r\napple\r\nbanana\r\npear\r\n9\r\n10\r\n") != NULL);
    send(m, "sort -ru ~/s.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\npear\r\nbanana\r\napple\r\n9\r\n10\r\n-3\r\n") != NULL);
    const char *u = "a\na\nb\nc\nc\nc\na\n";
    CHECK(roc_write_file(m, "test", "/home/guest/u.txt", (const uint8_t *)u, strlen(u)));
    send(m, "uniq -c ~/u.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n   2 a\r\n   1 b\r\n   3 c\r\n   1 a\r\n") != NULL);
    send(m, "uniq -d ~/u.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\na\r\nc\r\n") != NULL);
    send(m, "uniq -u ~/u.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nb\r\na\r\n") != NULL);
    send(m, "cat ~/u.txt | sort | uniq -c\r"); /* the classic */
    o = drain(m);
    CHECK(strstr(o, "\r\n   3 a\r\n   1 b\r\n   3 c\r\n") != NULL);
    send(m, "sort -x ~/s.txt\r");
    o = drain(m);
    CHECK(strstr(o, "sort: bad option -x") != NULL);
}

/* * ? [...] expand into the paths they match, sorted, as sh does: not
   across a /, not into dot files unless the pattern starts with a dot,
   not when quoted; a pattern that matches nothing stays as typed. */
static void test_shell_glob(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    const char *names[] = {"/home/guest/a1.txt", "/home/guest/a2.txt", "/home/guest/b.txt",
                           "/home/guest/.hidden.txt", "/home/guest/notes.md"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        CHECK(roc_write_file(m, "test", names[i], (const uint8_t *)"x\n", 2));
    }
    send(m, "mkdir ~/d\r");
    drain(m);
    CHECK(roc_write_file(m, "test", "/home/guest/d/x.txt", (const uint8_t *)"y\n", 2));
    send(m, "echo ~/a*.txt\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\n/home/guest/a1.txt /home/guest/a2.txt\r\n") != NULL);
    send(m, "cd ~; echo *.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\na1.txt a2.txt b.txt\r\n") != NULL);
    send(m, "echo .*.txt a?.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n.hidden.txt a1.txt a2.txt\r\n") != NULL);
    send(m, "echo [ab]*.txt [!a]*.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\na1.txt a2.txt b.txt b.txt\r\n") != NULL);
    send(m, "echo */x.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nd/x.txt\r\n") != NULL);
    send(m, "echo 'a*' a\\* \"b*\" zz*\r"); /* quoted, or matching nothing: as typed */
    o = drain(m);
    CHECK(strstr(o, "\r\na* a* b* zz*\r\n") != NULL);
    send(m, "x='a*.txt'; echo $x \"$x\"\r"); /* an unquoted expansion is a pattern too */
    o = drain(m);
    CHECK(strstr(o, "\r\na1.txt a2.txt a*.txt\r\n") != NULL);
    send(m, "cat a*.txt | wc\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n2 2 4\r\n") != NULL); /* cat of both, one after the other */
    send(m, "ls *.txt d\r"); /* several: the files, then the directory under its name */
    o = drain(m);
    CHECK(strstr(o, "a1.txt") != NULL && strstr(o, "b.txt") != NULL);
    CHECK(strstr(o, "\r\nd:\r\n") != NULL && strstr(o, "x.txt") != NULL);
    send(m, "wc *.md\r");
    o = drain(m);
    CHECK(strstr(o, "notes.md") != NULL);
    for (int i = 0; i < 55; i++) { /* the home holds 64 files */
        char name[64];
        (void)snprintf(name, sizeof(name), "/home/guest/d/f%03d", i);
        CHECK(roc_write_file(m, "test", name, (const uint8_t *)"", 0));
    }
    send(m, "echo d/* /*/* /*/*/* | wc\r"); /* more names than a line once held */
    o = drain(m);
    unsigned lines_n = 0;
    unsigned words_n = 0;
    const char *wc = strstr(o, "| wc\r\n");
    CHECK(wc != NULL && sscanf(wc + 6, "%u %u", &lines_n, &words_n) == 2);
    CHECK(lines_n == 1 && words_n > 100);
    h.pending = 0;
    send(m, "cat /pub/k*\r"); /* the site's files are in the tree too */
    drain(m);
    CHECK(h.pending == 1 && strcmp(h.path, "/pub/kutta.md") == 0);
    roc_feed_fail(m, h.req_id);
    drain(m);
}

static void test_repl_and_site_files(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "filo\r");
    const char *o = drain(m);
    CHECK(strstr(o, "exit or Ctrl-D leaves") != NULL);
    CHECK(strstr(o, "filo> ") != NULL);
    send(m, "(+ 1 2)\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n3\r\n") != NULL);
    send(m, "(def twice (fn (x)\r");
    o = drain(m);
    CHECK(strstr(o, "....> ") != NULL); /* an open paren waits */
    send(m, "  (* 2 x)))\r");
    drain(m);
    send(m, "(twice 21)\r"); /* def kept between lines */
    o = drain(m);
    CHECK(strstr(o, "\r\n42\r\n") != NULL);
    send(m, "(nth (list) 3)\r");
    o = drain(m);
    CHECK(strstr(o, "filo: ") != NULL);
    send(m, "(echo (pwd))\r"); /* the shell's builtins are there too */
    o = drain(m);
    CHECK(strstr(o, "\r\n/\r\n") != NULL);
    send(m, "\x04"); /* Ctrl-D: back to the shell, not out of it */
    o = drain(m);
    CHECK(!m->sc.repl && m->t.napps == 0);
    CHECK(strstr(o, "guest@shell.test") != NULL);
    send(m, "filo\r");
    send(m, "exit\r");
    o = drain(m);
    CHECK(!m->sc.repl && strstr(o, "guest@shell.test") != NULL);

    /* cp of a site file: asked of the host, written when it arrives */
    h.pending = 0;
    send(m, "cp /pub/kutta.md ~/k.md\r");
    CHECK(h.pending == 1 && strcmp(h.path, "/pub/kutta.md") == 0);
    static const char body[] = "+++\ntitle = \"Kutta\"\n+++\n# Kutta\n\nbody\n";
    roc_feed(m, h.req_id, (const uint8_t *)body, sizeof(body) - 1);
    roc_feed_eof(m, h.req_id);
    o = drain(m);
    CHECK(m->mode == ROC_MODE_LINE);
    const uint8_t *d = NULL;
    size_t n = 0;
    CHECK(ufs_find(&m->uf, "/home/guest/k.md", &d, &n) && n == sizeof(body) - 1);
    CHECK(strstr(o, "guest@shell.test") != NULL);
    /* a failed fetch says so and the prompt is back */
    h.pending = 0;
    send(m, "cp /pub/caf\xc3\xa9.md ~/c.md\r");
    CHECK(h.pending == 1);
    roc_feed_fail(m, h.req_id);
    o = drain(m);
    CHECK(strstr(o, "read error") != NULL && m->mode == ROC_MODE_LINE);
    /* edit of a site file: the editor opens on the bytes */
    h.pending = 0;
    send(m, "edit /pub/kutta.md\r");
    CHECK(h.pending == 1 && strcmp(h.path, "/pub/kutta.md") == 0);
    roc_feed(m, h.req_id, (const uint8_t *)body, sizeof(body) - 1);
    roc_feed_eof(m, h.req_id);
    drain(m);
    CHECK(m->t.napps == 1 && strcmp(m->scr.name, "edt") == 0);
    CHECK(canvas_has(&m->cmp.target, "title = \"Kutta\""));
    CHECK(canvas_has(&m->cmp.target, "/pub/kutta.md"));
    send(m, "\x13"); /* saving there is refused; save as into the home works */
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "edt: /pub/kutta.md: Read-only file system"));
    send(m, "\x1b");
    roc_tick(m, 200);
    send(m, "a~/kutta.md\r");
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "Saved /home/guest/kutta.md"));
    send(m, "\x11");
    drain(m);
    CHECK(m->t.napps == 0);
}

/* cmd > file: text into the home, escapes and \r gone, refusals on screen */
static void test_redirect(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "ls /pub > ~/l.txt\r");
    const char *o = drain(m);
    CHECK(strstr(o, "kutta.md") == NULL); /* nothing of it on the terminal */
    const uint8_t *d = NULL;
    size_t n = 0;
    CHECK(ufs_find(&m->uf, "/home/guest/l.txt", &d, &n));
    CHECK(n > 0 && memchr(d, 0x1b, n) == NULL && memchr(d, '\r', n) == NULL);
    CHECK(memmem(d, n, "kutta.md", 8) != NULL && memmem(d, n, "\n", 1) != NULL);
    send(m, "echo hi >> ~/l.txt\r");
    drain(m);
    CHECK(ufs_find(&m->uf, "/home/guest/l.txt", &d, &n));
    CHECK(n > 3 && memcmp(d + n - 3, "hi\n", 3) == 0);
    CHECK(memmem(d, n, "kutta.md", 8) != NULL);
    send(m, "echo one > ~/l.txt\r"); /* > empties first */
    drain(m);
    CHECK(ufs_find(&m->uf, "/home/guest/l.txt", &d, &n) && n == 4 && memcmp(d, "one\n", 4) == 0);
    /* a script's output too, and a reader's, which ends later */
    send(m, "tree /bin > ~/t.txt\r");
    drain(m);
    CHECK(ufs_find(&m->uf, "/home/guest/t.txt", &d, &n) && memmem(d, n, "help", 4) != NULL);
    h.pending = 0;
    send(m, "cat /pub/kutta.md > ~/k.txt\r");
    CHECK(h.pending == 1);
    static const char body[] = "# Kutta\n\nplain *body*\n";
    roc_feed(m, h.req_id, (const uint8_t *)body, sizeof(body) - 1);
    roc_feed_eof(m, h.req_id);
    o = drain(m);
    CHECK(m->mode == ROC_MODE_LINE && strstr(o, "guest@shell.test") != NULL);
    CHECK(ufs_find(&m->uf, "/home/guest/k.txt", &d, &n));
    CHECK(memmem(d, n, "Kutta", 5) != NULL && memchr(d, 0x1b, n) == NULL);
    /* refusals: the file first, then the command's own, on the terminal */
    send(m, "ls > /pub/x.txt\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: rocchetto: /pub/x.txt: Read-only file system") != NULL);
    send(m, "cat nothere > ~/e.txt\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: cat: nothere: No such file or directory") != NULL);
    CHECK(ufs_find(&m->uf, "/home/guest/e.txt", &d, &n) && n == 0);
    send(m, "ls >\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: Syntax error: newline unexpected") != NULL);
    send(m, "> ~/empty\r");
    drain(m);
    CHECK(ufs_find(&m->uf, "/home/guest/empty", &d, &n) && n == 0);
    CHECK(!m->cap.on);
}

/* pasted text reaches the editor as one insert, painted once at the end */
static void test_paste_into_editor(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "edit ~/p.txt\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\x1b[?2004h") != NULL); /* asked for while the app is up */
    send(m, "\x1b[200~");
    send(m, "one\ntwo\nthree");
    o = drain(m);
    CHECK(m->t.pasting);
    CHECK(strstr(o, "three") == NULL); /* nothing painted yet */
    send(m, "\x1b[201~");
    o = drain(m);
    CHECK(!m->t.pasting);
    CHECK(strstr(o, "three") != NULL);
    CHECK(m->ed.tb.nlines == 3);
    send(m, "\x13\x11");
    o = drain(m);
    CHECK(m->t.napps == 0 && strstr(o, "\x1b[?2004l") != NULL);
    /* a pasted ":q!" still finds the colon line: the field is placed by a
       draw that runs even while the terminal waits */
    send(m, "edit ~/p.txt\r");
    drain(m);
    send(m, "\x1b");
    roc_tick(m, 200);
    send(m, "\x1b[200~:q!\r\x1b[201~");
    drain(m);
    CHECK(m->t.napps == 0);
}

/* The site's files as a host that waits for them answers (fh_open): the
   board's kutta.md, served at once. */
static const char site_body[] = "+++\ntitle = \"Kutta\"\n+++\n# Kutta\n\nplain body\n";

static int site_open(void *ctx, const char *path, int mode, void **h, uint64_t *size) {
    (void)ctx;
    if (mode != ROC_FH_READ || strncmp(path, "/pub/", 5) != 0) {
        return ROC_HOST_NOT_MINE;
    }
    if (strcmp(path, "/pub/kutta.md") != 0) {
        return ROC_HOST_NO;
    }
    *h = (void *)site_body;
    *size = sizeof(site_body) - 1;
    return ROC_HOST_YES;
}

static long long site_read(void *ctx, void *h, uint64_t off, uint8_t *buf, size_t n) {
    (void)ctx;
    (void)h;
    size_t len = sizeof(site_body) - 1;
    if (off >= len) {
        return 0;
    }
    size_t k = len - (size_t)off < n ? len - (size_t)off : n;
    memcpy(buf, site_body + off, k);
    return (long long)k;
}

static bool site_close(void *ctx, void *h, bool commit) {
    (void)ctx;
    (void)h;
    (void)commit;
    return true;
}

/* grep, head, tail, wc: over files here, and over the site's */
static void test_filters(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    static const char w[] = "(write-file \"~/t.txt\" \"one\\nTwo fish\\nthree\\nfour\\nfive\\n\")";
    static const tree_file tree[] = {{"bin/w.filo", (const uint8_t *)w, sizeof(w) - 1}};
    tree_programs(m, tree, 1);
    send(m, "w\r");
    drain(m);
    tree_set_source(NULL, 0);

    send(m, "wc ~/t.txt\r");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\n5 6 29 /home/guest/t.txt\r\n") != NULL);
    send(m, "wc /lib/roc/warriors/imp.red\r");
    o = drain(m);
    CHECK(strstr(o, "\r\n6 24 152 /lib/roc/warriors/imp.red\r\n") != NULL);
    send(m, "head -n 2 ~/t.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\none\r\nTwo fish\r\n") != NULL && strstr(o, "three") == NULL);
    send(m, "head -2 ~/t.txt\r");
    o = drain(m);
    CHECK(strstr(o, "Two fish\r\n") != NULL && strstr(o, "three") == NULL);
    send(m, "tail -n 2 ~/t.txt\r");
    o = drain(m);
    CHECK(strstr(o, "\r\nfour\r\nfive\r\n") != NULL && strstr(o, "three") == NULL);
    send(m, "grep f ~/t.txt\r");
    o = drain(m);
    CHECK(strstr(o, "Two fish\r\nfour\r\nfive\r\n") != NULL && strstr(o, "one") == NULL);
    send(m, "grep -n -i 'two FISH' ~/t.txt\r"); /* text with a space, case folded, numbered */
    o = drain(m);
    CHECK(strstr(o, "\r\n2:Two fish\r\n") != NULL);
    send(m, "grep two ~/t.txt\r");
    o = drain(m);
    CHECK(strstr(o, "Two") == NULL); /* plain text, case exact */

    /* refusals and usage */
    send(m, "grep\r");
    o = drain(m);
    CHECK(strstr(o, "usage: grep") != NULL);
    send(m, "grep x nothere\r");
    o = drain(m);
    CHECK(strstr(o, "grep: nothere: No such file or directory") != NULL);
    send(m, "wc /pub\r");
    o = drain(m);
    CHECK(strstr(o, "wc: /pub: Is a directory") != NULL);
    send(m, "head -x ~/t.txt\r");
    o = drain(m);
    CHECK(strstr(o, "head: bad option -x") != NULL);

    /* a site file, as a host that waits for the site's gives it (the
       browser's under Asyncify): read in the middle of the line */
    m->host.fh_open = site_open;
    m->host.fh_read = site_read;
    m->host.fh_close = site_close;
    send(m, "grep -n body /pub/kutta.md\r");
    o = drain(m);
    CHECK(strstr(o, "6:plain body\r\n") != NULL);
    CHECK(m->mode == ROC_MODE_LINE && strstr(o, "guest@shell.test") != NULL);
    send(m, "wc /pub/kutta.md > ~/wc.txt\r"); /* and it redirects like anything else */
    drain(m);
    const uint8_t *d = NULL;
    size_t n = 0;
    CHECK(ufs_find(&m->uf, "/home/guest/wc.txt", &d, &n));
    CHECK(n == 21 && memcmp(d, "6 9 44 /pub/kutta.md\n", 21) == 0);
    send(m, "head -n 1 /pub/kutta.md /pub/gone.md\r");
    o = drain(m);
    CHECK(strstr(o, "==> /pub/kutta.md <==\r\n+++\r\n") != NULL);
    CHECK(strstr(o, "head: /pub/gone.md: No such file or directory") != NULL);
}

/* mars: the classics fight, the score reads like pMARS, and it redirects */
static void test_mars_command(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "mars /lib/roc/warriors/imp.red /lib/roc/warriors/dwarf.red -F 4000 -b\r");
    const char *o = drain(m);
    CHECK(strstr(o, "Imp by A. K. Dewdney scores 1\r\nDwarf by A. K. Dewdney scores 1\r\nResults: "
                    "0 0 1\r\n") != NULL);
    send(m, "mars /lib/roc/warriors/dwarf.red /lib/roc/warriors/imp.red -F 100 -b\r");
    o = drain(m);
    CHECK(strstr(o, "Dwarf by A. K. Dewdney scores 3\r\nImp by A. K. Dewdney scores 0\r\nResults: "
                    "1 0 0\r\n") != NULL);
    send(m, "mars /lib/roc/warriors/imp.red /lib/roc/warriors/dwarf.red -r 5 -b\r");
    o = drain(m);
    CHECK(strstr(o, "Results: ") != NULL);
    send(m, "mars /lib/roc/warriors/imp.red\r");
    o = drain(m);
    CHECK(strstr(o, "usage: mars") != NULL);
    send(m, "mars /lib/roc/warriors/imp.red nothere\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: mars: nothere: No such file or directory") != NULL);
    static const char bad[] = "(write-file \"~/bad.red\" \"mov x, 1\\n\")";
    static const tree_file tree[] = {{"bin/w.filo", (const uint8_t *)bad, sizeof(bad) - 1}};
    tree_programs(m, tree, 1);
    send(m, "w\r");
    drain(m);
    tree_set_source(NULL, 0);
    send(m, "mars ~/bad.red /lib/roc/warriors/imp.red\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: mars: /home/guest/bad.red: line 1: unknown label: x") != NULL);
    send(m, "mars /lib/roc/warriors/imp.red /lib/roc/warriors/dwarf.red -F 4000 > ~/s.txt\r");
    drain(m);
    const uint8_t *d = NULL;
    size_t n = 0;
    CHECK(ufs_find(&m->uf, "/home/guest/s.txt", &d, &n) &&
          memmem(d, n, "Results: 0 0 1\n", 15) != NULL);
    /* without -b the fight is on screen, and the score comes out on leaving */
    send(m, "mars /lib/roc/warriors/imp.red /lib/roc/warriors/dwarf.red -F 4000 -r 2\r");
    drain(m);
    CHECK(m->t.napps == 1 && m->cw.s.rounds == 2 && m->cw.s.fixed && m->cw.s.report);
    CHECK(canvas_has(&m->cmp.target, "round 1/2"));
    send(m, "n");
    send(m, "n");
    drain(m);
    CHECK(m->cw.s.over);
    send(m, "\x1b");
    roc_tick(m, 200);
    o = drain(m);
    CHECK(m->t.napps == 0 && m->mode == ROC_MODE_LINE);
    CHECK(strstr(o, "Imp by A. K. Dewdney scores 2\r\nDwarf by A. K. Dewdney scores 2\r\nResults: "
                    "0 0 2\r\n") != NULL);
}

/* the corewar door: the core as a map, the panel, rounds on ticks, keys */
static void test_corewar_door(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "corewar nothere\r");
    const char *o = drain(m);
    CHECK(strstr(o, "usage: corewar") != NULL);
    send(m, "corewar /lib/roc/warriors/imp.red /lib/roc/warriors/dwarf.red\r");
    o = drain(m);
    CHECK(m->t.napps == 1 && m->mode == ROC_MODE_APP);
    CHECK(canvas_has(&m->cmp.target, "Imp") && canvas_has(&m->cmp.target, "Dwarf"));
    CHECK(canvas_has(&m->cmp.target, "round 1/10"));
    CHECK(canvas_has(&m->cmp.target, "MOV.I")); /* the listing: Imp's one instruction */
    CHECK(m->cw.s.rounds == 10 && !m->cw.s.report);
    CHECK(canvas_has(&m->cmp.target, "Space pause") && canvas_has(&m->cmp.target, "M core"));
    CHECK(canvas_has(&m->cmp.target, "8000 cells  standard"));
    CHECK(canvas_has(&m->cmp.target, "0/80000")); /* the cycle line, at the start */
    CHECK(canvas_has(&m->cmp.target, "procs 1")); /* the panel counts processes */
    CHECK(canvas_has(&m->cmp.target, "0.0%"));    /* and the core each one holds */
    CHECK(canvas_has(&m->cmp.target, "undecided: it ends on processes"));
    CHECK(strstr(o, "\xe2\x96\x80") != NULL); /* the map, two cells to a line */
    /* ticks run the fight; N calls the round and the bar keeps its winner */
    int i = 0;
    while (i < 40) {
        roc_tick(m, 50);
        i++;
    }
    drain(m);
    send(m, "n");
    drain(m);
    CHECK(m->cw.s.round == 1 && m->cw.s.won[0] == -1); /* both alive: a tie */
    /* the score pMARS keeps: three for a win, one to each for a tie, and
       a round is counted once however it was called */
    CHECK(m->cw.s.res[0][1] == 1 && m->cw.s.res[0][0] == 0 && m->cw.s.res[1][0] == 0);
    CHECK(canvas_has(&m->cmp.target, "score 1"));
    send(m, "n");
    drain(m);
    CHECK(m->cw.s.round == 2 && m->cw.s.res[0][1] == 2);
    CHECK(canvas_has(&m->cmp.target, "score 2"));
    CHECK(canvas_has(&m->cmp.target, "procs"));        /* both still running */
    CHECK(m->cw.s.held[0] > 0 && m->cw.s.held[1] > 0); /* what each one had taken */
    send(m, " ");                                      /* pause */
    uint32_t r = m->cw.s.round;
    i = 0;
    while (i < 40) {
        roc_tick(m, 50);
        i++;
    }
    CHECK(m->cw.s.round == r && m->cw.s.paused);
    send(m, " ");
    send(m, "+");
    CHECK(m->cw.s.speed == 3);
    /* a shorter round: the cycle limit falls and the match starts over */
    send(m, "+");
    send(m, "c");
    drain(m);
    CHECK(m->cw.s.limit == 1 && m->cw.s.round == 0 && m->cw.s.speed == 4);
    CHECK(canvas_has(&m->cmp.target, "round ends at 40000 cycles"));
    i = 0;
    while (i < 80 && m->cw.s.round == 0) {
        roc_tick(m, 50);
        i++;
    }
    drain(m);
    CHECK(m->cw.s.round >= 1); /* the round ended on its own, on ticks */
    /* a smaller core: both warriors are assembled again for it */
    send(m, "m");
    drain(m);
    CHECK(m->cw.s.size == 1 && m->cw.s.round == 0);
    CHECK(canvas_has(&m->cmp.target, "800 cells  tiny"));
    send(m, "r");
    drain(m);
    CHECK(m->cw.s.round == 0 && m->cw.s.points[0] == 0 && m->cw.s.points[1] == 0);
    send(m, "\x1b");
    roc_tick(m, 200);
    o = drain(m);
    CHECK(m->t.napps == 0 && m->mode == ROC_MODE_LINE);
    /* named warriors, from the home too */
    send(m, "cp /lib/roc/warriors/imp.red ~/i.red\r");
    send(m, "corewar ~/i.red /lib/roc/warriors/dwarf.red\r");
    drain(m);
    CHECK(m->t.napps == 1);
    send(m, "q");
    drain(m);
    CHECK(m->t.napps == 0);
    /* a core the warrior no longer fits in: refused, and the footer says why */
    static const char big[] =
        "(write-file \"~/big.red\" \";redcode\\n;name Big\\n"
        "mov 0, 1\\nmov 0, 1\\nmov 0, 1\\nmov 0, 1\\nmov 0, 1\\nmov 0, 1\\n\")";
    static const tree_file tree[] = {{"bin/w.filo", (const uint8_t *)big, sizeof(big) - 1}};
    tree_programs(m, tree, 1);
    send(m, "w\r");
    drain(m);
    tree_set_source(NULL, 0);
    send(m, "corewar ~/big.red /lib/roc/warriors/dwarf.red\r");
    drain(m);
    CHECK(m->t.napps == 1 && m->cw.s.size == 0);
    send(m, "m"); /* tiny: six instructions still fit */
    drain(m);
    CHECK(m->cw.s.size == 1);
    send(m, "m"); /* nano: five at most, so the core stays as it was */
    drain(m);
    CHECK(m->cw.s.size == 1);
    CHECK(canvas_has(&m->cmp.target, "nano core: big.red: "));
    CHECK(canvas_has(&m->cmp.target, "warrior too long"));
    send(m, "q");
    drain(m);
    CHECK(m->t.napps == 0);
}

/* the screen that picks the warriors: the shell's, yours, and the editor */
static void test_corewar_pick(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "cp /lib/roc/warriors/imp.red ~/mine.red\r");
    drain(m);
    send(m, "corewar\r"); /* no files named: the pick comes first */
    const char *o = drain(m);
    (void)o;
    CHECK(m->t.napps == 1 && m->mode == ROC_MODE_APP);
    CHECK(canvas_has(&m->cmp.target, "Core War"));
    CHECK(canvas_has(&m->cmp.target, "1  imp.red")); /* the shell's two, ready */
    CHECK(canvas_has(&m->cmp.target, "2  dwarf.red"));
    CHECK(canvas_has(&m->cmp.target, "mine.red") && canvas_has(&m->cmp.target, "yours"));
    CHECK(canvas_has(&m->cmp.target, "classic"));
    send(m, "c\r");   /* the list is yours to build */
    send(m, "+ 6\r"); /* the shell's five come first, so yours is the sixth */
    drain(m);
    CHECK(m->cw.s.n == 1 && strcmp(m->cw.s.path[0], "/home/guest/mine.red") == 0);
    send(m, "+ 1\r");
    drain(m);
    CHECK(m->cw.s.n == 2 && strncmp(m->cw.s.path[1], "/lib/roc/warriors/", 18) == 0);
    send(m, "+ 99\r"); /* nothing there, and the list stands */
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "no warrior with that number") && m->cw.s.n == 2);
    send(m, "f\r"); /* fight: the door, over the screen */
    drain(m);
    CHECK(m->t.napps == 2 && canvas_has(&m->cmp.target, "round 1/10"));
    send(m, "\x1b");
    roc_tick(m, 200); /* a lone ESC settles on the next tick */
    o = drain(m);
    CHECK(m->t.napps == 1 && canvas_has(&m->cmp.target, "Core War"));
    CHECK(strstr(o, "\x1b[?25h") != NULL);         /* the arena hands the cursor back */
    CHECK(canvas_has(&m->cmp.target, "mine.red")); /* the list survived the fight */
    send(m, "e 6\r");                              /* edit it: the editor takes the screen */
    o = drain(m);
    CHECK(m->t.napps == 1 && canvas_has(&m->cmp.target, "mine.red"));
    CHECK(canvas_has(&m->cmp.target, "Esc menu"));
    CHECK(strstr(o, "\x1b[?25h") != NULL); /* and the editor's caret is on */
    send(m, "\x11");                       /* ^Q: and the editor comes back to the pick */
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "Core War"));
    CHECK(m->cw.s.n == 2 && strcmp(m->cw.s.path[0], "/home/guest/mine.red") == 0);
    send(m, "h\r"); /* the manual, over the pick screen */
    o = drain(m);   /* the pager writes text, not cells */
    CHECK(m->t.napps == 2);
    CHECK(strstr(o, "Core War is a game") != NULL);
    CHECK(strstr(o, "redcode.md") != NULL); /* the pager's own status line */
    send(m, "q");                           /* and back to the pick, with the choice kept */
    drain(m);
    CHECK(m->t.napps == 1 && canvas_has(&m->cmp.target, "mine.red"));
    send(m, "n dwarf2\r"); /* a new one, in the home, named by you */
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "dwarf2.red"));
    /* the manual from inside the editor, and back to the same text */
    send(m, "\x1b");  /* Esc opens the menu */
    roc_tick(m, 200); /* a lone ESC settles on the next tick */
    send(m, "h");
    o = drain(m);
    CHECK(m->t.napps == 2 && strstr(o, "Core War is a game") != NULL);
    send(m, "q");
    drain(m);
    CHECK(m->t.napps == 1 && canvas_has(&m->cmp.target, "dwarf2.red"));
    send(m, "\x11");
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "Core War"));
}

/* more than two in the core: the command takes a list, the pick screen
   builds one, and a round is shared the way pMARS shares it */
static void test_corewar_many(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);
    send(m, "mars /lib/roc/warriors/imp.red /lib/roc/warriors/dwarf.red "
            "/lib/roc/warriors/sweeper.red -r 2 -b\r");
    const char *o = drain(m);
    CHECK(strstr(o, "Imp by A. K. Dewdney scores ") != NULL);
    CHECK(strstr(o, "Sweeper by crg.eti.br scores ") != NULL);
    CHECK(strstr(o, "  Results:") != NULL); /* the breakdown pMARS prints */
    send(m, "corewar /lib/roc/warriors/imp.red /lib/roc/warriors/dwarf.red "
            "/lib/roc/warriors/sweeper.red\r");
    drain(m);
    CHECK(m->t.napps == 1 && m->cw.s.n == 3);
    CHECK(canvas_has(&m->cmp.target, "3 warriors")); /* no room for the names up there */
    CHECK(canvas_has(&m->cmp.target, "Sweeper"));    /* the panel names all three */
    send(m, "n");                                    /* called with all three alive */
    drain(m);
    CHECK(m->cw.s.round == 1 && m->cw.s.won[0] == -1);
    CHECK(m->cw.s.points[0] == 2 && m->cw.s.points[1] == 2 && m->cw.s.points[2] == 2);
    CHECK(m->cw.s.res[0][2] == 1); /* shared with two others */
    send(m, "q");
    drain(m);
    CHECK(m->t.napps == 0);
    /* and the pick screen carries the list on, up to eight */
    send(m, "corewar\r");
    drain(m);
    CHECK(m->cw.s.n == 3 && canvas_has(&m->cmp.target, "sweeper.red"));
    send(m, "+ 4\r");
    drain(m);
    CHECK(m->cw.s.n == 4);
    send(m, "f\r");
    drain(m);
    CHECK(m->t.napps == 2 && canvas_has(&m->cmp.target, "4 warriors"));
    send(m, "q");
    drain(m);
    CHECK(m->t.napps == 1);
}

/* ---- pager ---- */

static void open_pager_path(roc *m, mock_host *h, const char *path, const char *body) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "less %s\r", path);
    send(m, cmd);
    drain(m);
    CHECK(h->pending == 1);
    h->pending = 0;
    roc_feed(m, h->req_id, (const uint8_t *)body, strlen(body));
    roc_feed_eof(m, h->req_id);
}

/* .txt keeps the body verbatim, which is what the wrapping tests want to
   measure; .md goes through the markdown renderer. */
static void open_pager(roc *m, mock_host *h, const char *body) {
    open_pager_path(m, h, "/readme.txt", body);
}

static void test_pager_basics(void) {
    mock_host h;
    roc *m = boot(&h);
    open_pager(m, &h, "+++\ntitle = \"secret\"\n+++\nline one\nline two");
    const char *o = drain(m);
    CHECK(strstr(o, "\x1b[?1049h") != NULL); /* alt screen on */
    CHECK(strstr(o, "line one") != NULL);
    CHECK(strstr(o, "secret") == NULL);     /* front matter stripped here too */
    CHECK(strstr(o, "readme.txt") != NULL); /* the name sits in the status bar */
    CHECK(strstr(o, "100%") != NULL);
    send(m, "q");
    o = drain(m);
    CHECK(strstr(o, "\x1b[?1049l") != NULL); /* back to main buffer */
    CHECK(strstr(o, "$ ") != NULL);
    send(m, "pwd\r");
    o = drain(m);
    CHECK(strstr(o, "/\r\n") != NULL); /* shell is alive again */
}

static const char *many_lines(void) {
    static char body[4096];
    size_t off = 0;
    for (int i = 0; i < 60; i++) {
        off += (size_t)snprintf(body + off, sizeof(body) - off, "linha %02d\n", i);
    }
    return body;
}

static void test_pager_scrolling(void) {
    mock_host h;
    roc *m = boot(&h);
    open_pager(m, &h, many_lines()); /* 60 lines on a 24-row screen */
    const char *o = drain(m);
    CHECK(strstr(o, "linha 00") != NULL);
    CHECK(strstr(o, "linha 59") == NULL);
    send(m, "j"); /* one line down */
    o = drain(m);
    CHECK(strstr(o, "linha 01") != NULL);
    CHECK(strstr(o, "linha 00") == NULL);
    send(m, "G"); /* bottom */
    o = drain(m);
    CHECK(strstr(o, "linha 59") != NULL);
    CHECK(strstr(o, "100%") != NULL);
    send(m, "g"); /* top */
    o = drain(m);
    CHECK(strstr(o, "linha 00") != NULL);
    send(m, "\x1b[6~"); /* PgDn */
    o = drain(m);
    CHECK(strstr(o, "linha 22") != NULL);
    send(m, "\x1b[B"); /* arrow down = +1 */
    o = drain(m);
    CHECK(strstr(o, "linha 23") != NULL);
    send(m, "\x1b"); /* bare ESC quits */
    roc_tick(m, ROC_ESC_TIMEOUT_MS);
    o = drain(m);
    CHECK(strstr(o, "\x1b[?1049l") != NULL);
}

static void test_pager_wrap_and_resize(void) {
    mock_host h;
    roc *m = boot(&h);
    /* one long line must wrap on an 80-col screen */
    static char body[512];
    memset(body, 'x', 200);
    memcpy(body + 100, " y", 2); /* a break opportunity */
    body[200] = '\0';
    open_pager(m, &h, body);
    drain(m);
    CHECK(m->pg.nrows >= 3);
    size_t rows_at_80 = m->pg.nrows;
    roc_resize(m, 40, 24);
    drain(m);
    CHECK(m->pg.nrows > rows_at_80); /* narrower means more wrapped rows */
    roc_resize(m, 80, 24);
    send(m, "q");
    drain(m);
}

static void test_pager_wide_runes_wrap(void) {
    mock_host h;
    roc *m = boot(&h);
    /* 30 CJK runes = 60 columns; on a 40-col screen they must wrap into 2 rows */
    static char body[256];
    size_t off = 0;
    for (int i = 0; i < 30; i++) {
        memcpy(body + off, "\xe6\x97\xa5", 3);
        off += 3;
    }
    body[off] = '\0';
    roc_resize(m, 40, 24);
    open_pager(m, &h, body);
    drain(m);
    CHECK(m->pg.nrows == 2);
    CHECK(m->pg.rows[0].len == 60); /* 20 runes x 3 bytes fill exactly 40 columns */
    send(m, "q");
    drain(m);
    roc_resize(m, 80, 24);
}

/* ---- markdown ---- */

static const char *cat_md(roc *m, mock_host *h, const char *body) {
    send(m, "cat /pub/kutta.md\r");
    drain(m);
    h->pending = 0;
    roc_feed(m, h->req_id, (const uint8_t *)body, strlen(body));
    roc_feed_eof(m, h->req_id);
    return drain(m);
}

static void test_md_inline(void) {
    mock_host h;
    roc *m = boot(&h);
    const char *o = cat_md(m, &h, "# Title\n\nA **bold** and *it* and `code` word.\n");
    CHECK(strstr(o, "\x1b[0;1;36;4mTitle") != NULL); /* h1: bold, cyan, underlined */
    CHECK(strstr(o, "\x1b[0;1mbold") != NULL);
    CHECK(strstr(o, "\x1b[0;3mit") != NULL);
    CHECK(strstr(o, "\x1b[0;33mcode") != NULL);

    /* an identifier is not emphasis, and arithmetic is not either */
    o = cat_md(m, &h, "snake_case_here and 2 * 3 * 4\n");
    CHECK(strstr(o, "snake_case_here") != NULL);
    CHECK(strstr(o, "\x1b[0;3m") == NULL);

    /* blocks */
    o = cat_md(m, &h, "- one\n2. two\n> quoted\n---\n");
    CHECK(strstr(o, "\xe2\x80\xa2") != NULL); /* bullet */
    CHECK(strstr(o, "2.") != NULL);
    CHECK(strstr(o, "\xe2\x94\x82") != NULL);             /* quote bar */
    CHECK(strstr(o, "\xe2\x94\x80\xe2\x94\x80") != NULL); /* rule */
}

static void test_md_links(void) {
    mock_host h;
    roc *m = boot(&h);
    const char *o = cat_md(m, &h, "[abs](https://example.com/x) [rel](/pub/) [none](foo.md)\n");
    CHECK(strstr(o, "\x1b]8;;https://example.com/x\x1b\\") != NULL);
    /* a site-relative target has no base in a terminal: give it one */
    CHECK(strstr(o, "\x1b]8;;https://shell.test/pub/\x1b\\") != NULL);
    CHECK(strstr(o, "\x1b]8;;\x1b\\") != NULL); /* every link is closed */
    /* a target that cannot be linked is not dressed up as one */
    const char *none = strstr(o, "none");
    CHECK(none != NULL && strstr(o, "\x1b]8;;foo.md") == NULL);
    o = cat_md(m, &h, "![alt](https://example.com/i.png)\n");
    CHECK(strstr(o, "[img] alt") != NULL);
}

/* Hugo shortcodes are not markdown, but 56 of the site's pages carry them;
   raw template syntax on screen is worse than nothing. */
static void test_md_shortcodes(void) {
    mock_host h;
    roc *m = boot(&h);
    const char *o = cat_md(m, &h, "{{< youtube abc123 >}}\n");
    CHECK(strstr(o, "\x1b]8;;https://www.youtube.com/watch?v=abc123\x1b\\") != NULL);
    CHECK(strstr(o, "video") != NULL);
    CHECK(strstr(o, "{{<") == NULL);

    o = cat_md(m, &h, "{{< anim src=\"a.webp\" href=\"https://example.com/r\" alt=\"Kutta\" >}}\n");
    CHECK(strstr(o, "\x1b]8;;https://example.com/r\x1b\\") != NULL);
    CHECK(strstr(o, "[anim] Kutta") != NULL);

    /* ref names a page by slug; the index knows which section it lives in */
    o = cat_md(m, &h, "see [it]({{< ref \"kutta\" >}}) here\n");
    CHECK(strstr(o, "\x1b]8;;https://shell.test/pub/kutta/\x1b\\") != NULL);
    o = cat_md(m, &h, "see [it]({{< ref \"nothing-indexed\" >}}) here\n");
    CHECK(strstr(o, "\x1b]8;;") == NULL); /* unresolvable: plain text, not a bad link */

    o = cat_md(m, &h, "a {{< mystery x=1 >}} b\n");
    CHECK(strstr(o, "mystery") == NULL);
    CHECK(strstr(o, "a  b") != NULL);
}

static void test_md_code_fence(void) {
    mock_host h;
    roc *m = boot(&h);
    const char *o = cat_md(m, &h, "```go\nfunc f() { s := \"x\" } // c\n```\n");
    CHECK(strstr(o, "\x1b[0;36mfunc") != NULL);  /* keyword */
    CHECK(strstr(o, "\x1b[0;32m\"x\"") != NULL); /* string */
    CHECK(strstr(o, "\x1b[0;2m// c") != NULL);   /* comment */
    CHECK(strstr(o, "```") == NULL);             /* the fence takes no row */

    /* a block comment survives the newline */
    o = cat_md(m, &h, "```c\n/* one\ntwo */ int x;\n```\n");
    CHECK(strstr(o, "\x1b[0;2m/* one") != NULL);
    CHECK(strstr(o, "\x1b[0;2mtwo */") != NULL);
    CHECK(strstr(o, "\x1b[0;36mint") != NULL);

    /* a fence inside a list item is indented; missing that left a stray
       backtick on screen where the fence line was */
    o = cat_md(m, &h, "1. step:\n   ```go\n   func f() {}\n   ```\n   done\n");
    CHECK(strstr(o, "`") == NULL);
    CHECK(strstr(o, "\x1b[0;36mfunc") != NULL);
    CHECK(strstr(o, "done") != NULL);

    /* an unlabeled fence carries terminal output more often than code */
    o = cat_md(m, &h, "```\nfunc select from where\n```\n");
    CHECK(strstr(o, "\x1b[0;36m") == NULL);
}

static void test_md_only_for_markdown(void) {
    mock_host h;
    roc *m = boot(&h);
    send(m, "cat /readme.txt\r");
    drain(m);
    h.pending = 0;
    const char *body = "# not a heading\n**not bold**\n";
    roc_feed(m, h.req_id, (const uint8_t *)body, strlen(body));
    roc_feed_eof(m, h.req_id);
    const char *o = drain(m);
    CHECK(strstr(o, "# not a heading") != NULL);
    CHECK(strstr(o, "**not bold**") != NULL);
}

/* A file must never drive the terminal: the renderer is the only thing that
   emits escapes, so escape-shaped bytes in the source are dropped. */
static void test_md_strips_source_escapes(void) {
    mock_host h;
    roc *m = boot(&h);
    const char *o = cat_md(m, &h, "before \x1b[31mred\x1b[0m and \x1b]0;title\x07 after\n");
    CHECK(strstr(o, "\x1b[31m") == NULL);
    CHECK(strstr(o, "before red and") != NULL);
    CHECK(strstr(o, "title") == NULL); /* an OSC payload is part of the escape */
    CHECK(strstr(o, "after") != NULL);
}

/* The pager wraps rendered ANSI: escapes take no columns, and a row that
   starts inside a style or a hyperlink has to reopen it. A dangling OSC 8
   would turn the rest of the screen into a link. */
static void test_md_pager_wraps_styles(void) {
    mock_host h;
    roc *m = boot(&h);
    roc_resize(m, 20, 24);
    open_pager_path(m, &h, "/pub/kutta.md",
                    "**aaaa bbbb cccc dddd eeee ffff** tail\n"
                    "[llll mmmm nnnn oooo pppp](https://example.com/) rest\n");
    const char *o = drain(m);
    const pager *p = &m->pg;
    CHECK(p->nrows >= 4); /* both lines wrapped */

    /* every row that starts mid-style records it, and paint reopens it */
    size_t styled = 0;
    size_t linked = 0;
    for (size_t i = 0; i < p->nrows; i++) {
        if (p->rows[i].sgr > 0) {
            styled++;
        }
        if (p->rows[i].link > 0) {
            linked++;
        }
    }
    CHECK(styled > 0);
    CHECK(linked > 0); /* the link text spans more than one row */

    /* no hyperlink is ever left dangling on the painted screen */
    bool open = false;
    size_t opens = 0;
    for (const char *c = o; (c = strstr(c, "\x1b]8;;")) != NULL; c += 5) {
        if (c[5] == '\x1b') {
            open = false;
            continue;
        }
        CHECK(!open); /* an open while one is already open means a leak */
        open = true;
        opens++;
    }
    CHECK(opens > 0);
    CHECK(!open);
    send(m, "q");
    drain(m);
    roc_resize(m, 80, 24);
}

/* A wrapped list item hangs under its text, a wrapped paragraph does not,
   and an indented line keeps its indent. */
static void test_md_pager_hangs_lists(void) {
    mock_host h;
    roc *m = boot(&h);
    roc_resize(m, 20, 24);
    open_pager_path(m, &h, "/pub/kutta.md",
                    "- aaaa bbbb cccc dddd eeee\n"
                    "12. aaaa bbbb cccc dddd\n"
                    "aaaa bbbb cccc dddd eeee\n"
                    "```\n    aaaa bbbb cccc dddd\n```\n");
    const char *o = drain(m);
    const pager *p = &m->pg;
    CHECK(p->nrows >= 8);
    CHECK(p->rows[0].indent == 0 && p->rows[1].indent == 2); /* "• " */
    CHECK(p->rows[2].indent == 0 && p->rows[3].indent == 4); /* "12. " */
    CHECK(p->rows[4].indent == 0 && p->rows[5].indent == 0); /* a paragraph */
    CHECK(p->rows[6].indent == 0 && p->rows[7].indent == 4); /* code's own */
    CHECK(strstr(o, "\x1b[0m  ") != NULL);
    send(m, "q");
    drain(m);
    roc_resize(m, 80, 24);
}

/* ---- menu ---- */

/* The Filo snake keeps its state in the canvas: cell (x, y) of the field is
   row y + 2, column 2x + 1, and a segment's tag is its direction plus one. */
static double scr_num(roc *m, const char *name) {
    filo_value v = {0};
    CHECK(filo_get_global(&m->scr.ctx, name, &v));
    return v.u.num;
}

/* ---- canvas ---- */

static cv_text light_box[8];
static cv_text no_bottom[8];

static void canvas_test_setup(void) {
    const char *const light[8] = {
        "\xe2\x94\x8c", "\xe2\x94\x80", "\xe2\x94\x90", "\xe2\x94\x82",
        "\xe2\x94\x98", "\xe2\x94\x80", "\xe2\x94\x94", "\xe2\x94\x82",
    };
    const char *const open_bottom[8] = {"+", "-", "+", "|", "", "", "", "|"};
    for (size_t i = 0; i < 8; i++) {
        light_box[i] = cv_cstr(light[i]);
        no_bottom[i] = cv_cstr(open_bottom[i]);
    }
}

static uint32_t cell_at(const canvas *c, uint16_t row, uint16_t col) {
    return c->cells[row][col].cp;
}

static canvas CV; /* the grids are far too large for the stack */
static canvas CV2;

static void test_canvas_draws(void) {
    cv_reset(&CV, 6, 20);
    CHECK(cv_put(&CV, 1, 2, cv_cstr("hi")) == 2);
    CHECK(cell_at(&CV, 1, 2) == 'h');
    CHECK(cell_at(&CV, 1, 3) == 'i');

    /* fill and the box edges repeat the string; a box is only its border */
    cv_fill(&CV, 3, 0, 1, 5, cv_cstr("-="));
    CHECK(cell_at(&CV, 3, 0) == '-');
    CHECK(cell_at(&CV, 3, 1) == '=');
    CHECK(cell_at(&CV, 3, 4) == '-');

    cv_reset(&CV, 6, 20);
    cv_box(&CV, 0, 0, 3, 4, light_box);
    CHECK(cell_at(&CV, 0, 0) == 0x250C);
    CHECK(cell_at(&CV, 0, 1) == 0x2500);
    CHECK(cell_at(&CV, 0, 3) == 0x2510);
    CHECK(cell_at(&CV, 1, 0) == 0x2502);
    CHECK(cell_at(&CV, 1, 3) == 0x2502);
    CHECK(cell_at(&CV, 1, 1) == ' '); /* the inside is left alone */
    CHECK(cell_at(&CV, 2, 0) == 0x2514);
    CHECK(cell_at(&CV, 2, 3) == 0x2518);

    /* an empty element leaves that part of the border untouched, which is
       how a box opens at the bottom or sits on top of art */
    cv_reset(&CV, 6, 20);
    cv_put(&CV, 2, 1, cv_cstr("xx"));
    cv_box(&CV, 0, 0, 3, 4, no_bottom);
    CHECK(cell_at(&CV, 0, 0) == '+');
    CHECK(cell_at(&CV, 2, 1) == 'x');
    CHECK(cell_at(&CV, 2, 0) == ' ');

    /* a double-width rune owns two cells, and writing over either half
       clears the other so no stray half is left behind */
    cv_reset(&CV, 4, 10);
    CHECK(cv_put(&CV, 0, 0, cv_cstr("\xe6\x97\xa5")) == 2);
    CHECK(cell_at(&CV, 0, 0) == 0x65E5);
    CHECK(cell_at(&CV, 0, 1) == 0);
    cv_put(&CV, 0, 1, cv_cstr("a"));
    CHECK(cell_at(&CV, 0, 0) == ' ');
    CHECK(cell_at(&CV, 0, 1) == 'a');
}

static void test_canvas_clips(void) {
    cv_reset(&CV, 4, 6);
    cv_put(&CV, -1, 0, cv_cstr("off"));  /* above the top */
    cv_put(&CV, 9, 0, cv_cstr("off"));   /* below the bottom */
    cv_put(&CV, 0, -2, cv_cstr("abcd")); /* starts left of the edge */
    CHECK(cell_at(&CV, 0, 0) == 'c');
    CHECK(cell_at(&CV, 0, 1) == 'd');
    cv_put(&CV, 1, 4, cv_cstr("abcd")); /* runs off the right edge */
    CHECK(cell_at(&CV, 1, 4) == 'a');
    CHECK(cell_at(&CV, 1, 5) == 'b');

    /* a wide rune must not be split by the right edge */
    cv_put(&CV, 2, 5, cv_cstr("\xe6\x97\xa5"));
    CHECK(cell_at(&CV, 2, 5) == ' ');

    cv_reset(&CV2, 2, 3);
    cv_put(&CV2, 0, 0, cv_cstr("abc"));
    cv_put(&CV2, 1, 0, cv_cstr("def"));
    cv_blit(&CV, 3, 4, &CV2); /* only the part that fits lands */
    CHECK(cell_at(&CV, 3, 4) == 'a');
    CHECK(cell_at(&CV, 3, 5) == 'b');
}

/* The flush is the reason the grid exists: what reaches the terminal must be
   only the cells that changed, or an animated screen would cost a full
   repaint per frame on every viewer's connection. */
static void test_canvas_flush_is_incremental(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    drain(m);

    cv_reset(&m->cmp.shown, 4, 10);
    cv_reset(&m->cmp.work, 4, 10);
    cv_put(&m->cmp.work, 0, 0, cv_cstr("hello"));
    cv_flush(&m->t, &m->cmp.shown, &m->cmp.work);
    const char *o = drain(m);
    CHECK(strstr(o, "hello") != NULL);

    /* nothing changed: no cursor move and no text, only the pen bookends */
    cv_flush(&m->t, &m->cmp.shown, &m->cmp.work);
    o = drain(m);
    CHECK(strstr(o, "hello") == NULL);
    CHECK(strstr(o, "H") == NULL);

    /* one cell changed: one cursor move and one glyph */
    cv_put(&m->cmp.work, 2, 3, cv_cstr("X"));
    cv_flush(&m->t, &m->cmp.shown, &m->cmp.work);
    o = drain(m);
    CHECK(strstr(o, "\x1b[3;4H") != NULL);
    CHECK(strstr(o, "X") != NULL);
    CHECK(strstr(o, "hello") == NULL);
    CHECK(strlen(o) < 32);

    /* a size change is a full repaint, so the old screen cannot survive */
    cv_reset(&m->cmp.work, 5, 12);
    cv_put(&m->cmp.work, 0, 0, cv_cstr("again"));
    cv_flush(&m->t, &m->cmp.shown, &m->cmp.work);
    o = drain(m);
    CHECK(strstr(o, "\x1b[2J") != NULL);
    CHECK(strstr(o, "again") != NULL);
}

/* Colour 255 is a real palette entry — the top of the grey ramp, and where
   quantised white lands. It used to share its value with "no colour of its
   own", so art painted in it reached the terminal as the default and left
   holes wherever the picture was brightest. */
static void test_canvas_keeps_colour_255(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    drain(m);

    cv_reset(&m->cmp.shown, 2, 8);
    cv_reset(&m->cmp.work, 2, 8);
    cv_pen(&m->cmp.work, 255, 255, 0);
    cv_put(&m->cmp.work, 0, 0, cv_cstr("W"));
    cv_pen(&m->cmp.work, CV_COLOR_DEFAULT, CV_COLOR_DEFAULT, 0);
    cv_put(&m->cmp.work, 1, 0, cv_cstr("D"));
    cv_flush(&m->t, &m->cmp.shown, &m->cmp.work);
    const char *o = drain(m);
    CHECK(strstr(o, "38;5;255") != NULL);
    CHECK(strstr(o, "48;5;255") != NULL);

    /* and the cell that really has no colour still says nothing about one */
    const char *d = strstr(o, "\x1b[2;1H");
    CHECK(d != NULL);
    CHECK(strstr(d, "38;5;") == NULL);
}

static bool canvas_same(const canvas *a, const canvas *b) {
    if (a->rows != b->rows || a->cols != b->cols) {
        return false;
    }
    uint16_t y = 0;
    while (y < a->rows) {
        uint16_t x = 0;
        while (x < a->cols) {
            const cv_cell *p = &a->cells[y][x];
            const cv_cell *q = &b->cells[y][x];
            if (p->cp != q->cp || p->fg != q->fg || p->bg != q->bg || p->attr != q->attr) {
                printf("       first difference at row %u col %u\n", y, x);
                return false;
            }
            x++;
        }
        y++;
    }
    return true;
}

/* What makes a diff honest: replaying the emitted bytes onto a terminal that
   already holds the previous frame must reproduce the canvas, cell for cell.
   A cursor or pen slip that the byte-level checks miss shows up here. */
static void test_canvas_flush_replays_to_the_same_screen(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    drain(m);

    cv_reset(&m->cmp.shown, 6, 24);
    cv_reset(&m->cmp.work, 6, 24);
    cv_reset(&CV, 6, 24); /* CV stands in for the terminal */

    cv_put(&m->cmp.work, 0, 0, cv_cstr("header"));
    cv_pen(&m->cmp.work, 3, 4, CV_A_BOLD);
    cv_put(&m->cmp.work, 2, 4, cv_cstr("colored"));
    cv_pen_reset(&m->cmp.work);
    cv_box(&m->cmp.work, 3, 0, 3, 8, light_box);
    cv_put(&m->cmp.work, 4, 20, cv_cstr("\xe6\x97\xa5"));
    cv_flush(&m->t, &m->cmp.shown, &m->cmp.work);
    cv_capture cap;
    cv_capture_begin(&cap, &CV);
    const char *o = drain(m);
    cv_capture_feed(&cap, (const uint8_t *)o, strlen(o));
    CHECK(canvas_same(&CV, &m->cmp.work));

    /* a few cells change: only the difference goes out, and the terminal
       still ends up holding the whole screen */
    cv_put(&m->cmp.work, 0, 3, cv_cstr("DER"));
    cv_pen(&m->cmp.work, 5, CV_COLOR_DEFAULT, 0);
    cv_put(&m->cmp.work, 2, 4, cv_cstr("CHANGED"));
    cv_pen_reset(&m->cmp.work);
    cv_put(&m->cmp.work, 4, 20, cv_cstr("ab")); /* over both halves of the wide rune */
    cv_flush(&m->t, &m->cmp.shown, &m->cmp.work);
    cv_capture_begin(&cap, &CV);
    o = drain(m);
    CHECK(strlen(o) < 128); /* a diff, not a repaint */
    cv_capture_feed(&cap, (const uint8_t *)o, strlen(o));
    CHECK(canvas_same(&CV, &m->cmp.work));
}

/* ---- screens: a Filo program paints, the shell emits ---- */

static const char *const draw_src =
    "(do"
    "  (fg 3)"
    "  (print-at 0 0 \"hello\")"
    "  (fg C_DEFAULT)"
    "  (box 2 0 3 10 (list \"+\" \"-\" \"+\" \"|\" \"+\" \"-\" \"+\" \"|\"))"
    "  (print-at (- H 1) 0 (str-fmt \"%d x %d\" W H)))";

static bool set_draw(roc *m, const char *src) {
    return screen_set_draw(m, (const uint8_t *)src, strlen(src));
}

static bool scr_bool(roc *m, const char *name) {
    filo_value v = {0};
    CHECK(filo_get_global(&m->scr.ctx, name, &v));
    return v.u.b;
}

/* A tree read from disk, as the build left it under build/. */
static uint8_t cp_bytes[1U << 20U];
static tree_file cp_files[128];
static size_t cp_n = 0;
static char cp_paths[128][TREE_PATH_MAX];
static size_t cp_used = 0;

static void cp_walk(const char *dir, const char *prefix) {
    DIR *d = opendir(dir);
    if (d == NULL) {
        return;
    }
    const struct dirent *e = NULL;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char full[512];
        char rel[TREE_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        snprintf(rel, sizeof(rel), "%s%s", prefix, e->d_name);
        DIR *sub = opendir(full);
        if (sub != NULL) {
            closedir(sub);
            char deeper[TREE_PATH_MAX];
            snprintf(deeper, sizeof(deeper), "%s/", rel);
            cp_walk(full, deeper);
            continue;
        }
        FILE *f = fopen(full, "rb");
        if (f == NULL || cp_n >= 128) {
            if (f != NULL) {
                fclose(f);
            }
            continue;
        }
        size_t n = fread(cp_bytes + cp_used, 1, sizeof(cp_bytes) - cp_used, f);
        fclose(f);
        snprintf(cp_paths[cp_n], TREE_PATH_MAX, "%s", rel);
        cp_files[cp_n].path = cp_paths[cp_n];
        cp_files[cp_n].data = cp_bytes + cp_used;
        cp_files[cp_n].len = n;
        cp_n++;
        cp_used += n;
    }
    closedir(d);
}

static void cp_load(void) {
    cp_n = 0;
    cp_used = 0;
    cp_walk("build/cardputer-tree", "");
    cp_walk("build/cardputer-units", "");
    CHECK(cp_n > 0);
}

/* The tree carries every Filo screen twice, as source and as the unit
   compiled from it (a member of the bundle), and the shell runs the unit. Each one must give what
   its source gives: the same cells after the first paint and after a key,
   the same error when there is one. */
static canvas from_source;
static tree_file sources_only[512];

static bool compose_step(roc *m, const char *name, canvas *out, char *err, size_t cap) {
    bool ok = screen_compose(m, name);
    if (ok) {
        ok = screen_key(m, FT_KEY_DOWN);
        drain(m);
    }
    *out = m->cmp.target;
    snprintf(err, cap, "%s", screen_error(m));
    return ok;
}

static bool same_canvas(const canvas *a, const canvas *b) {
    if (a->rows != b->rows || a->cols != b->cols) {
        return false;
    }
    for (uint16_t y = 0; y < a->rows; y++) {
        if (memcmp(a->cells[y], b->cells[y], sizeof(cv_cell) * a->cols) != 0) {
            return false;
        }
    }
    return true;
}

/* The screens of a tree (all, with its bundle), each from its source and
   from its unit; how many there were. The shell's tree is back after. */
static int units_match_sources(roc *m, const tree_file *all, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < n && k < sizeof(sources_only) / sizeof(sources_only[0]); i++) {
        if (strcmp(all[i].path, SCR_BUNDLE) != 0) {
            sources_only[k] = all[i];
            k++;
        }
    }
    int units = 0;
    for (size_t i = 0; i < n; i++) {
        const char *slash = strchr(all[i].path, '/');
        if (slash == NULL || strcmp(slash + 1, "init.filo") != 0) {
            continue;
        }
        char name[TREE_PATH_MAX];
        snprintf(name, sizeof(name), "%.*s", (int)(slash - all[i].path), all[i].path);
        char err_src[SCR_ERROR_MAX];
        char err_unit[SCR_ERROR_MAX];
        tree_programs(m, sources_only, k);
        bool ok_src = compose_step(m, name, &from_source, err_src, sizeof(err_src));
        CHECK(m->scr.unit == NULL);
        tree_set_source(all, n);
        bool ok_unit = compose_step(m, name, &m->cmp.work, err_unit, sizeof(err_unit));
        CHECK(m->scr.unit != NULL);
        CHECK(ok_src == ok_unit);
        CHECK(strcmp(err_src, err_unit) == 0);
        CHECK(same_canvas(&from_source, &m->cmp.work));
        if (!same_canvas(&from_source, &m->cmp.work) || strcmp(err_src, err_unit) != 0) {
            printf("  screen %s: source and unit differ (%s | %s)\n", name, err_src, err_unit);
        }
        units++;
    }
    tree_set_source(NULL, 0);
    return units;
}

static void test_screens_run_from_units(void) {
    cp_load();
    mock_host h;
    roc *m = boot_menu(&h);
    CHECK(units_match_sources(m, cp_files, cp_n) == 4);
}

/* The Cardputer's own screens, from the tree its firmware carries (made
   under build/ by the Makefile), on its 20x8 terminal: each one loads from
   its unit, the keys printed on its keycaps move around, and nothing reaches
   the panel that its ASCII font cannot draw. */
static bool ascii_only(const canvas *c) {
    for (uint16_t y = 0; y < c->rows; y++) {
        for (uint16_t x = 0; x < c->cols; x++) {
            if (c->cells[y][x].cp >= 0x7F) {
                printf("  non-ASCII U+%04X at row %u col %u\n", (unsigned)c->cells[y][x].cp, y, x);
                return false;
            }
        }
    }
    return true;
}

static void cp_on(roc *m, const char *name) {
    CHECK(strcmp(m->scr.name, name) == 0);
    CHECK(m->scr.unit != NULL);
    CHECK(screen_error(m)[0] == '\0');
    CHECK(m->cmp.target.cols == 20 && m->cmp.target.rows == 8);
    CHECK(ascii_only(&m->cmp.target));
}

static void test_cardputer_screens(void) {
    cp_load();
    tree_set_source(cp_files, cp_n);
    mock_host h;
    roc *m = boot_menu(&h);
    if (roc_layer_spec.home == NULL) {
        screen_enter(m, "main"); /* no layer to boot to: the firmware opens it */
    }
    roc_resize(m, 20, 8);
    drain(m);
    cp_on(m, "main");
    CHECK(canvas_has(&m->cmp.target, "[S] Shell"));

    send(m, "f");
    drain(m);
    cp_on(m, "files");
    send(m, "/"); /* the right arrow's keycap: over to /bin */
    drain(m);
    cp_on(m, "files");
    CHECK(canvas_has(&m->cmp.target, "/bin"));
    CHECK(canvas_has(&m->cmp.target, "help"));
    send(m, ".");
    drain(m);
    CHECK(scr_num(m, "sel") == 1);
    send(m, "\r"); /* reads it in the pager, over the list */
    drain(m);
    CHECK(!screen_is_top(m));
    send(m, "q");
    drain(m);
    CHECK(screen_is_top(m));
    cp_on(m, "files");
    CHECK(scr_num(m, "sel") == 1);
    send(m, "`");
    drain(m);
    cp_on(m, "main");

    send(m, "n");
    drain(m);
    cp_on(m, "snake-filo");
    CHECK(!scr_bool(m, "cramped"));
    CHECK(canvas_has(&m->cmp.target, "@"));
    CHECK(canvas_has(&m->cmp.target, "*"));
    for (int i = 0; i < 5; i++) { /* it waits for the first arrow */
        roc_tick(m, (uint32_t)scr_num(m, "STEP_MS"));
        drain(m);
    }
    CHECK(!scr_bool(m, "started"));
    CHECK(canvas_has(&m->cmp.target, "to go"));
    send(m, "/");
    drain(m);
    CHECK(scr_bool(m, "started"));
    for (int i = 0; i < 30 && !scr_bool(m, "over"); i++) {
        roc_tick(m, (uint32_t)scr_num(m, "STEP_MS"));
        drain(m);
    }
    CHECK(scr_bool(m, "over")); /* straight ahead meets the wall */
    cp_on(m, "snake-filo");
    CHECK(canvas_has(&m->cmp.target, "GAME OVER"));
    send(m, "r");
    drain(m);
    CHECK(!scr_bool(m, "over"));
    send(m, "`");
    drain(m);

    send(m, "i");
    drain(m);
    cp_on(m, "info");
    CHECK(canvas_has(&m->cmp.target, "20 x 8"));
    send(m, "x");
    drain(m);
    cp_on(m, "main");
    tree_set_source(NULL, 0);
    roc_resize(m, 80, 24);
}

/* No Filo file that ships defines a name the scripts' context has as a
   builtin: the builtin would take its place without a word, as a builtin
   "status" once did to home.filo's own function. How many files it read. */
static size_t shadows_checked(const filo_ctx *ctx, const tree_file *all, size_t n) {
    size_t files = 0;
    for (size_t i = 0; i < n; i++) {
        size_t pl = strlen(all[i].path);
        if (pl < 5 || strcmp(all[i].path + pl - 5, ".filo") != 0) {
            continue;
        }
        files++;
        for (uint32_t b = 0; b < ctx->nbuiltins; b++) {
            char def[80];
            size_t k = (size_t)snprintf(def, sizeof(def), "(def %s", ctx->builtins[b].name);
            const uint8_t *p = all[i].data;
            size_t left = all[i].len;
            const uint8_t *hit = NULL;
            while ((hit = memmem(p, left, def, k)) != NULL) {
                size_t at = (size_t)(hit - all[i].data) + k;
                if (at < all[i].len && strchr(" \t\r\n", all[i].data[at]) != NULL) {
                    fprintf(stderr, "%s defines builtin %s\n", all[i].path, ctx->builtins[b].name);
                    CHECK(false);
                    break;
                }
                left = all[i].len - (size_t)(hit + 1 - all[i].data);
                p = hit + 1;
            }
        }
    }
    return files;
}

static void test_builtins_shadow_nothing(void) {
    mock_host h;
    roc *m = boot(&h);
    const filo_ctx *ctx = script_context(m);
    CHECK(!m->sc.broken && ctx->nbuiltins < FILO_BUILTINS_MAX);
    cp_n = 0;
    cp_used = 0;
    cp_walk(ROC_ROOT "/commands", "commands/");
    cp_walk("build/cardputer-tree", "cardputer/");
    CHECK(shadows_checked(ctx, cp_files, cp_n) > 50); /* the commands and the Cardputer's */
}

/* Each command runs in a fresh context: the arena does not grow from one to
   the next, and what one defined the next does not see. */
/* The bytes of a file of the home, NULL when it is not there. */
static const uint8_t *home_bytes(roc *m, const char *path, size_t *len) {
    const uint8_t *d = NULL;
    *len = 0;
    return ufs_find(&m->uf, path, &d, len) ? d : NULL;
}

static bool home_is(roc *m, const char *path, const uint8_t *want, size_t n) {
    size_t len = 0;
    const uint8_t *d = home_bytes(m, path, &len);
    return d != NULL && len == n && (n == 0 || memcmp(d, want, n) == 0);
}

/* What $? is after line, read back through a file (not the screen). */
static int status_after(roc *m, const char *line) {
    char cmd[512];
    (void)snprintf(cmd, sizeof(cmd), "%s; echo $? > ~/.st\r", line);
    send(m, cmd);
    drain(m);
    size_t len = 0;
    const uint8_t *d = home_bytes(m, "/home/guest/.st", &len);
    return d == NULL ? -1 : atoi((const char *)d);
}

/* Etapa 1: a script's status and stderr are its own, and data goes
   through > | < byte for byte, in source and in bytecode alike. */
static void test_script_status_and_data(void) {
    mock_host h;
    roc *m = boot(&h);
    static const char st2[] = "(exit-status 2)";
    static const char ret3[] = "(+ 1 2)";
    static const char ex3[] = "(exit 3)";
    static const char bad[] = "(nth (list) 5)";
    static const char late[] = "(exit-status 2) (nth (list) 5)";
    static const char both[] = "(out-write \"out\\n\") (err-write \"err\\n\") (out-write 42)";
    static const char copy[] = "(out-write (in-read))";
    static const char legacy[] = "(out-write STDIN)";
    static const char chunks[] =
        "(def loop (fn () (let ((b (in-read 4093))) (if (is-nil b) 0 (do (out-write b) (loop))))))"
        "(loop)";
    static const char lines[] =
        "(def loop (fn (k) (let ((l (in-line))) (if (is-nil l) (out-write k) "
        "(do (out-write \"[\" l \"]\") (loop (+ k 1)))))))"
        "(loop 0)";
    static const char peek[] = "(out-write (exit-status))";
    static uint8_t st2b[4096];
    size_t st2b_len = 0;
    CHECK(script_build(m, (const uint8_t *)st2, sizeof(st2) - 1, st2b, sizeof(st2b), &st2b_len));
    static uint8_t copyb[4096];
    size_t copyb_len = 0;
    CHECK(
        script_build(m, (const uint8_t *)copy, sizeof(copy) - 1, copyb, sizeof(copyb), &copyb_len));
    const tree_file tree[] = {
        {"bin/st2.filo", (const uint8_t *)st2, sizeof(st2) - 1},
        {"bin/st2b.fbb", st2b, st2b_len},
        {"bin/ret3.filo", (const uint8_t *)ret3, sizeof(ret3) - 1},
        {"bin/ex3.filo", (const uint8_t *)ex3, sizeof(ex3) - 1},
        {"bin/bad.filo", (const uint8_t *)bad, sizeof(bad) - 1},
        {"bin/late.filo", (const uint8_t *)late, sizeof(late) - 1},
        {"bin/both.filo", (const uint8_t *)both, sizeof(both) - 1},
        {"bin/copy.filo", (const uint8_t *)copy, sizeof(copy) - 1},
        {"bin/copyb.fbb", copyb, copyb_len},
        {"bin/peek.filo", (const uint8_t *)peek, sizeof(peek) - 1},
        {"bin/legacy.filo", (const uint8_t *)legacy, sizeof(legacy) - 1},
        {"bin/chunks.filo", (const uint8_t *)chunks, sizeof(chunks) - 1},
        {"bin/lines.filo", (const uint8_t *)lines, sizeof(lines) - 1},
    };
    tree_programs(m, tree, sizeof(tree) / sizeof(tree[0]));
    send(m, "cd\r");
    drain(m);

    /* status: explicit, the same from bytecode; old scripts unchanged */
    CHECK(status_after(m, "st2") == 2);
    CHECK(status_after(m, "st2b") == 2);
    CHECK(status_after(m, "ret3") == 0);
    CHECK(status_after(m, "ex3") == 0);
    CHECK(status_after(m, "bad 2>/dev/null") == 1);
    CHECK(status_after(m, "late 2>/dev/null") == 2);
    CHECK(status_after(m, "st2 && echo yes > ~/y") == 2 &&
          home_bytes(m, "/home/guest/y", &(size_t){0}) == NULL);
    CHECK(status_after(m, "st2 || true") == 0);
    send(m, "false; peek > ~/p\r");
    drain(m);
    CHECK(home_is(m, "/home/guest/p", (const uint8_t *)"0", 1)); /* each command starts at 0 */

    /* stdout and stderr apart; 2> and 2>&1 */
    CHECK(status_after(m, "both > ~/o 2> ~/e") == 0);
    CHECK(home_is(m, "/home/guest/o", (const uint8_t *)"out\n42", 6));
    CHECK(home_is(m, "/home/guest/e", (const uint8_t *)"err\n", 4));
    CHECK(status_after(m, "both > ~/oe 2>&1") == 0);
    CHECK(home_is(m, "/home/guest/oe", (const uint8_t *)"out\nerr\n42", 10));
    send(m, "both 2>/dev/null\r");
    const char *o = drain(m);
    CHECK(strstr(o, "out\r\n42") != NULL && strstr(o, "err") == NULL);
    send(m, "both > /dev/null\r");
    o = drain(m);
    CHECK(strstr(o, "err\r\n") != NULL && strstr(o, "out") == NULL);

    /* bytes 0..255 through <, > and |, as they are */
    static uint8_t all[256 * 3];
    for (size_t i = 0; i < sizeof(all); i++) {
        all[i] = (uint8_t)i;
    }
    CHECK(roc_write_file(m, "t", "/home/guest/all", all, sizeof(all)));
    CHECK(status_after(m, "copy < ~/all > ~/c1") == 0);
    CHECK(home_is(m, "/home/guest/c1", all, sizeof(all)));
    CHECK(status_after(m, "copy < ~/all | copyb | copy > ~/c2") == 0);
    CHECK(home_is(m, "/home/guest/c2", all, sizeof(all)));
    CHECK(status_after(m, "copy < ~/all >> ~/c2") == 0);
    size_t len = 0;
    const uint8_t *twice = home_bytes(m, "/home/guest/c2", &len);
    CHECK(twice != NULL && len == 2 * sizeof(all) &&
          memcmp(twice + sizeof(all), all, sizeof(all)) == 0);

    /* the capture's edge: full passes; past it the > goes to its file as it
       is written (a pipe still cuts) */
    static uint8_t big[ROC_CAP_MAX + 1];
    for (size_t i = 0; i < sizeof(big); i++) {
        big[i] = (uint8_t)(i * 7);
    }
    CHECK(roc_write_file(m, "t", "/home/guest/full", big, ROC_CAP_MAX));
    CHECK(roc_write_file(m, "t", "/home/guest/over", big, sizeof(big)));
    CHECK(status_after(m, "copy < ~/full > ~/f1") == 0);
    CHECK(home_is(m, "/home/guest/f1", big, ROC_CAP_MAX));
    CHECK(status_after(m, "copy < ~/full | copy > ~/f2") == 0);
    CHECK(home_is(m, "/home/guest/f2", big, ROC_CAP_MAX));
    CHECK(status_after(m, "copy < ~/over > ~/f3") == 0);
    CHECK(home_is(m, "/home/guest/f3", big, sizeof(big)));
    /* past the store's 512K a file: said, and the one there stays whole */
    CHECK(status_after(m, "copy < ~/over >> ~/f3 2>/dev/null") == 1);
    CHECK(home_is(m, "/home/guest/f3", big, sizeof(big)));
    CHECK(ufs_entry(&m->uf, "/home/guest/f3.~o") == NULL && !m->uf.open);
    CHECK(status_after(m, "copy < ~/full >> ~/f1") == 0); /* >> past the edge, in the store */
    size_t f1n = 0;
    const uint8_t *f1 = home_bytes(m, "/home/guest/f1", &f1n);
    CHECK(f1 != NULL && f1n == 2 * ROC_CAP_MAX && memcmp(f1 + ROC_CAP_MAX, big, ROC_CAP_MAX) == 0);
    /* the store's 2M back for what follows (this test's /bin has no rm) */
    CHECK(ufs_remove(&m->uf, "/home/guest/f1") && ufs_remove(&m->uf, "/home/guest/f3"));
    /* a loop's > past the edge spills too: two 200K files into one */
    static uint8_t half[200000];
    for (size_t i = 0; i < sizeof(half); i++) {
        half[i] = (uint8_t)(i % 10 == 9 ? '\n' : 'a' + (i % 10));
    }
    CHECK(roc_write_file(m, "t", "/home/guest/half", half, sizeof(half)));
    CHECK(status_after(m, "for i in 1 2; do cat ~/half; done > ~/bl") == 0);
    size_t bln = 0;
    const uint8_t *bl = home_bytes(m, "/home/guest/bl", &bln);
    CHECK(bl != NULL && bln == 2 * sizeof(half) &&
          memcmp(bl + sizeof(half), half, sizeof(half)) == 0);
    CHECK(ufs_remove(&m->uf, "/home/guest/bl") && ufs_remove(&m->uf, "/home/guest/half"));
    CHECK(status_after(m, "copy < ~/over 2>/dev/null | copy > /dev/null") == 1);
    CHECK(status_after(m, "copy < ~/over 2>/dev/null | copy > /dev/null || true") == 0);
    CHECK(status_after(m, "echo fine | copy > /dev/null") == 0); /* the cut does not linger */

    /* STDIN as before: a full pipe fits in half the persistent arena */
    CHECK(status_after(m, "legacy < ~/all > ~/l1") == 0);
    CHECK(home_is(m, "/home/guest/l1", all, sizeof(all)));
    CHECK(status_after(m, "legacy < ~/full > ~/l2") == 0);
    CHECK(home_is(m, "/home/guest/l2", big, ROC_CAP_MAX));
    /* reads in pieces and by lines, the rest kept between them */
    CHECK(status_after(m, "chunks < ~/full > ~/k1") == 0);
    CHECK(home_is(m, "/home/guest/k1", big, ROC_CAP_MAX));
    CHECK(status_after(m, "chunks < /dev/null > ~/k2") == 0);
    CHECK(home_is(m, "/home/guest/k2", NULL, 0));
    CHECK(roc_write_file(m, "t", "/home/guest/ls", (const uint8_t *)"a\n\nb\r\nc", 7));
    CHECK(status_after(m, "lines < ~/ls > ~/k3") == 0);
    CHECK(home_is(m, "/home/guest/k3", (const uint8_t *)"[a\n][\n][b\r\n][c]4", 16));

    /* a destination that cannot be written */
    CHECK(status_after(m, "copy < ~/all > /pub/x 2>/dev/null") == 1);

    /* the shell's own data commands keep their bytes too */
    CHECK(status_after(m, "printf 'a\\033[1mb\\r\\nc\\n' > ~/pf") == 0);
    CHECK(home_is(m, "/home/guest/pf", (const uint8_t *)"a\x1b[1mb\r\nc\n", 10));
    CHECK(status_after(m, "echo one  two > ~/ec; echo -n x >> ~/ec") == 0);
    CHECK(home_is(m, "/home/guest/ec", (const uint8_t *)"one two\nx", 9));
    CHECK(status_after(m, "cat ~/all > ~/ca") == 0);
    CHECK(home_is(m, "/home/guest/ca", all, sizeof(all)));
    CHECK(status_after(m, "cat ~/all | copy > ~/cb") == 0);
    CHECK(home_is(m, "/home/guest/cb", all, sizeof(all)));
    CHECK(status_after(m, "x=$(printf 'p\\nq\\n\\n'); echo \"$x\" > ~/sub") == 0);
    CHECK(home_is(m, "/home/guest/sub", (const uint8_t *)"p\nq\n", 4));
    send(m, "printf 'l1\\nl2\\n'\r");
    o = drain(m);
    CHECK(strstr(o, "l1\r\nl2\r\n") != NULL); /* on the terminal, lines as ever */
    tree_set_source(NULL, 0);
}

/* The C oracle for cksum: bit by bit, the way POSIX describes it. */
static uint32_t cksum_bitwise(const uint8_t *p, size_t n) {
    uint32_t crc = 0;
    size_t len = n;
    for (size_t i = 0;; i++) {
        uint8_t b = 0;
        if (i < n) {
            b = p[i];
        } else if (len > 0) {
            b = (uint8_t)(len & 0xFFU);
            len >>= 8U;
        } else {
            break;
        }
        crc ^= (uint32_t)b << 24U;
        for (int k = 0; k < 8; k++) {
            crc = (crc & 0x80000000U) != 0 ? (crc << 1U) ^ 0x04C11DB7U : crc << 1U;
        }
    }
    return ~crc;
}

/* Etapa 2: bytes, 32-bit words, regexes, UTF-8, int-text and iterate, as
   a script sees them, from source and from bytecode alike; cksum written
   in Filo against known vectors and against the C oracle. */
static void test_script_data(void) {
    mock_host h;
    roc *m = boot(&h);
    static const char prims[] =
        "(def say (fn (x) (out-write (if (= (type-of x) \"number\") x (string x)) \"\\n\")))\n"
        "(def s (bytes 0 104 105 255 10))\n"
        "(say (byte-len s))\n"
        "(say (byte-at s 0))\n"
        "(say (byte-at s 3))\n"
        "(say (byte-at s 5))\n"
        "(say (byte-at s -1))\n"
        "(say (= (byte-sub s 1 3) \"hi\"))\n"
        "(say (byte-len (byte-sub s 3)))\n"
        "(say (byte-sub s 4 2))\n"
        "(say (= (byte-sub s -5 100) s))\n"
        "(say (byte-find \"abcabc\" \"c\"))\n"
        "(say (byte-find \"abcabc\" \"c\" 3))\n"
        "(say (byte-find \"abc\" \"x\"))\n"
        "(say (byte-find \"abc\" \"\"))\n"
        "(say (byte-find (bytes 1 0 2) (bytes 0)))\n"
        "(say (byte-cmp \"a\" \"b\"))\n"
        "(say (byte-cmp \"b\" \"a\"))\n"
        "(say (byte-cmp \"ab\" \"a\"))\n"
        "(say (byte-cmp \"\" \"\"))\n"
        "(say (byte-cmp (bytes 255) (bytes 1)))\n"
        "(say (byte-list (bytes 0 7 255)))\n"
        "(say (byte-len (bytes (list 1 2 3) 4)))\n"
        "(say (str-len \"café\"))\n"
        "(say (byte-len \"café\"))\n"
        "(say (u32 -1))\n"
        "(say (u32-and 240 60))\n"
        "(say (u32-or 240 15))\n"
        "(say (u32-xor 255 15))\n"
        "(say (u32-not 0))\n"
        "(say (u32-shl 1 31))\n"
        "(say (u32-shl 1 32))\n"
        "(say (u32-shr 4294967295 28))\n"
        "(say (u32-add 4294967295 2))\n"
        "(say (u32-mul 65536 65536))\n"
        "(say (u32-mul 4294967295 4294967295))\n"
        "(def r (re-compile \"b\\\\(a*\\\\)c\"))\n"
        "(say (re-match r \"xbaac\"))\n"
        "(say (re-match (re-compile \"(x|y)+z\" \"E\") \"aaxyxz\"))\n"
        "(say (re-match (re-compile \"ABC\" \"i\") \"xabc\"))\n"
        "(say (re-match r \"nothing here\"))\n"
        "(say (type-of (re-compile \"a\\\\(\")))\n"
        "(say (re-match (re-compile \"^a\") \"ba\" 1))\n"
        "(say (re-match (re-compile \"^a\") \"ba\" 1 #t))\n"
        "(say (re-match (re-compile \"(a)|b\" \"E\") \"b\"))\n"
        "(say (re-free r))\n"
        "(say (re-free r))\n"
        "(say (utf8-valid \"café\"))\n"
        "(say (utf8-valid (bytes 99 195)))\n"
        "(say (utf8-valid (bytes 255)))\n"
        "(say (utf8-runes \"aé€😀\"))\n"
        "(say (utf8-runes (bytes 97 255 98)))\n"
        "(say (utf8-runes (bytes 97 226 130)))\n"
        "(say (= (utf8-encode 99 97 102 233) \"café\"))\n"
        "(say (str-width \"aé中😀\"))\n"
        "(say (str-width (str-concat \"e\" (utf8-encode 769))))\n"
        "(say (str-width (bytes 9 255 97)))\n"
        "(say (rune-width 20013))\n"
        "(say (iterate (fn (k) (if (= k 10000) (list) (+ k 1))) 0))\n"
        "(say (int-text 4294967295))\n"
        "(say (int-text 255 16))\n"
        "(say (int-text 8 8))\n"
        "(say (int-text -5 2))\n"
        "";
    static const char want[] = "5\n"
                               "0\n"
                               "255\n"
                               "(list)\n"
                               "(list)\n"
                               "#t\n"
                               "2\n"
                               "\n"
                               "#t\n"
                               "2\n"
                               "5\n"
                               "-1\n"
                               "0\n"
                               "1\n"
                               "-1\n"
                               "1\n"
                               "1\n"
                               "0\n"
                               "1\n"
                               "(list 0 7 255)\n"
                               "4\n"
                               "4\n"
                               "5\n"
                               "4294967295\n"
                               "48\n"
                               "255\n"
                               "240\n"
                               "4294967295\n"
                               "2147483648\n"
                               "0\n"
                               "15\n"
                               "1\n"
                               "0\n"
                               "1\n"
                               "(list (tuple 1 5) (tuple 2 4))\n"
                               "(list (tuple 2 6) (tuple 4 5))\n"
                               "(list (tuple 1 4))\n"
                               "(list)\n"
                               "string\n"
                               "(list)\n"
                               "(list)\n"
                               "(list (tuple 0 1) (list))\n"
                               "#t\n"
                               "#f\n"
                               "#t\n"
                               "#f\n"
                               "#f\n"
                               "(list 97 233 8364 128512)\n"
                               "(list 97 65533 98)\n"
                               "(list 97 65533)\n"
                               "#t\n"
                               "6\n"
                               "1\n"
                               "2\n"
                               "2\n"
                               "10000\n"
                               "4294967295\n"
                               "ff\n"
                               "10\n"
                               "-101\n"
                               "";
    static const char cksum[] =
        "; POSIX cksum: CRC-32 of polynomial 04C11DB7, high bit first, over the\n"
        "; bytes and then the length (low byte first), complemented.\n"
        "(def poly 79764919)\n"
        "(def table\n"
        "  (map (fn (i)\n"
        "         (fold (fn (c k) (if (= (u32-and c 2147483648) 0) (u32-shl c 1) (u32-xor (u32-shl "
        "c 1) poly)))\n"
        "               (u32-shl i 24) (range 0 8)))\n"
        "       (range 0 256)))\n"
        "(def step (fn (crc b) (u32-xor (u32-shl crc 8) (nth table (u32-xor (u32-shr crc 24) "
        "b)))))\n"
        "(def with-len (fn (crc n) (if (= n 0) crc (with-len (step crc (u32-and n 255)) (floor (/ "
        "n 256))))))\n"
        "(def total\n"
        "  (iterate (fn (st)\n"
        "             (let ((b (in-read 4096)))\n"
        "               (if (is-nil b) (list) (list (fold step (nth st 0) (byte-list b)) (+ (nth "
        "st 1) (byte-len b))))))\n"
        "           (list 0 0)))\n"
        "(out-write (u32-not (with-len (nth total 0) (nth total 1))) \" \" (nth total 1) \"\\n\")\n"
        "";
    static const char reloop[] =
        "(def re (re-compile \"^ab*c [0-9]*5$\"))\n"
        "(out-write (iterate (fn (k) (let ((l (in-line))) (if (is-nil l) (list) (if (is-nil "
        "(re-match re (byte-sub l 0 (- (byte-len l) 1)))) k (+ k 1))))) 0))\n"
        "";
    static const char keep[] = "(out-write (re-compile \"a\"))";
    static const char reuse[] = "(re-match (number (in-read)) \"a\")";
    static const char e1[] = "(bytes 256)";
    static const char e2[] = "(byte-at \"a\" 1.5)";
    static const char e3[] = "(u32 1.5)";
    static const char e4[] = "(u32 4294967296)";
    static const char e5[] = "(u32-shl 1 33)";
    static const char e6[] = "(re-match 12345 \"x\")";
    static const char e7[] = "(utf8-encode 55296)";
    static const char e8[] = "(re-compile \"a\" \"Q\")";
    static const char e9[] = "(iterate 5 0)";
    static const char e10[] = "(int-text 1.5)";
    static uint8_t primsb[16384];
    size_t primsb_len = 0;
    CHECK(script_build(m, (const uint8_t *)prims, sizeof(prims) - 1, primsb, sizeof(primsb),
                       &primsb_len));
    const tree_file tree[] = {
        {"bin/prims.filo", (const uint8_t *)prims, sizeof(prims) - 1},
        {"bin/primsb.fbb", primsb, primsb_len},
        {"bin/cksum.filo", (const uint8_t *)cksum, sizeof(cksum) - 1},
        {"bin/reloop.filo", (const uint8_t *)reloop, sizeof(reloop) - 1},
        {"bin/keep.filo", (const uint8_t *)keep, sizeof(keep) - 1},
        {"bin/reuse.filo", (const uint8_t *)reuse, sizeof(reuse) - 1},
        {"bin/e1.filo", (const uint8_t *)e1, sizeof(e1) - 1},
        {"bin/e2.filo", (const uint8_t *)e2, sizeof(e2) - 1},
        {"bin/e3.filo", (const uint8_t *)e3, sizeof(e3) - 1},
        {"bin/e4.filo", (const uint8_t *)e4, sizeof(e4) - 1},
        {"bin/e5.filo", (const uint8_t *)e5, sizeof(e5) - 1},
        {"bin/e6.filo", (const uint8_t *)e6, sizeof(e6) - 1},
        {"bin/e7.filo", (const uint8_t *)e7, sizeof(e7) - 1},
        {"bin/e8.filo", (const uint8_t *)e8, sizeof(e8) - 1},
        {"bin/e9.filo", (const uint8_t *)e9, sizeof(e9) - 1},
        {"bin/e10.filo", (const uint8_t *)e10, sizeof(e10) - 1},
    };
    tree_programs(m, tree, sizeof(tree) / sizeof(tree[0]));
    send(m, "cd\r");
    drain(m);
    CHECK(status_after(m, "prims > ~/p") == 0);
    CHECK(home_is(m, "/home/guest/p", (const uint8_t *)want, sizeof(want) - 1));
    CHECK(status_after(m, "primsb > ~/pb") == 0);
    CHECK(home_is(m, "/home/guest/pb", (const uint8_t *)want, sizeof(want) - 1));
    CHECK(!m->sc.broken);

    /* cksum: the vectors every implementation gives, then the C oracle */
    CHECK(status_after(m, "cksum < /dev/null > ~/k0") == 0);
    CHECK(home_is(m, "/home/guest/k0", (const uint8_t *)"4294967295 0\n", 13));
    CHECK(status_after(m, "printf 123456789 | cksum > ~/k1") == 0);
    CHECK(home_is(m, "/home/guest/k1", (const uint8_t *)"930766865 9\n", 12));
    CHECK(status_after(m, "printf 'a\\0b\\377' | cksum > ~/k2") == 0);
    CHECK(home_is(m, "/home/guest/k2", (const uint8_t *)"2691244216 4\n", 13));
    static uint8_t noise[200000];
    uint32_t x = 2463534242U;
    for (size_t i = 0; i < sizeof(noise); i++) {
        x ^= x << 13U;
        x ^= x >> 17U;
        x ^= x << 5U;
        noise[i] = (uint8_t)x;
    }
    CHECK(roc_write_file(m, "t", "/home/guest/noise", noise, sizeof(noise)));
    CHECK(status_after(m, "cksum < ~/noise > ~/k3") == 0);
    char line[32];
    int k = snprintf(line, sizeof(line), "%u %zu\n", cksum_bitwise(noise, sizeof(noise)),
                     sizeof(noise));
    CHECK(home_is(m, "/home/guest/k3", (const uint8_t *)line, (size_t)k));

    /* one regex over 20000 lines: no memory grows with them */
    static char lines[20000 * 12];
    size_t n = 0;
    for (int i = 0; i < 20000; i++) {
        n += (size_t)snprintf(lines + n, sizeof(lines) - n, "%s %d\n", i % 2 ? "abbc" : "ac", i);
    }
    CHECK(roc_write_file(m, "t", "/home/guest/lines", (const uint8_t *)lines, n));
    CHECK(status_after(m, "reloop < ~/lines > ~/rl") == 0);
    CHECK(home_is(m, "/home/guest/rl", (const uint8_t *)"2000", 4));

    /* a handle is this run's: one kept from another is refused */
    CHECK(status_after(m, "keep > ~/h") == 0);
    CHECK(status_after(m, "reuse < ~/h 2> ~/he") == 1);
    size_t hl = 0;
    const uint8_t *he = home_bytes(m, "/home/guest/he", &hl);
    CHECK(he != NULL && memmem(he, hl, "not a regex of this run", 23) != NULL);

    /* each mistake is an error, status 1, said on stderr */
    CHECK(status_after(m, "e1 2>/dev/null") == 1);
    CHECK(status_after(m, "e2 2>/dev/null") == 1);
    CHECK(status_after(m, "e3 2>/dev/null") == 1);
    CHECK(status_after(m, "e4 2>/dev/null") == 1);
    CHECK(status_after(m, "e5 2>/dev/null") == 1);
    CHECK(status_after(m, "e6 2>/dev/null") == 1);
    CHECK(status_after(m, "e7 2>/dev/null") == 1);
    CHECK(status_after(m, "e8 2>/dev/null") == 1);
    CHECK(status_after(m, "e9 2>/dev/null") == 1);
    CHECK(status_after(m, "e10 2>/dev/null") == 1);
    tree_set_source(NULL, 0);
}

/* Etapa 3: what a path is (the memory, the shell's tree and the site's
   index, each said apart; a time the store does not keep is nil, not
   1970), a directory read in pages in byte order with nothing cut, and
   paths resolved as the shell resolves them. */
static void test_script_files(void) {
    mock_host h;
    roc *m = boot(&h);
    static const char meta[] =
        "(def txt (fn (x) (cond ((= (type-of x) \"number\") (int-text x)) ((= (type-of x) "
        "\"bool\") (if x \"#t\" \"#f\")) ((= (type-of x) \"string\") x) ((is-nil x) \"nil\") (else "
        "(string x)))))\n"
        "(def show (fn (x) (out-write (txt x) \"\\n\")))\n"
        "(def st (fn (p) (let ((s (file-stat p))) (show (if (is-nil s) \"nil\" (letv (k z t o) s "
        "(str-join \" \" (map txt (list k z t o)))))))))\n"
        "(st \"~/f.txt\")\n"
        "(st \"~/d\")\n"
        "(st \"~/missing\")\n"
        "(st \"/pub/kutta.md\")\n"
        "(st \"/pub\")\n"
        "(letv (k z t o) (file-stat \"/lib/roc/common.filo\") (show (str-join \" \" (map txt (list "
        "k (> z 0) t o)))))\n"
        "(def page (fn (r) (letv (es next) r (show (str-join \" \" (list (str-join \",\" (map (fn "
        "(e) (letv (nm kd) e (str-concat nm \":\" kd))) es)) (txt next)))))))\n"
        "(page (dir-read \"~/d\"))\n"
        "(page (dir-read \"~/d\" 2 2))\n"
        "(page (dir-read \"~/d\" 6))\n"
        "(show (path-resolve \"~/a/../b\"))\n"
        "(show (path-resolve \"x//y/.\"))\n"
        "(show (path-resolve \"/..\"))\n"
        "";
    static const char want[] = "file 5 nil home\n"
                               "dir nil nil home\n"
                               "nil\n"
                               "file 1234 1786876200 site\n"
                               "dir nil 1787911200 site\n"
                               "file #t nil tree\n"
                               "-dash:file,.hidden:file,a:file,b:file,sp ace:file,sub:dir nil\n"
                               "a:file,b:file 4\n"
                               " nil\n"
                               "/home/guest/b\n"
                               "/home/guest/x/y\n"
                               "/\n"
                               "";
    static const char pages[] =
        "(def total\n"
        "  (iterate (fn (st)\n"
        "             (if (is-nil (nth st 0)) (list)\n"
        "               (letv (es next) (dir-read \"~/many\" (nth st 0) 200)\n"
        "                 (list next (+ (nth st 1) (length es))\n"
        "                       (if (= (nth st 1) 0) (letv (nm kd) (nth es 0) nm) (nth st 2))))))\n"
        "           (list 0 0 \"\")))\n"
        "(out-write (nth total 1) \" \" (nth total 2) \" \" (length (dir-entries \"~/many\")))\n"
        "";
    static const char notdir[] = "(dir-read \"~/f.txt\")";
    static const char nodir[] = "(dir-read \"~/missing\")";
    static const char badstat[] = "(file-stat 5)";
    const tree_file tree[] = {
        {"bin/meta.filo", (const uint8_t *)meta, sizeof(meta) - 1},
        {"bin/pages.filo", (const uint8_t *)pages, sizeof(pages) - 1},
        {"bin/notdir.filo", (const uint8_t *)notdir, sizeof(notdir) - 1},
        {"bin/nodir.filo", (const uint8_t *)nodir, sizeof(nodir) - 1},
        {"bin/badstat.filo", (const uint8_t *)badstat, sizeof(badstat) - 1},
        {"common.filo", (const uint8_t *)"; shared\n", 9},
    };
    tree_programs(m, tree, sizeof(tree) / sizeof(tree[0]));
    send(m, "cd\r");
    drain(m);
    CHECK(roc_cmd_mkdir(m, "/home/guest/d") && roc_cmd_mkdir(m, "/home/guest/d/sub") &&
          roc_cmd_mkdir(m, "/home/guest/many"));
    CHECK(roc_write_file(m, "t", "/home/guest/f.txt", (const uint8_t *)"hello", 5));
    const char *names[] = {"b", "a", "-dash", "sp ace", ".hidden"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[64];
        (void)snprintf(path, sizeof(path), "/home/guest/d/%s", names[i]);
        CHECK(roc_write_file(m, "t", path, (const uint8_t *)"x", 1));
    }
    CHECK(status_after(m, "meta > ~/o") == 0);
    size_t ol = 0;
    const uint8_t *o = home_bytes(m, "/home/guest/o", &ol);
    if (o == NULL || ol != sizeof(want) - 1 || memcmp(o, want, ol) != 0) {
        fprintf(stderr, "meta gave:\n%.*s\n", (int)ol, o != NULL ? (const char *)o : "");
    }
    CHECK(home_is(m, "/home/guest/o", (const uint8_t *)want, sizeof(want) - 1));

    /* 513 entries: pages of 200, past the 512 dir-entries once stopped at */
    for (int i = 0; i < 513; i++) {
        char path[48];
        (void)snprintf(path, sizeof(path), "/home/guest/many/d%03d", 512 - i);
        CHECK(roc_cmd_mkdir(m, path));
    }
    CHECK(status_after(m, "pages > ~/pg") == 0);
    CHECK(home_is(m, "/home/guest/pg", (const uint8_t *)"513 d000 513", 12));
    CHECK(status_after(m, "notdir 2>/dev/null") == 1);
    CHECK(status_after(m, "nodir 2>/dev/null") == 1);
    CHECK(status_after(m, "badstat 2>/dev/null") == 1);
    tree_set_source(NULL, 0);
}

/* Etapa 3b: a script runs another command (run) in a context of its own,
   carved from the caller's memory, the caller's state as it was after: what
   it wrote so far, its input not yet read, its directory, its $?. */
static void test_script_calls(void) {
    mock_host h;
    roc *m = boot(&h);
    static const char inner[] = "(out-write (str-join \"|\" ARGS) \"\\n\") (err-write \"e:\" "
                                "(byte-len (in-read)) \"\\n\") (exit-status (length ARGS))";
    static const char outer[] =
        "(out-write \"before\\n\")\n"
        "(letv (st o e) (run (list \"inner\" \"a b\" \"c'd\" \"\" \"$HOME *\") \"xyz\")\n"
        "  (out-write \"status=\" st \"\\nout=\" o \"err=\" e))\n"
        "(out-write \"after\\n\")";
    static const char outer2[] =
        "(def first (in-line))\n"
        "(letv (st o e) (run (list \"eval\" \"echo pq | copy\")) (out-write o))\n"
        "(out-write first (in-read))";
    static const char copy[] = "(out-write (in-read))";
    static const char s1[] = "(letv (st o e) (run (list \"s2\")) (out-write \"1:\" o))";
    static const char s2[] = "(letv (st o e) (run (list \"s3\")) (out-write \"2:\" o))";
    static const char s3[] = "(out-write \"3\")";
    static const char incd[] = "(cd \"/pub\") (out-write (pwd))";
    static const char outcd[] = "(letv (st o e) (run (list \"incd\")) (out-write o \" \" (pwd)))";
    static const char rec[] =
        "(letv (st o e) (run (list \"rec\")) (do (err-write e) (exit-status (if (= st 0) 0 1))))";
    static const char inless[] = "(less \"x\")";
    static const char outless[] = "(letv (st o e) (run (list \"inless\")) (out-write st \" \" "
                                  "(byte-find e \"not in a command another one ran\")))";
    static const char inre[] = "(re-match (number (nth ARGS 0)) \"a\")";
    static const char outre[] =
        "(def h (re-compile \"a\"))\n"
        "(letv (st o e) (run (list \"inre\" (int-text h))) (out-write st \" \" (byte-find e \"not "
        "a regex of this run\") \" \" (string (re-match h \"a\"))))";
    static const char outc[] =
        "(letv (st o e) (run (list \"printf\" \"%s-%s\" \"a b\" \"c\")) (out-write st \" \" o))";
    static const char outfn[] = "(letv (st o e) (run (list \"f\" \"x y\")) (out-write st \" \" o))";
    static const char outmiss[] = "(letv (st o e) (run (list \"no-such-command\")) (out-write st "
                                  "\" \" (byte-find e \"command not found\")))";
    static const char outlong[] =
        "(run (list (str-join \"\" (map (fn (i) \"xxxxxxxxxx\") (range 0 200)))))";
    static uint8_t outerb[16384];
    size_t outerb_len = 0;
    CHECK(script_build(m, (const uint8_t *)outer, sizeof(outer) - 1, outerb, sizeof(outerb),
                       &outerb_len));
    const tree_file tree[] = {
        {"bin/inner.filo", (const uint8_t *)inner, sizeof(inner) - 1},
        {"bin/outer.filo", (const uint8_t *)outer, sizeof(outer) - 1},
        {"bin/outer2.filo", (const uint8_t *)outer2, sizeof(outer2) - 1},
        {"bin/copy.filo", (const uint8_t *)copy, sizeof(copy) - 1},
        {"bin/s1.filo", (const uint8_t *)s1, sizeof(s1) - 1},
        {"bin/s2.filo", (const uint8_t *)s2, sizeof(s2) - 1},
        {"bin/s3.filo", (const uint8_t *)s3, sizeof(s3) - 1},
        {"bin/incd.filo", (const uint8_t *)incd, sizeof(incd) - 1},
        {"bin/outcd.filo", (const uint8_t *)outcd, sizeof(outcd) - 1},
        {"bin/rec.filo", (const uint8_t *)rec, sizeof(rec) - 1},
        {"bin/inless.filo", (const uint8_t *)inless, sizeof(inless) - 1},
        {"bin/outless.filo", (const uint8_t *)outless, sizeof(outless) - 1},
        {"bin/inre.filo", (const uint8_t *)inre, sizeof(inre) - 1},
        {"bin/outre.filo", (const uint8_t *)outre, sizeof(outre) - 1},
        {"bin/outc.filo", (const uint8_t *)outc, sizeof(outc) - 1},
        {"bin/outfn.filo", (const uint8_t *)outfn, sizeof(outfn) - 1},
        {"bin/outmiss.filo", (const uint8_t *)outmiss, sizeof(outmiss) - 1},
        {"bin/outlong.filo", (const uint8_t *)outlong, sizeof(outlong) - 1},
        {"bin/outerb.fbb", outerb, outerb_len},
    };
    tree_programs(m, tree, sizeof(tree) / sizeof(tree[0]));
    send(m, "cd\r");
    drain(m);
    static const char want[] = "before\n"
                               "status=4\n"
                               "out=a b|c'd||$HOME *\n"
                               "err=e:3\n"
                               "after\n"
                               "";
    CHECK(status_after(m, "outer > ~/o") == 0);
    CHECK(home_is(m, "/home/guest/o", (const uint8_t *)want, sizeof(want) - 1));
    CHECK(status_after(m, "outerb > ~/ob") == 0);
    CHECK(home_is(m, "/home/guest/ob", (const uint8_t *)want, sizeof(want) - 1));
    CHECK(status_after(m, "outer | copy > ~/op") == 0);
    CHECK(home_is(m, "/home/guest/op", (const uint8_t *)want, sizeof(want) - 1));
    /* the caller's input survives a pipeline the called one runs */
    CHECK(status_after(m, "printf 'L1\\nL2\\n' | outer2 > ~/g") == 0);
    CHECK(home_is(m, "/home/guest/g", (const uint8_t *)"pq\nL1\nL2\n", 9));
    CHECK(status_after(m, "s1 > ~/s") == 0);
    CHECK(home_is(m, "/home/guest/s", (const uint8_t *)"1:2:3", 5));
    CHECK(status_after(m, "outcd > ~/c") == 0);
    CHECK(home_is(m, "/home/guest/c", (const uint8_t *)"/pub /home/guest", 16));
    CHECK(status_after(m, "outless > ~/l") == 0);
    size_t ll = 0;
    const uint8_t *l = home_bytes(m, "/home/guest/l", &ll);
    CHECK(l != NULL && ll > 2 && l[0] == '1' && memchr(l, '-', ll) == NULL); /* found, not -1 */
    CHECK(status_after(m, "outre > ~/r") == 0);
    l = home_bytes(m, "/home/guest/r", &ll);
    CHECK(l != NULL && ll > 4 && l[0] == '1' && memmem(l, ll, "(list (tuple 0 1))", 18) != NULL);
    CHECK(status_after(m, "outc > ~/pc") == 0);
    CHECK(home_is(m, "/home/guest/pc", (const uint8_t *)"0 a b-c", 7));
    CHECK(status_after(m, "f() { echo fn \"$1\"; }; outfn > ~/fn") == 0);
    CHECK(home_is(m, "/home/guest/fn", (const uint8_t *)"0 fn x y\n", 9));
    CHECK(status_after(m, "outmiss > ~/mi") == 0);
    l = home_bytes(m, "/home/guest/mi", &ll);
    CHECK(l != NULL && ll > 4 && memcmp(l, "127 ", 4) == 0);
    CHECK(status_after(m, "outlong 2> ~/le") == 1);
    l = home_bytes(m, "/home/guest/le", &ll);
    CHECK(l != NULL && memmem(l, ll, "Argument list too long", 22) != NULL);
    /* a run with no end stops, and the shell is itself afterwards */
    CHECK(status_after(m, "rec 2> ~/re") == 1);
    l = home_bytes(m, "/home/guest/re", &ll);
    CHECK(l != NULL && memmem(l, ll, "too deep", 8) != NULL);
    CHECK(status_after(m, "echo fine > ~/ok") == 0);
    CHECK(home_is(m, "/home/guest/ok", (const uint8_t *)"fine\n", 5));
    CHECK(m->sc.cur == NULL);
    tree_set_source(NULL, 0);
}

/* Etapa 5: files read and written a piece at a time from a script, in
   memory (the browser's home): a write that stops halfway leaves the old
   file, one written at a time, reasons instead of crashes, handles that
   belong to their run and close with it. */
static void test_script_file_handles(void) {
    mock_host h;
    roc *m = boot(&h);
    static const char wr[] = "(def h (file-open \"~/w.txt\" \"w\"))\n"
                             "(iterate (fn (i) (if (= i 3000) (list) (do (file-write h (int-text "
                             "i) \"\\n\") (+ i 1)))) 0)\n"
                             "(out-write (string (file-close h)))";
    static const char rd[] = "(def h (file-open \"~/w.txt\"))\n"
                             "(def r (iterate (fn (st) (let ((l (file-line h))) (if (is-nil l) "
                             "(list) (list (+ (nth st 0) 1) l)))) (list 0 \"\")))\n"
                             "(out-write (nth r 0) \" \" (nth r 1))";
    static const char ap[] = "(def h (file-open \"~/w.txt\" \"a\")) (file-write h \"tail\" 7) "
                             "(out-write (string (file-close h)))";
    static const char half[] =
        "(def h (file-open \"~/w.txt\" \"w\")) (file-write h \"half\") (nth (list) 1)";
    static const char implicit[] = "(file-write (file-open \"~/imp.txt\" \"w\") \"kept\")";
    static const char two[] =
        "(def a (file-open \"~/a.txt\" \"w\")) (out-write (file-open \"~/b.txt\" \"w\"))";
    static const char keeph[] = "(out-write (file-open \"~/w.txt\"))";
    static const char useh[] = "(file-line (number (in-read)))";
    static const char reasons[] =
        "(out-write (file-open \"~/nope\") \"|\" (file-open \"~/d\") \"|\" (file-open "
        "\"/pub/kutta.md\") \"|\" (file-open \"/pub/x\" \"w\") \"|\" (file-open \"~/d\" \"w\"))";
    static const char chunks[] = "(def h (file-open \"~/w.txt\"))\n"
                                 "(file-seek h 5)\n"
                                 "(out-write (file-read h 4) \"|\" (string (is-nil (file-read h "
                                 "1000000))) \"|\" (string (is-nil (file-read h))))";
    static const char innerw[] = "(file-write (file-open \"~/inner.txt\" \"w\") \"from inner\")";
    static const char outerw[] = "(letv (st o e) (run (list \"innerw\")) (out-write st))";
    static const char innerbad[] =
        "(file-write (file-open \"~/innerbad.txt\" \"w\") \"x\") (nth (list) 1)";
    static const char outerbad[] = "(letv (st o e) (run (list \"innerbad\")) (out-write st))";
    const tree_file tree[] = {
        {"bin/wr.filo", (const uint8_t *)wr, sizeof(wr) - 1},
        {"bin/rd.filo", (const uint8_t *)rd, sizeof(rd) - 1},
        {"bin/ap.filo", (const uint8_t *)ap, sizeof(ap) - 1},
        {"bin/half.filo", (const uint8_t *)half, sizeof(half) - 1},
        {"bin/implicit.filo", (const uint8_t *)implicit, sizeof(implicit) - 1},
        {"bin/two.filo", (const uint8_t *)two, sizeof(two) - 1},
        {"bin/keeph.filo", (const uint8_t *)keeph, sizeof(keeph) - 1},
        {"bin/useh.filo", (const uint8_t *)useh, sizeof(useh) - 1},
        {"bin/reasons.filo", (const uint8_t *)reasons, sizeof(reasons) - 1},
        {"bin/chunks.filo", (const uint8_t *)chunks, sizeof(chunks) - 1},
        {"bin/innerw.filo", (const uint8_t *)innerw, sizeof(innerw) - 1},
        {"bin/outerw.filo", (const uint8_t *)outerw, sizeof(outerw) - 1},
        {"bin/innerbad.filo", (const uint8_t *)innerbad, sizeof(innerbad) - 1},
        {"bin/outerbad.filo", (const uint8_t *)outerbad, sizeof(outerbad) - 1},
    };
    tree_programs(m, tree, sizeof(tree) / sizeof(tree[0]));
    send(m, "cd\r");
    drain(m);
    CHECK(roc_cmd_mkdir(m, "/home/guest/d"));
    CHECK(status_after(m, "wr > ~/o") == 0);
    CHECK(home_is(m, "/home/guest/o", (const uint8_t *)"#t", 2));
    size_t wl = 0;
    const uint8_t *w = home_bytes(m, "/home/guest/w.txt", &wl);
    CHECK(w != NULL && wl == 13890 && memcmp(w + wl - 5, "2999\n", 5) == 0);
    CHECK(home_bytes(m, "/home/guest/w.txt.~w", &wl) == NULL); /* nothing left beside it */
    CHECK(status_after(m, "rd > ~/o") == 0);
    CHECK(home_is(m, "/home/guest/o", (const uint8_t *)"3000 2999\n", 10));
    CHECK(status_after(m, "chunks > ~/o") == 0);
    CHECK(home_is(m, "/home/guest/o", (const uint8_t *)"\n3\n4|#f|#t", 10));
    CHECK(status_after(m, "ap > ~/o") == 0);
    w = home_bytes(m, "/home/guest/w.txt", &wl);
    CHECK(w != NULL && wl == 13895 && memcmp(w + wl - 10, "2999\ntail7", 10) == 0);
    /* stopped halfway: the old file as it was */
    CHECK(status_after(m, "half 2>/dev/null") == 1);
    w = home_bytes(m, "/home/guest/w.txt", &wl);
    CHECK(w != NULL && wl == 13895);
    CHECK(home_bytes(m, "/home/guest/w.txt.~w", &wl) == NULL);
    CHECK(status_after(m, "implicit") == 0);
    CHECK(home_is(m, "/home/guest/imp.txt", (const uint8_t *)"kept", 4));
    CHECK(status_after(m, "two > ~/o") == 0);
    CHECK(home_is(m, "/home/guest/o",
                  (const uint8_t *)"Device busy: one file is written at a time here", 47));
    CHECK(status_after(m, "keeph > ~/hh") == 0);
    CHECK(status_after(m, "useh < ~/hh 2> ~/he") == 1);
    size_t el = 0;
    const uint8_t *e = home_bytes(m, "/home/guest/he", &el);
    CHECK(e != NULL && memmem(e, el, "not a file this run has open", 28) != NULL);
    CHECK(status_after(m, "reasons > ~/o") == 0);
    static const char reasons_want[] =
        "No such file or directory|Is a directory|On the site, not here: cp it home first|"
        "Read-only file system|Is a directory";
    size_t ol = 0;
    const uint8_t *ov = home_bytes(m, "/home/guest/o", &ol);
    if (ov == NULL || ol != sizeof(reasons_want) - 1 || memcmp(ov, reasons_want, ol) != 0) {
        fprintf(stderr, "reasons gave: %.*s\n", (int)ol, ov != NULL ? (const char *)ov : "");
    }
    CHECK(home_is(m, "/home/guest/o", (const uint8_t *)reasons_want, sizeof(reasons_want) - 1));
    /* a called run's files close with it: kept when it ends well, not when it fails */
    CHECK(status_after(m, "outerw > ~/o") == 0);
    CHECK(home_is(m, "/home/guest/inner.txt", (const uint8_t *)"from inner", 10));
    CHECK(status_after(m, "outerbad > ~/o") == 0);
    CHECK(home_is(m, "/home/guest/o", (const uint8_t *)"1", 1));
    CHECK(home_bytes(m, "/home/guest/innerbad.txt", &wl) == NULL);
    CHECK(!m->uf.open); /* no transfer left open behind any of it */
    tree_set_source(NULL, 0);
}

/* A utility's case: line run with its stdout and stderr to files; the
   output byte for byte, the status, and a piece of stderr when one is
   expected (NULL: stderr empty). */
static void util_case(roc *m, const char *line, const char *want, int status, const char *err) {
    char cmd[640];
    (void)snprintf(cmd, sizeof(cmd), "%s > ~/.u.out 2> ~/.u.err", line);
    int got = status_after(m, cmd);
    size_t ol = 0;
    const uint8_t *o = home_bytes(m, "/home/guest/.u.out", &ol);
    size_t el = 0;
    const uint8_t *e = home_bytes(m, "/home/guest/.u.err", &el);
    bool ok_out = o != NULL && ol == strlen(want) && memcmp(o, want, ol) == 0;
    bool ok_err = err == NULL ? el == 0 : (e != NULL && memmem(e, el, err, strlen(err)) != NULL);
    if (!ok_out || got != status || !ok_err) {
        fprintf(stderr,
                "case: %s\n  status %d (want %d)\n  out<<%.*s>>\n  want<<%s>>\n  err<<%.*s>>\n",
                line, got, status, (int)ol, o != NULL ? (const char *)o : "", want, (int)el,
                e != NULL ? (const char *)e : "");
    }
    CHECK(ok_out && got == status && ok_err);
}

static int polls;
static int stop_after;
static bool mock_interrupted(void *ctx) {
    (void)ctx;
    polls++;
    return stop_after > 0 && polls >= stop_after;
}

/* A Ctrl-C while a script runs: the host says so when asked, the script
   ends with 130, the rest of the line does not run, the next one does. */
static void test_script_interrupt(void) {
    mock_host h;
    roc *m = boot(&h);
    m->host.interrupted = mock_interrupted;
    send(m, "cd\r");
    drain(m);
    static const char loop[] = "(iterate (fn (i) (+ i 1)) 0)";
    CHECK(roc_write_file(m, "t", "/home/guest/loop.filo", (const uint8_t *)loop, sizeof(loop) - 1));
    polls = 0;
    stop_after = 3;
    send(m, "filo loop.filo; echo after; echo $?\r");
    const char *o = drain(m);
    CHECK(strstr(o, "rocchetto: filo: Interrupted") != NULL || strstr(o, "Interrupted") != NULL);
    CHECK(strstr(o, "\r\nafter") == NULL && m->status == 130);
    CHECK(polls == 3);
    /* through run: the called script stops, and the one that called it */
    static const char outer[] = "(run (list \"filo\" \"loop.filo\")) (out-write \"after\")";
    CHECK(roc_write_file(m, "t", "/home/guest/outer.filo", (const uint8_t *)outer,
                         sizeof(outer) - 1));
    polls = 0;
    stop_after = 3;
    send(m, "filo outer.filo; echo $?\r");
    o = drain(m);
    CHECK(strstr(o, "after") == NULL && strstr(o, "Interrupted") != NULL && m->status == 130);
    stop_after = 0;
    util_case(m, "echo fine", "fine\n", 0, NULL);
    /* a script that ends by itself is never stopped */
    util_case(m, "seq 1 20000 | wc -l", "20000\n", 0, NULL);
    CHECK(polls > 3);
}

/* test/fixtures/utilities.txt: the cases the WASM harness and the Fosforo
   host run too (see oracle.mjs for the fields). */
typedef struct {
    char line[256];
    char status[8];
    char err[4];
    char out[1024];
    size_t out_len;
} fixture_case;

static size_t fixture_unescape(const char *s, char *out, size_t cap) {
    size_t n = 0;
    for (; *s != '\0' && n < cap; s++) {
        if (*s != '\\') {
            out[n++] = *s;
            continue;
        }
        s++;
        if (*s == 'n') {
            out[n++] = '\n';
        } else if (*s == 't') {
            out[n++] = '\t';
        } else if (*s == 'x' && s[1] != '\0' && s[2] != '\0') {
            char hex[3] = {s[1], s[2], '\0'};
            out[n++] = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            out[n++] = *s;
        }
    }
    return n;
}

static size_t fixture_load(fixture_case *cases, size_t cap) {
    FILE *f = fopen(ROC_ROOT "/test/fixtures/utilities.txt", "rb");
    CHECK(f != NULL);
    if (f == NULL) {
        return 0;
    }
    static char raw[4096];
    size_t n = 0;
    while (n < cap && fgets(raw, sizeof(raw), f) != NULL) {
        raw[strcspn(raw, "\n")] = '\0';
        if (raw[0] == '\0' || raw[0] == '#') {
            continue;
        }
        char *fields[4] = {raw, NULL, NULL, NULL};
        for (int i = 1; i < 4; i++) {
            char *tab = fields[i - 1] != NULL ? strchr(fields[i - 1], '\t') : NULL;
            if (tab != NULL) {
                *tab = '\0';
                fields[i] = tab + 1;
            }
        }
        CHECK(fields[3] != NULL);
        if (fields[3] == NULL) {
            continue;
        }
        fixture_case *c = &cases[n++];
        (void)snprintf(c->line, sizeof(c->line), "%s",
                       fields[0][0] == '!' ? fields[0] + 1 : fields[0]);
        (void)snprintf(c->status, sizeof(c->status), "%s", fields[1]);
        (void)snprintf(c->err, sizeof(c->err), "%s", fields[2]);
        c->out_len = fixture_unescape(fields[3], c->out, sizeof(c->out));
    }
    fclose(f);
    return n;
}

static void fixture_escape(const uint8_t *d, size_t n, char *out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; i < n && k + 5 < cap; i++) {
        if (d[i] == '\n') {
            k += (size_t)snprintf(out + k, cap - k, "\\n");
        } else if (d[i] == '\t') {
            k += (size_t)snprintf(out + k, cap - k, "\\t");
        } else if (d[i] == '\\') {
            k += (size_t)snprintf(out + k, cap - k, "\\\\");
        } else if (d[i] < 32 || d[i] > 126) {
            k += (size_t)snprintf(out + k, cap - k, "\\x%02x", d[i]);
        } else {
            out[k++] = (char)d[i];
        }
    }
    out[k] = '\0';
}

/* Whether a case's line names the utility, as a word of its own: the home
   holds 64 files, so only the sources the cases run go there. */
static bool fixture_uses(const fixture_case *cases, size_t n, const char *name, size_t len) {
    for (size_t i = 0; i < n; i++) {
        for (const char *at = strstr(cases[i].line, name); at != NULL; at = strstr(at + 1, name)) {
            char before = at == cases[i].line ? ' ' : at[-1];
            char after = at[len];
            if (strchr(" ;|&(!{`$", before) != NULL &&
                (after == '\0' || strchr(" ;|&)}`", after) != NULL)) {
                return true;
            }
        }
    }
    return false;
}

/* Each case in an empty ~/fx, by /bin (bytecode) and then by the sources
   (the site's /pub/filo/examples), put in ~/src, through an alias for each utility. */
static void test_fixtures(void) {
    static fixture_case cases[256];
    size_t n = fixture_load(cases, sizeof(cases) / sizeof(cases[0]));
    CHECK(n > 100);
    for (int source = 0; source < 2; source++) {
        mock_host h;
        roc *m = boot(&h);
        if (source == 1) {
            send(m, "mkdir ~/src\r");
            drain(m);
            DIR *d = opendir(ROC_ROOT "/commands/bin");
            CHECK(d != NULL);
            const struct dirent *e = NULL;
            while (d != NULL && (e = readdir(d)) != NULL) {
                size_t len = strlen(e->d_name);
                char name[64];
                (void)snprintf(name, sizeof(name), "%.*s", (int)(len > 5 ? len - 5 : 0), e->d_name);
                if (len > 5 && strcmp(e->d_name + len - 5, ".filo") == 0 &&
                    fixture_uses(cases, n, name, strlen(name))) {
                    static uint8_t body[65536];
                    size_t blen = command_source(e->d_name, body, sizeof(body));
                    char home[160];
                    (void)snprintf(home, sizeof(home), "/home/guest/src/%s", e->d_name);
                    CHECK(blen > 0 && roc_write_file(m, "test", home, body, blen));
                    char alias[160];
                    (void)snprintf(alias, sizeof(alias), "alias %.*s='filo ~/src/%s'\r",
                                   (int)(len - 5), e->d_name, e->d_name);
                    send(m, alias);
                    drain(m);
                }
            }
            if (d != NULL) {
                closedir(d);
            }
        }
        int failed = 0;
        for (size_t i = 0; i < n; i++) {
            const fixture_case *c = &cases[i];
            send(m, "cd; rm -rf ~/fx; mkdir ~/fx; cd ~/fx\r");
            drain(m);
            char cmd[512];
            (void)snprintf(cmd, sizeof(cmd),
                           "{ %s; } > ~/.fx.out 2> ~/.fx.err; echo $? > ~/.fx.st\r", c->line);
            send(m, cmd);
            drain(m);
            size_t ol = 0;
            size_t el = 0;
            size_t sl = 0;
            const uint8_t *o = home_bytes(m, "/home/guest/.fx.out", &ol);
            const uint8_t *e = home_bytes(m, "/home/guest/.fx.err", &el);
            const uint8_t *st = home_bytes(m, "/home/guest/.fx.st", &sl);
            int got = st == NULL ? -1 : atoi((const char *)st);
            bool ok_status = strcmp(c->status, "+") == 0 ? got > 0 : got == atoi(c->status);
            bool ok_err = (el > 0) == (strcmp(c->err, "+") == 0);
            bool ok_out = o != NULL && ol == c->out_len && memcmp(o, c->out, ol) == 0;
            if (!ok_status || !ok_err || !ok_out) {
                static char shown[2048];
                fixture_escape(o, o != NULL ? ol : 0, shown, sizeof(shown));
                fprintf(stderr, "fixture (%s): %s\t%d\t%s\t%s\n",
                        source == 1 ? "source" : "bytecode", c->line, got, el > 0 ? "+" : "-",
                        shown);
                if (el > 0) {
                    fprintf(stderr, "  stderr: %.*s\n", (int)(el > 200 ? 200 : el),
                            (const char *)e);
                }
                failed++;
            }
        }
        CHECK(failed == 0);
    }
}

/* Every example of lib/bin/filo_api.md, a filo block and then what it
   writes, run as it is: the reference cannot drift from what runs. */
static void test_api_examples(void) {
    FILE *f = fopen(ROC_ROOT "/lib/bin/filo_api.md", "rb");
    CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    static char doc[32768];
    size_t len = fread(doc, 1, sizeof(doc) - 1, f);
    fclose(f);
    doc[len] = '\0';
    mock_host h;
    roc *m = boot(&h);
    int examples = 0;
    const char *at = doc;
    while ((at = strstr(at, "```filo\n")) != NULL) {
        const char *code = at + 8;
        const char *code_end = strstr(code, "```\n");
        CHECK(code_end != NULL && strncmp(code_end + 4, "```\n", 4) == 0);
        if (code_end == NULL || strncmp(code_end + 4, "```\n", 4) != 0) {
            break;
        }
        const char *want = code_end + 8;
        const char *want_end = strstr(want, "```");
        CHECK(want_end != NULL);
        if (want_end == NULL) {
            break;
        }
        CHECK(roc_write_file(m, "t", "/home/guest/example.filo", (const uint8_t *)code,
                             (size_t)(code_end - code)));
        static char expect[1024];
        (void)snprintf(expect, sizeof(expect), "%.*s", (int)(want_end - want), want);
        util_case(m, "filo ~/example.filo", expect, 0, NULL);
        examples++;
        at = want_end + 3;
    }
    CHECK(examples >= 7);
}

/* A spool of the test's: memory, counted, so a | larger than the shell
   holds is seen going through and nothing is left behind. */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} test_spool;
static int spools_live;

static void *ts_new(void *ctx) {
    (void)ctx;
    test_spool *t = calloc(1, sizeof(*t));
    if (t != NULL) {
        spools_live++;
    }
    return t;
}

static bool ts_write(void *ctx, void *s, const uint8_t *data, size_t n) {
    (void)ctx;
    test_spool *t = s;
    if (t->len + n > t->cap) {
        size_t cap = (t->len + n) * 2;
        uint8_t *bigger = realloc(t->data, cap);
        if (bigger == NULL) {
            return false;
        }
        t->data = bigger;
        t->cap = cap;
    }
    memcpy(t->data + t->len, data, n);
    t->len += n;
    return true;
}

static long long ts_read(void *ctx, void *s, uint64_t off, uint8_t *buf, size_t n) {
    (void)ctx;
    const test_spool *t = s;
    if (off >= t->len) {
        return 0;
    }
    size_t k = t->len - (size_t)off < n ? t->len - (size_t)off : n;
    memcpy(buf, t->data + off, k);
    return (long long)k;
}

static void ts_free(void *ctx, void *s) {
    (void)ctx;
    test_spool *t = s;
    free(t->data);
    free(t);
    spools_live--;
}

/* A | past ROC_CAP_MAX: spooled, read whole by the Filo utilities and by
   cat, refused by what needs it in memory; none left after a line. */
static void test_pipe_spool(void) {
    mock_host h;
    roc *m = boot(&h);
    m->host.spool_new = ts_new;
    m->host.spool_write = ts_write;
    m->host.spool_read = ts_read;
    m->host.spool_free = ts_free;
    send(m, "cd\r");
    drain(m);
    util_case(m, "seq 1 100000 | wc -l", "100000\n", 0, NULL);
    CHECK(spools_live == 0);
    util_case(m, "seq 1 100000 | cat | tail -n 1", "100000\n", 0, NULL);
    util_case(m, "seq 1 100000 | grep -c 9", "40951\n", 0, NULL);
    util_case(m, "seq 1 70000 | tee ~/te3 | tail -n 1", "70000\n", 0, NULL);
    util_case(m, "tail -n 1 ~/te3", "70000\n", 0, NULL);
    util_case(m, "seq 1 100000 | head -n 2", "1\n2\n", 0, NULL);
    util_case(m, "x=$(seq 1 100000 | wc -l); echo $x", "100000\n", 0, NULL);
    util_case(m, "seq 1 100000 | sed -n '$p'", "100000\n", 0,
              NULL); /* sed is Filo: it reads it all */
    util_case(m, "seq 1 100000 | less", "", 1, "Input too large to hold whole here");
    CHECK(roc_write_file(
        m, "t", "/home/guest/rs.filo",
        (const uint8_t *)"(letv (st o e) (run (list \"sh\" \"-c\" \"seq 1 100000 | wc "
                         "-l\")) (out-write o))",
        75));
    util_case(m, "filo rs.filo", "100000\n", 0, NULL);
    util_case(m, "seq 1 100000 | filo rs.filo | wc -l", "1\n", 0, NULL);
    CHECK(spools_live == 0);
    /* without a spool the | holds ROC_CAP_MAX, and says so past it */
    m->host.spool_new = NULL;
    util_case(m, "seq 1 100000 | wc -l", "45541\n", 1, NULL); /* the part that fit, and $? 1 */
}

/* sed, now in Filo: the commands POSIX names, against macOS's. */
static void test_filo_sed(void) {
    mock_host h;
    roc *m = boot(&h);
    send(m, "cd\r");
    drain(m);
    CHECK(roc_write_file(m, "t", "/home/guest/s4", (const uint8_t *)"a\nb\nc\nd\n", 8));
    CHECK(roc_write_file(m, "t", "/home/guest/ins", (const uint8_t *)"x\n", 2));
    CHECK(roc_write_file(m, "t", "/home/guest/sc", (const uint8_t *)"s/a/A/\ns/d/D/\n", 14));
    util_case(m, "sed '$!N;s/\\n/ /' s4", "a b\nc d\n", 0, NULL);
    util_case(m, "sed -n '1!G;h;$p' s4", "d\nc\nb\na\n", 0, NULL);
    util_case(m, "sed '/b/,/c/d' s4", "a\nd\n", 0, NULL);
    util_case(m, "sed -n '2{p;q;}' s4", "b\n", 0, NULL);
    util_case(m, "sed '2r ins' s4", "a\nb\nx\nc\nd\n", 0, NULL);
    util_case(m, "sed -n 'H;${x;s/\\n/,/g;p;}' s4", ",a,b,c,d\n", 0, NULL);
    util_case(m, "sed -f sc s4", "A\nb\nc\nD\n", 0, NULL);
    util_case(m, "sed -e 's/a/1/' -e 's/b/2/' s4", "1\n2\nc\nd\n", 0, NULL);
    util_case(m, "printf 'one two\\n' | sed -e ':a' -e 's/o/0/' -e 'ta'", "0ne tw0\n", 0, NULL);
    util_case(m, "sed -n '/b/{=;p;}' s4", "2\nb\n", 0, NULL);
    util_case(m, "sed '2,3!d' s4", "b\nc\n", 0, NULL);
    util_case(m, "printf 'a\\tb\\001\\n' | sed -n l", "a\\tb\\001$\n", 0, NULL);
    util_case(m, "sed -n '/a/,/c/{/c/!p;}' s4", "a\nb\n", 0, NULL);
    util_case(m, "sed 's/b/B/w wout' s4 > /dev/null; cat wout", "B\n", 0, NULL);
    util_case(m, "printf 'hello\\n' | sed 'y/el/ip/'", "hippo\n", 0, NULL);
    util_case(m, "sed 3q s4", "a\nb\nc\n", 0, NULL);
    util_case(m, "sed -n '$=' s4", "4\n", 0, NULL);
    util_case(m, "printf 'a b\\n' | sed -E 's/(a) (b)/\\2 \\1/'", "b a\n", 0, NULL);
    util_case(m, "printf '1\\n2\\n3\\n4\\n5\\n' | sed -n '2,+1p'", "2\n3\n", 0, NULL);
    util_case(m, "printf 'a\\n\\nb\\n' | sed '/^$/d'", "a\nb\n", 0, NULL);
    util_case(m, "printf 'a\\nb\\n' | sed 'N;P;D'", "a\n", 0, NULL);
    util_case(m, "printf 'a\\n' | sed N", "", 0, NULL);
    util_case(m, "printf 'a\\n' | sed 'a\\\nadded'", "a\nadded\n", 0, NULL);
    util_case(m, "printf 'x\\n' | sed 's/x/a\\\nb/'", "a\nb\n", 0, NULL);
    util_case(m, "sed -i '1d' s4; cat s4", "b\nc\nd\n", 0, NULL);
    util_case(m, "sed 'bnowhere' s4", "", 1, "can't find label for jump to nowhere");
    util_case(m, "sed '}' s4", "", 1, "unexpected }");
    util_case(m, "sed '{p' s4", "", 1, "unmatched {");
    util_case(m, "sed y/ab/c/ s4", "", 1, "y strings differ in length");
    util_case(m, "sed -n p nope s4", "b\nc\nd\n", 1, "sed: nope: No such file or directory");
}

/* head, tail, wc, grep, sort, uniq, cut, tr, cmp, now in Filo: what
   macOS's give (LC_ALL=C) on the same input. */
static void test_filo_classics(void) {
    mock_host h;
    roc *m = boot(&h);
    send(m, "cd\r");
    drain(m);
    CHECK(roc_write_file(m, "t", "/home/guest/ha", (const uint8_t *)"1\n2\n3\n", 6));
    CHECK(roc_write_file(m, "t", "/home/guest/hb", (const uint8_t *)"x\ny", 3));
    CHECK(roc_write_file(m, "t", "/home/guest/s1", (const uint8_t *)"b 2\na 10\nB 1\na 9\n", 17));
    CHECK(roc_write_file(m, "t", "/home/guest/c1", (const uint8_t *)"a:b:c\nnodelim\nx:y\n", 18));
    CHECK(roc_write_file(m, "t", "/home/guest/p", (const uint8_t *)"ab\ncd\n", 6));
    CHECK(roc_write_file(m, "t", "/home/guest/q", (const uint8_t *)"ab\ncx\n", 6));

    /* head and tail */
    util_case(m, "head -n 2 ha hb", "==> ha <==\n1\n2\n\n==> hb <==\nx\ny", 0, NULL);
    util_case(m, "head -c 3 ha hb", "==> ha <==\n1\n2\n==> hb <==\nx\ny", 0, NULL);
    util_case(m, "head -2 ha", "1\n2\n", 0, NULL);
    util_case(m, "head nope ha", "==> ha <==\n1\n2\n3\n", 1,
              "head: nope: No such file or directory");
    util_case(m, "tail -n 1 ha hb", "==> ha <==\n3\n\n==> hb <==\ny", 0, NULL);
    util_case(m, "tail -n +2 ha", "2\n3\n", 0, NULL);
    util_case(m, "tail -c 3 ha", "\n3\n", 0, NULL);
    util_case(m, "seq 1 20 | tail -3", "18\n19\n20\n", 0, NULL);
    util_case(m, "tail -f ha", "", 1, "tail: -f is not supported here");

    /* wc */
    util_case(m, "wc ha hb", "3 3 6 ha\n1 2 3 hb\n4 5 9 total\n", 0, NULL);
    util_case(m, "printf 'caf\\303\\251 x\\n' | wc -m", "7\n", 0, NULL);
    util_case(m, "printf 'a\\tb  c\\n\\nd' | wc -lw", "2 4\n", 0, NULL);
    util_case(m, "wc -l nope ha", "3 ha\n3 total\n", 1, "wc: nope: No such file or directory");

    /* grep */
    CHECK(roc_write_file(m, "t", "/home/guest/g1", (const uint8_t *)"a\nb\n", 4));
    CHECK(roc_write_file(m, "t", "/home/guest/g2", (const uint8_t *)"b\nc", 3));
    util_case(m, "grep b g1 g2", "g1:b\ng2:b\n", 0, NULL);
    util_case(m, "grep -c b g1 g2", "g1:1\ng2:1\n", 0, NULL);
    util_case(m, "grep -l b g1 g2 nonex", "g1\ng2\n", 2, "grep: nonex: No such file or directory");
    util_case(m, "grep -n c g2", "2:c\n", 0, NULL);
    util_case(m, "echo b | grep -H b", "(standard input):b\n", 0, NULL);
    util_case(m, "echo b | grep -l b", "(standard input)\n", 0, NULL);
    util_case(m, "grep -h b g1 g2", "b\nb\n", 0, NULL);
    util_case(m, "grep -q b nonex g1", "", 0, "grep: nonex: No such file or directory");
    util_case(m, "grep -s b nonex", "", 2, NULL);
    util_case(m, "grep '[' g1", "", 2, "grep: ");
    util_case(m, "printf 'ab\\nAB\\n' | grep -ix ab", "ab\nAB\n", 0, NULL);
    util_case(m, "printf 'a.b\\naxb\\n' | grep -F a.b", "a.b\n", 0, NULL);
    util_case(m, "printf 'x\\ny\\nz\\n' | grep -e x -e z", "x\nz\n", 0, NULL);
    util_case(m, "printf 'x\\n' | grep ''", "x\n", 0, NULL);
    util_case(m, "printf 'x\\ny\\n' | grep -v x", "y\n", 0, NULL);
    util_case(m, "printf 'x\\ny\\n' | grep z", "", 1, NULL);
    util_case(m, "printf 'x\\n' > pats; printf 'y\\nx\\n' | grep -f pats", "x\n", 0, NULL);

    /* sort */
    util_case(m, "sort s1", "B 1\na 10\na 9\nb 2\n", 0, NULL);
    util_case(m, "sort -f s1", "a 10\na 9\nB 1\nb 2\n", 0, NULL);
    util_case(m, "sort -k2 -n s1", "B 1\nb 2\na 9\na 10\n", 0, NULL);
    util_case(m, "sort -t' ' -k2,2n -r s1", "B 1\nb 2\na 9\na 10\n", 0,
              NULL); /* a key's letters rule */
    util_case(m, "sort -r s1", "b 2\na 9\na 10\nB 1\n", 0, NULL);
    util_case(m, "sort -u -k1,1 s1", "B 1\na 10\nb 2\n", 0, NULL);
    util_case(m, "printf '3\\n1\\n2\\n' | sort -c", "", 1, "sort: -:2: disorder: 1");
    util_case(m, "printf '1\\n2\\n' | sort -c", "", 0, NULL);
    util_case(m, "printf ' x\\nx\\n y\\n' | sort -b", " x\nx\n y\n", 0, NULL);
    util_case(m, "printf '1.5\\n-2\\n10\\nabc\\n' | sort -n", "-2\nabc\n1.5\n10\n", 0, NULL);
    util_case(m, "sort -o s1 s1 && cat s1", "B 1\na 10\na 9\nb 2\n", 0, NULL);
    util_case(m, "sort -k 0 s1", "", 2, "sort: -k field");

    /* uniq */
    util_case(m, "printf 'a\\na\\nb\\nA\\na\\n' | uniq -c", "   2 a\n   1 b\n   1 A\n   1 a\n", 0,
              NULL);
    util_case(m, "printf 'a\\na\\nb\\n' | uniq -d", "a\n", 0, NULL);
    util_case(m, "printf 'a\\na\\nb\\n' | uniq -u", "b\n", 0, NULL);
    util_case(m, "printf 'x a\\ny a\\nz b\\n' | uniq -f 1", "x a\nz b\n", 0, NULL);
    util_case(m, "printf 'xa\\nya\\n' | uniq -s 1", "xa\n", 0, NULL);
    util_case(m, "printf 'a\\na\\n' > u1; uniq u1 u2 && cat u2", "a\n", 0, NULL);

    /* cut */
    util_case(m, "cut -d: -f2 c1", "b\nnodelim\ny\n", 0, NULL);
    util_case(m, "cut -d: -f1,3 -s c1", "a:c\nx\n", 0, NULL);
    util_case(m, "cut -c2-3 c1", ":b\nod\n:y\n", 0, NULL);
    util_case(m, "cut -b -2 c1", "a:\nno\nx:\n", 0, NULL);
    util_case(m, "cut -d: -f2- c1", "b:c\nnodelim\ny\n", 0, NULL);
    util_case(m, "cut -f 0 c1", "", 1, "cut: a list is");

    /* tr */
    util_case(m, "printf 'hello\\n' | tr a-y b-z", "ifmmp\n", 0, NULL);
    util_case(m, "printf 'aabbcc\\n' | tr -s ab", "abcc\n", 0, NULL);
    util_case(m, "printf 'abc\\n' | tr -d b", "ac\n", 0, NULL);
    util_case(m, "printf 'abc 123\\n' | tr -cd '[:digit:]\\n'", "123\n", 0, NULL);
    util_case(m, "printf 'abc\\n' | tr a-c x", "xxx\n", 0, NULL);
    util_case(m, "printf 'aaa\\n' | tr -s a x", "x\n", 0, NULL);
    util_case(m, "printf 'abcd\\n' | tr '[:lower:]' '[:upper:]'", "ABCD\n", 0, NULL);
    util_case(m, "printf 'a\\n' | tr aa xy", "y\n", 0, NULL);
    util_case(m, "printf 'abc\\n' | tr 'a[b*]' 'xy'", "xyc\n", 0, NULL);
    util_case(m, "printf 'ab\\n' | tr z-a x", "", 1, "tr: a range backwards");

    /* cmp */
    util_case(m, "cmp p q", "p q differ: char 5, line 2\n", 1, NULL);
    CHECK(roc_write_file(m, "t", "/home/guest/r", (const uint8_t *)"ab", 2));
    util_case(m, "cmp p r", "", 1, "cmp: EOF on r");
    util_case(m, "cmp -l p q", "     5 144 170\n", 1, NULL);
    util_case(m, "cmp -s p q", "", 1, NULL);
    util_case(m, "cmp p p", "", 0, NULL);
    util_case(m, "cmp p nonexist", "", 2, "cmp: nonexist: No such file or directory");
    util_case(m, "cat p | cmp - p", "", 0, NULL);

    /* (range 0 n) holds no items: each builtin that takes a list asks for them */
    static const char rg[] =
        "(out-write (int-text (byte-len (bytes (range 0 3)))) \" \" (str-join "
        "\",\" (map int-text (list-sort (range 0 4) (fn (a b) (- b a))))) \" \" "
        "(utf8-encode (range 0 1)) \"\\n\")";
    CHECK(roc_write_file(m, "t", "/home/guest/rg.filo", (const uint8_t *)rg, sizeof(rg) - 1));
    util_case(m, "filo rg.filo | tr '\\000' Z", "3 3,2,1,0 Z\n", 0, NULL);
    util_case(m, "printf '(run (range 0 2))' > rr.filo; filo rr.filo", "", 1,
              "run: each word is a string");

    /* a pipe of 108 KB through each: memory and steps for real input */
    util_case(m, "seq 1 20000 | sort -rn | head -n 3", "20000\n19999\n19998\n", 0, NULL);
    util_case(m, "seq 1 20000 | grep -c 7", "6878\n", 0, NULL);
    util_case(m, "seq 1 20000 | sort | tail -1", "9999\n", 0, NULL);
    util_case(m, "seq 1 20000 | cut -c1-2 | sort -u | wc -l", "99\n", 0, NULL);
    util_case(m, "seq 1 20000 | tr 0-9 a-j | tail -1", "caaaa\n", 0, NULL);
    util_case(m, "seq 1 20000 | uniq | wc -l", "20000\n", 0, NULL);
}

/* rm, mv and cp: many operands, -r, -f, -p, into a directory. */
static void test_filo_rm_mv_cp(void) {
    mock_host h;
    roc *m = boot(&h);
    m->host.clock = test_clock;
    send(m, "cd\r");
    drain(m);
    send(m, "mkdir -p t/a/b\r");
    drain(m);
    CHECK(roc_write_file(m, "t", "/home/guest/t/one", (const uint8_t *)"1\n", 2));
    CHECK(roc_write_file(m, "t", "/home/guest/t/a/two", (const uint8_t *)"22\n", 3));
    CHECK(roc_write_file(m, "t", "/home/guest/t/a/b/three", (const uint8_t *)"333\n", 4));

    util_case(m, "cp -R t u && find u", "u\nu/a\nu/a/b\nu/a/b/three\nu/a/two\nu/one\n", 0, NULL);
    util_case(m, "cksum u/a/b/three t/a/b/three | cut -d' ' -f1,2", "3298658159 4\n3298658159 4\n",
              0, NULL);
    util_case(m, "cp t u2", "", 1, "cp: t is a directory (not copied)");
    util_case(m, "cp -R t t/a", "", 1, "cannot copy a directory into itself");
    util_case(m, "mkdir v && cp t/one t/a/two v && find v", "v\nv/one\nv/two\n", 0, NULL);
    util_case(m, "cp t/one t/a/two nodir", "", 1, "cp: nodir: Not a directory");
    util_case(m, "cp nope v", "", 1, "cp: nope: No such file or directory");
    util_case(m,
              "touch -t 202001010000 t/one && cp -p t/one p1 && cp t/one p2 && find . -name 'p*' "
              "-newer t/one",
              "./p2\n", 0, NULL);
    util_case(m, "cp -i t/one x", "", 1, "cp: -i is not supported here");

    util_case(m, "mkdir w && mv p1 p2 w && find w", "w\nw/p1\nw/p2\n", 0, NULL);
    util_case(m, "mv w/p1 w/p3 && find w", "w\nw/p2\nw/p3\n", 0, NULL);
    util_case(m, "mv w/p2 w/p3 nodir", "", 1, "mv: nodir: Not a directory");
    util_case(m, "mv", "", 1, "usage: mv");

    util_case(m, "rm u", "", 1, "rm: u: is a directory");
    util_case(m, "rm -r u v w && find u v w", "", 1, "find: u: No such file or directory");
    util_case(m, "rm -f nope1 nope2", "", 0, NULL);
    util_case(m, "rm nope1 t/one", "", 1, "rm: nope1: No such file or directory");
    util_case(m, "find t/one", "", 1, "No such file or directory");
    util_case(m, "rm -r t/a/.", "", 1, "may not be removed");
    util_case(m, "rm -R t && find t", "", 1, "find: t: No such file or directory");
    util_case(m, "rm -f", "", 0, NULL);
}

/* touch, with the test clock (1790517909, 180 minutes west): the times
   macOS's touch gives for the same arguments in that zone. */
static void test_filo_touch(void) {
    mock_host h;
    roc *m = boot(&h);
    m->host.clock = test_clock;
    send(m, "cd\r");
    drain(m);
    static const char mt[] = "(map (fn (f) (let ((s (file-stat f))) (if (is-nil s) (out-write f "
                             "\" none\\n\") (letv (k z t o) s (out-write f \" \" (if (is-nil t) "
                             "\"-\" (int-text t)) \"\\n\"))))) ARGS)";
    CHECK(roc_write_file(m, "t", "/home/guest/mt.filo", (const uint8_t *)mt, sizeof(mt) - 1));

    util_case(m, "touch n1 && filo mt.filo n1", "n1 1790517909\n", 0, NULL);
    util_case(m, "touch -t 202601021530.45 n1 && filo mt.filo n1", "n1 1767378645\n", 0, NULL);
    util_case(m, "touch -t 2601021530 n1 && filo mt.filo n1", "n1 1767378600\n", 0, NULL);
    util_case(m, "touch -t 01021530 n1 && filo mt.filo n1", "n1 1767378600\n", 0, NULL);
    util_case(m, "touch -t 6901010000 n1 && filo mt.filo n1", "n1 -31525200\n", 0, NULL);
    util_case(m, "touch -t 6812312359 n1 && filo mt.filo n1", "n1 3124234740\n", 0, NULL);
    util_case(m, "touch -d 2026-01-02T15:30:45Z n1 && filo mt.filo n1", "n1 1767367845\n", 0, NULL);
    util_case(m, "touch -d '2026-01-02 15:30:45.5' n1 && filo mt.filo n1", "n1 1767378645\n", 0,
              NULL);
    util_case(m, "touch -r n1 n2 n3 && filo mt.filo n2 n3", "n2 1767378645\nn3 1767378645\n", 0,
              NULL);
    util_case(m, "touch -c nope && filo mt.filo nope", "nope none\n", 0, NULL);
    util_case(m, "touch new && find . -newer n2 -name 'n*'", "./new\n", 0, NULL);
    util_case(m, "mkdir dd && touch dd", "", 1, "touch: dd: Operation not supported");
    util_case(m, "touch -a n1", "", 1, "touch: -a is not supported here");
    util_case(m, "touch -t 202613011200 n1", "", 1, "illegal time specification: 202613011200");
    util_case(m, "touch -t 202602301200 n1", "", 1, "illegal time specification");
    util_case(m, "touch -d 2026-01-02 n1", "", 1, "illegal time specification");
    util_case(m, "touch -r nope n1", "", 1, "touch: nope: No such file or directory");
    util_case(m, "touch -r n1 -t 202001010000 n1", "", 1, "touch: one of -r, -t and -d");
    util_case(m, "touch", "", 1, "usage: touch");
    util_case(m, "touch -m n9 && filo mt.filo n9", "n9 1790517909\n", 0, NULL);
}

/* find, over a tree made for it; directories in byte order. */
static void test_filo_find(void) {
    mock_host h;
    roc *m = boot(&h);
    send(m, "cd\r");
    drain(m);
    static uint8_t big[600];
    memset(big, 'x', sizeof(big));
    send(m, "mkdir -p d/sub/deep\r");
    drain(m);
    CHECK(roc_write_file(m, "t", "/home/guest/d/.hid", (const uint8_t *)"", 0));
    CHECK(roc_write_file(m, "t", "/home/guest/d/a.txt", (const uint8_t *)"hello", 5));
    CHECK(roc_write_file(m, "t", "/home/guest/d/b.log", big, sizeof(big)));
    CHECK(roc_write_file(m, "t", "/home/guest/d/sub/c.txt", (const uint8_t *)"c\n", 2));
    CHECK(roc_write_file(m, "t", "/home/guest/d/sub/deep/e.txt", (const uint8_t *)"e\n", 2));

    util_case(m, "find d",
              "d\nd/.hid\nd/a.txt\nd/b.log\nd/sub\nd/sub/c.txt\nd/sub/deep\nd/sub/deep/e.txt\n", 0,
              NULL);
    util_case(m, "find d -name '*.txt'", "d/a.txt\nd/sub/c.txt\nd/sub/deep/e.txt\n", 0, NULL);
    util_case(m, "find d -type d", "d\nd/sub\nd/sub/deep\n", 0, NULL);
    util_case(m, "find d -name sub -prune -o -print", "d\nd/.hid\nd/a.txt\nd/b.log\n", 0, NULL);
    util_case(m, "find d -depth -type d", "d/sub/deep\nd/sub\nd\n", 0, NULL);
    util_case(m, "find d ! -name '*.txt' -type f", "d/.hid\nd/b.log\n", 0, NULL);
    util_case(m, "find d \\( -name a.txt -o -name b.log \\) -print", "d/a.txt\nd/b.log\n", 0, NULL);
    util_case(m, "find d -path 'd/sub/*' -type f", "d/sub/c.txt\nd/sub/deep/e.txt\n", 0, NULL);
    util_case(m, "find d -type f -size 2", "d/b.log\n", 0, NULL);
    util_case(m, "find d -type f -size -1", "d/.hid\n", 0, NULL);
    util_case(m, "find d -type f -size +4c -size -6c", "d/a.txt\n", 0, NULL);
    util_case(m, "find d -type f -exec test -s {} \\; -print",
              "d/a.txt\nd/b.log\nd/sub/c.txt\nd/sub/deep/e.txt\n", 0, NULL);
    util_case(m, "find d -name '*.txt' -exec echo got {} \\;",
              "got d/a.txt\ngot d/sub/c.txt\ngot d/sub/deep/e.txt\n", 0, NULL);
    util_case(m, "find d -name '*.txt' -exec echo {} +", "d/a.txt d/sub/c.txt d/sub/deep/e.txt\n",
              0, NULL);
    util_case(m, "find d -name '*.txt' -exec test -z {} +", "", 1, "unexpected operator");
    util_case(m, "find d/ -name a.txt", "d/a.txt\n", 0, NULL);
    util_case(m, "find nope d -name a.txt", "d/a.txt\n", 1,
              "find: nope: No such file or directory");
    util_case(m, "find d -mtime 0", "", 0, NULL); /* the home keeps no times yet */
    util_case(m, "find d -bogus", "", 1, "find: -bogus: unknown primary or operator");
    util_case(m, "find d -name", "", 1, "find: -name: requires additional arguments");
    util_case(m, "find d -exec echo", "", 1, "no terminating");
    util_case(m, "find d -newer nope", "", 1, "find: nope: No such file or directory");
    util_case(m, "find d \\( -name a.txt", "", 1, "find: ( without )");
    util_case(m, "find d -type q", "", 1, "find: -type: q: unknown type");
    util_case(m, "find", "", 1, "usage: find");

    /* mkdir and rmdir, -p and many at once */
    util_case(m, "mkdir -p d/sub/deep && mkdir -p m/n/o m/p && find m", "m\nm/n\nm/n/o\nm/p\n", 0,
              NULL);
    util_case(m, "mkdir m", "", 1, "mkdir: m: File exists");
    util_case(m, "mkdir -p d/a.txt/x", "", 1, "mkdir: d/a.txt: Not a directory");
    util_case(m, "mkdir -m 755 q", "", 1, "mkdir: -m is not supported here");
    util_case(m, "mkdir q1 nope/q2 q3", "", 1, "nope/q2");
    util_case(m, "find q1 q3", "q1\nq3\n", 0, NULL);
    util_case(m, "rmdir -p m/n/o", "", 1, "Directory not empty");
    util_case(m, "find m", "m\nm/p\n", 0, NULL);
    util_case(m, "rmdir m/p m q1 q3 && find m q1", "", 1, "find: m: No such file or directory");
    util_case(m, "mkdir", "", 1, "usage: mkdir [-p] dir...");
    util_case(m, "rmdir", "", 1, "usage: rmdir [-p] dir...");
    util_case(m, "find d -name '*.txt' | xargs cksum",
              "3287646509 5 d/a.txt\n2475711845 2 d/sub/c.txt\n2537797239 2 d/sub/deep/e.txt\n", 0,
              NULL);
}

/* Etapa 8: the utilities written in Filo, against what POSIX says and
   what macOS's give on the same input. */
static void test_filo_utilities(void) {
    mock_host h;
    roc *m = boot(&h);
    send(m, "cd\r");
    drain(m);
    CHECK(roc_write_file(m, "t", "/home/guest/a.txt", (const uint8_t *)"hello\n", 6));
    CHECK(roc_write_file(m, "t", "/home/guest/e.txt", (const uint8_t *)"", 0));

    /* cksum */
    util_case(m, "cksum a.txt e.txt", "3015617425 6 a.txt\n4294967295 0 e.txt\n", 0, NULL);
    util_case(m, "printf 'hello\\n' | cksum", "3015617425 6\n", 0, NULL);
    util_case(m, "cksum nope.txt a.txt", "3015617425 6 a.txt\n", 1,
              "cksum: nope.txt: No such file or directory");

    /* paste */
    CHECK(roc_write_file(m, "t", "/home/guest/p1", (const uint8_t *)"a\nb\nc\n", 6));
    CHECK(roc_write_file(m, "t", "/home/guest/p2", (const uint8_t *)"1\n2\n", 4));
    util_case(m, "paste p1 p2", "a\t1\nb\t2\nc\t\n", 0, NULL);
    util_case(m, "paste -d, p1 p2", "a,1\nb,2\nc,\n", 0, NULL);
    util_case(m, "paste -s p1", "a\tb\tc\n", 0, NULL);
    util_case(m, "paste -s -d: p1 p2", "a:b:c\n1:2\n", 0, NULL);
    util_case(m, "printf 'x\\ny' | paste - p2", "x\t1\ny\t2\n", 0, NULL);
    util_case(m, "paste -d '\\n,' p1 p2", "a\n1\nb\n2\nc\n\n", 0, NULL);
    util_case(m, "paste -s -d ',;' p1", "a,b;c\n", 0, NULL);
    util_case(m, "paste p1 nope", "", 1, "paste: nope: No such file or directory");
    util_case(m, "paste -x p1", "", 1, "paste: bad option -x");
    CHECK(roc_write_file(m, "t", "/home/guest/n1", (const uint8_t *)"one\n\ntwo\n", 9));

    /* nl */
    util_case(m, "nl n1", "     1\tone\n      \t\n     2\ttwo\n", 0, NULL);
    util_case(m, "nl -b a n1", "     1\tone\n     2\t\n     3\ttwo\n", 0, NULL);
    util_case(m, "nl -n rz -w 3 -s: n1", "001:one\n   :\n002:two\n", 0, NULL);
    util_case(m, "nl -v 10 -i 5 n1", "    10\tone\n      \t\n    15\ttwo\n", 0, NULL);
    util_case(m, "printf a | nl", "     1\ta", 0, NULL);
    util_case(m, "printf 'x\\n  \\ny\\n' | nl", "     1\tx\n     2\t  \n     3\ty\n", 0, NULL);
    util_case(m, "printf 'ab\\n' | nl -n ln -w 4", "1   \tab\n", 0, NULL);
    util_case(m, "printf 'x\\ny\\nfoo\\n' | nl -b pf", "      \tx\n      \ty\n     1\tfoo\n", 0,
              NULL);

    /* fold */
    util_case(m, "printf 'abcdefghij\\n' | fold -w 4", "abcd\nefgh\nij\n", 0, NULL);
    util_case(m, "printf 'aa bb cc dd\\n' | fold -s -w 6", "aa bb \ncc dd\n", 0, NULL);
    util_case(m, "printf 'a\\tb\\n' | fold -w 4", "a\n\t\nb\n", 0, NULL);
    util_case(m, "printf abcdefghij | fold -w 4", "abcd\nefgh\nij", 0, NULL);
    util_case(m, "printf 'aaaa bbbbbbbbb c\\n' | fold -s -w 5", "aaaa \nbbbbb\nbbbb \nc\n", 0,
              NULL);
    util_case(m, "printf abc | fold -b -w 2", "ab\nc", 0, NULL);

    /* expand */
    util_case(m, "printf 'a\\tb\\tc\\n' | expand", "a       b       c\n", 0, NULL);
    util_case(m, "printf 'a\\tb\\n' | expand -t 4", "a   b\n", 0, NULL);
    util_case(m, "printf 'a\\tb\\tc\\n' | expand -t 2,5", "a b  c\n", 0, NULL);
    util_case(m, "printf 'abcdefgh\\tx\\n' | expand -t 2,5", "abcdefgh x\n", 0, NULL);
    util_case(m, "printf 'ab\\bc\\td\\n' | expand", "ab\bc      d\n", 0, NULL);
    util_case(m, "printf 'a\\tb\\tc\\n' | expand -t 1,2,3,4,5,6,7,8,9,10", "a b c\n", 0, NULL);

    /* comm and join */
    CHECK(roc_write_file(m, "t", "/home/guest/c1", (const uint8_t *)"a\nb\nd\n", 6));
    CHECK(roc_write_file(m, "t", "/home/guest/c2", (const uint8_t *)"b\nc\nd\ne\n", 8));
    CHECK(roc_write_file(m, "t", "/home/guest/j1", (const uint8_t *)"1 a\n2 b\n3 c\n", 12));
    CHECK(roc_write_file(m, "t", "/home/guest/j2", (const uint8_t *)"1 x\n3 y\n4 z\n", 12));
    CHECK(roc_write_file(m, "t", "/home/guest/j3", (const uint8_t *)"k:1\nm:2\n", 8));
    CHECK(roc_write_file(m, "t", "/home/guest/j4", (const uint8_t *)"k:A\nn:B\n", 8));
    CHECK(roc_write_file(m, "t", "/home/guest/j5", (const uint8_t *)"a 1\nb 2\n", 8));
    CHECK(roc_write_file(m, "t", "/home/guest/j6", (const uint8_t *)"1 X\n2 Y\n", 8));
    CHECK(roc_write_file(m, "t", "/home/guest/m1", (const uint8_t *)"a 1\na 2\n", 8));
    CHECK(roc_write_file(m, "t", "/home/guest/m2", (const uint8_t *)"a x\na y\n", 8));
    util_case(m, "comm c1 c2", "a\n\t\tb\n\tc\n\t\td\n\te\n", 0, NULL);
    util_case(m, "comm -12 c1 c2", "b\nd\n", 0, NULL);
    util_case(m, "comm -3 c1 c2", "a\n\tc\n\te\n", 0, NULL);
    util_case(m, "comm -1 c1 c2", "\tb\nc\n\td\ne\n", 0, NULL);
    util_case(m, "join j1 j2", "1 a x\n3 c y\n", 0, NULL);
    util_case(m, "join -a 1 j1 j2", "1 a x\n2 b\n3 c y\n", 0, NULL);
    util_case(m, "join -v 2 j1 j2", "4 z\n", 0, NULL);
    util_case(m, "join -v 1 j1 j2", "2 b\n", 0, NULL);
    util_case(m, "join -t : j3 j4", "k:1:A\n", 0, NULL);
    util_case(m, "join -1 2 -2 1 j5 j6", "1 a X\n2 b Y\n", 0, NULL);
    util_case(m, "join -o 1.1,2.2 j1 j2", "1 x\n3 y\n", 0, NULL);
    util_case(m, "join m1 m2", "a 1 x\na 1 y\na 2 x\na 2 y\n", 0, NULL);
    util_case(m, "join -a 1 -e EMPTY -o 0,1.2,2.2 j1 j2", "1 a x\n2 b EMPTY\n3 c y\n", 0, NULL);
    util_case(m, "comm c1", "", 1, "usage: comm");
    util_case(m, "join -1 0 j1 j2", "", 1, "join: -1 and -2 take a field number from 1");

    /* od: the layout macOS's, byte for byte */
    CHECK(roc_write_file(m, "t", "/home/guest/o1", (const uint8_t *)"ab\001\377", 4));
    {
        static const uint8_t zeros[48] = {0};
        CHECK(roc_write_file(m, "t", "/home/guest/z48", zeros, sizeof(zeros)));
    }
    util_case(
        m, "od o1",
        "0000000    061141  177401                                                \n0000004\n", 0,
        NULL);
    util_case(
        m, "od -t x1 o1",
        "0000000    61  62  01  ff                                                \n0000004\n", 0,
        NULL);
    util_case(
        m, "od -t o1 o1",
        "0000000   141 142 001 377                                                \n0000004\n", 0,
        NULL);
    util_case(
        m, "od -t u1 o1",
        "0000000    97  98   1 255                                                \n0000004\n", 0,
        NULL);
    util_case(
        m, "od -t d1 o1",
        "0000000    97  98   1  -1                                                \n0000004\n", 0,
        NULL);
    util_case(m, "od -t a o1",
              "0000000    a   b soh  ff                                                \n0000004\n",
              0, NULL);
    util_case(m, "od -t c o1",
              "0000000    a   b 001 377                                                \n0000004\n",
              0, NULL);
    util_case(
        m, "od -t x2 o1",
        "0000000      6261    ff01                                                \n0000004\n", 0,
        NULL);
    util_case(
        m, "od -t d2 o1",
        "0000000     25185    -255                                                \n0000004\n", 0,
        NULL);
    util_case(
        m, "od -t u2 o1",
        "0000000     25185   65281                                                \n0000004\n", 0,
        NULL);
    util_case(
        m, "od -t o4 o1",
        "0000000       37700261141                                                \n0000004\n", 0,
        NULL);
    util_case(
        m, "od -t x4 o1",
        "0000000          ff016261                                                \n0000004\n", 0,
        NULL);
    util_case(
        m, "od -t d4 o1",
        "0000000         -16686495                                                \n0000004\n", 0,
        NULL);
    util_case(
        m, "od -A d -t x1 o1",
        "0000000    61  62  01  ff                                                \n0000004\n", 0,
        NULL);
    util_case(
        m, "od -A x -t x1 o1",
        "0000000    61  62  01  ff                                                \n0000004\n", 0,
        NULL);
    util_case(m, "od -A n -t x1 o1",
              "           61  62  01  ff                                                \n\n", 0,
              NULL);
    util_case(
        m, "od -j 1 -t x1 o1",
        "0000001    62  01  ff                                                    \n0000004\n", 0,
        NULL);
    util_case(m, "od -N 2 -t c o1",
              "0000000    a   b                                                        \n0000002\n",
              0, NULL);
    util_case(
        m, "od -b o1",
        "0000000   141 142 001 377                                                \n0000004\n", 0,
        NULL);
    util_case(m, "od -c o1",
              "0000000    a   b 001 377                                                \n0000004\n",
              0, NULL);
    util_case(
        m, "od -d o1",
        "0000000     25185   65281                                                \n0000004\n", 0,
        NULL);
    util_case(
        m, "od -x o1",
        "0000000      6261    ff01                                                \n0000004\n", 0,
        NULL);
    util_case(m, "printf abcdefghijklmnopq | od -t x1 -t c",
              "0000000    61  62  63  64  65  66  67  68  69  6a  6b  6c  6d  6e  6f  70\n         "
              "  a   b   c   d   e   f   g   h   i   j   k   l   m   n   o   p\n0000020    71      "
              "                                                      \n           q                "
              "                                            \n0000021\n",
              0, NULL);
    util_case(
        m, "printf 'ab\\001' | od -t x2",
        "0000000      6261    0001                                                \n0000003\n", 0,
        NULL);
    util_case(m, "printf '\\303\\251\\n' | od -t c",
              "0000000  303 251  \\n                                                    "
              "\n0000003\n",
              0, NULL);
    util_case(
        m, "od -t x1 z48",
        "0000000    00  00  00  00  00  00  00  00  00  00  00  00  00  00  00  00\n*\n0000060\n",
        0, NULL);
    util_case(m, "od -v -t x1 z48",
              "0000000    00  00  00  00  00  00  00  00  00  00  00  00  00  00  00  00\n0000020  "
              "  00  00  00  00  00  00  00  00  00  00  00  00  00  00  00  00\n0000040    00  00 "
              " 00  00  00  00  00  00  00  00  00  00  00  00  00  00\n0000060\n",
              0, NULL);
    util_case(m, "od nope", "0000000\n", 1, "od: nope: No such file or directory");
    util_case(m, "od -t f4 o1", "", 1, "od: -t takes");

    /* unexpand */
    util_case(m, "printf '        a       b\\n' | unexpand", "\ta       b\n", 0, NULL);
    util_case(m, "printf '        a       b\\n' | unexpand -a", "\ta\tb\n", 0, NULL);
    util_case(m, "printf 'abcdefg b\\n' | unexpand -a", "abcdefg b\n", 0, NULL);
    util_case(m, "printf 'abcdef  b\\n' | unexpand -a", "abcdef\tb\n", 0, NULL);
    util_case(m, "printf '  \\t x\\n' | unexpand", "\t x\n", 0, NULL);

    /* env */
    util_case(m, "export FOO=bar; env | grep FOO", "FOO=bar\n", 0, NULL);
    util_case(m, "X=1 env | grep '^X='", "X=1\n", 0, NULL);
    util_case(m, "env A=1 B='x y' | grep '^[AB]='", "A=1\nB=x y\n", 0, NULL);
    util_case(m, "env Z=9 env | grep '^Z='", "Z=9\n", 0, NULL);
    util_case(m, "env | grep '^Z='", "", 1, NULL);
    util_case(m, "printf 'a\\nb\\n' | env head -n 1", "a\n", 0, NULL);
    util_case(m, "env -- K=v env | grep '^K='", "K=v\n", 0, NULL);
    util_case(m, "env -i", "", 125, "env: -i is not supported here");
    util_case(m, "env nosuch", "", 127, "nosuch");

    /* xargs, against macOS's */
    util_case(m, "printf 'a b\\nc\\n' | xargs", "a b c\n", 0, NULL);
    util_case(m, "printf 'a b c d e\\n' | xargs -n 2", "a b\nc d\ne\n", 0, NULL);
    util_case(m, "printf \"'x y' \\\"z w\\\" a\\\\\\\\ b\\n\" | xargs -n 1 echo", "x y\nz w\na b\n",
              0, NULL);
    util_case(m, "printf '  f1 x\\n\\nf2\\n' | xargs -I '{}' echo '[{}]'", "[f1 x]\n[f2]\n", 0,
              NULL);
    util_case(m, "printf 'a b STOP c\\n' | xargs -E STOP echo", "a b\n", 0, NULL);
    util_case(m, "printf 'a\\n' | xargs -t echo x", "x a\n", 0, "echo x a");
    util_case(m, "printf 'a b c' | xargs -s 9 echo", "a b\nc\n", 0, NULL);
    util_case(m, "printf '' | xargs echo hi", "", 0, NULL);
    util_case(m, "printf '1\\n2\\n' | xargs -n 1 test 2 =", "", 123, NULL);
    util_case(m, "printf \"'a\\n\" | xargs echo", "", 1, "xargs: unterminated quote");
    util_case(m, "printf 'a\\n' | xargs nosuch", "", 127, "nosuch");
    util_case(m, "printf 'aaaaaaaaaa\\n' | xargs -s 8 echo", "", 1, "too long");
    util_case(m, "printf 'a\\n' | xargs -n x", "", 1, "xargs: -n and -s take a number");
    util_case(m, "printf 'a.txt e.txt\\n' | xargs cksum",
              "3015617425 6 a.txt\n4294967295 0 e.txt\n", 0, NULL);
}

static uint8_t clip[256];
static size_t clip_len;
static const uint8_t *mock_clipboard_get(void *ctx, size_t *len) {
    (void)ctx;
    *len = clip_len;
    return clip_len == 0 ? NULL : clip;
}
static bool mock_clipboard_put(void *ctx, const uint8_t *data, size_t len) {
    (void)ctx;
    if (len > sizeof(clip)) {
        return false;
    }
    memcpy(clip, data, len);
    clip_len = len;
    return true;
}

/* pbcopy takes a | or its words, pbpaste prints and pipes; the Filo
   functions do the same; a host without a clipboard says so. */
static void test_clipboard(void) {
    static roc MC;
    static mock_host hc;
    memset(&hc, 0, sizeof(hc));
    roc_host host = {
        .filo_extend = roc_host_extend,
        .ctx = &hc,
        .host_name = "shell.test",
        .site_base = "https://shell.test",
        .request = mock_request,
        .clipboard_get = mock_clipboard_get,
        .clipboard_put = mock_clipboard_put,
    };
    roc_init(&MC, &host, 80, 24, ROC_F_NO_SPLASH);
    hc.pending = 0;
    roc_feed(&MC, hc.req_id, (const uint8_t *)test_index, strlen(test_index));
    roc_feed_eof(&MC, hc.req_id);
    drain(&MC);
    send(&MC, "s\r");
    drain(&MC);
    clip_len = 0;
    send(&MC, "pbpaste; echo end$?\r");
    const char *o = drain(&MC);
    CHECK(strstr(o, "\r\nend0\r\n") != NULL);
    send(&MC, "echo Hello World! | pbcopy; echo st$?\r");
    o = drain(&MC);
    CHECK(clip_len == 13 && memcmp(clip, "Hello World!\n", 13) == 0 &&
          strstr(o, "\r\nst0\r\n") != NULL);
    send(&MC, "pbpaste\r");
    o = drain(&MC);
    CHECK(strstr(o, "\r\nHello World!\r\n") != NULL);
    send(&MC, "pbpaste | wc\r");
    o = drain(&MC);
    CHECK(strstr(o, "\r\n1 2 13\r\n") != NULL);
    send(&MC, "pbcopy plain words; pbpaste\r");
    o = drain(&MC);
    CHECK(clip_len == 11 && strstr(o, "\r\nplain words\r\n") != NULL);
    send(&MC, "echo '(pbcopy \"from filo\")' > ~/c.filo; filo ~/c.filo; pbpaste\r");
    o = drain(&MC);
    CHECK(clip_len == 9 && strstr(o, "\r\nfrom filo\r\n") != NULL);
    send(&MC, "echo '(write (pbpaste))' > ~/p.filo; filo ~/p.filo\r");
    o = drain(&MC);
    CHECK(strstr(o, "from filo") != NULL);
    roc_host bare = {.filo_extend = roc_host_extend,
                     .ctx = &hc,
                     .host_name = "shell.test",
                     .site_base = "https://shell.test",
                     .request = mock_request};
    roc_init(&MC, &bare, 80, 24, ROC_F_NO_SPLASH);
    hc.pending = 0;
    roc_feed(&MC, hc.req_id, (const uint8_t *)test_index, strlen(test_index));
    roc_feed_eof(&MC, hc.req_id);
    drain(&MC);
    send(&MC, "s\r");
    drain(&MC);
    send(&MC, "echo x | pbcopy; echo st$?; pbpaste; echo st$?\r");
    o = drain(&MC);
    CHECK(strstr(o, "pbcopy: no clipboard on this host") != NULL &&
          strstr(o, "\r\nst1\r\n") != NULL);
    CHECK(strstr(o, "pbpaste: no clipboard on this host") != NULL);
}

static void test_commands_start_fresh(void) {
    mock_host h;
    roc *m = boot(&h);
    send(m, "ver\r");
    drain(m);
    size_t after_one = m->sc.ctx.persistent.used;
    for (int i = 0; i < 20; i++) {
        send(m, "ver\r");
        drain(m);
    }
    CHECK(m->sc.ctx.persistent.used == after_one);
    send(m, "filo\r");
    drain(m);
    send(m, "(def kept 7)\r");
    drain(m);
    send(m, "kept\r");
    CHECK(strstr(drain(m), "7") != NULL); /* inside the REPL a definition stays */
    send(m, "\x04");
    drain(m);
    send(m, "ver\r");
    drain(m);
    send(m, "filo\r");
    drain(m);
    send(m, "kept\r");
    CHECK(strstr(drain(m), "undefined") != NULL); /* a command in between started over */
    send(m, "\x04");
    drain(m);
}

/* A screen that fails says where, as a compiler does: its file, the line
   and the column of the node that failed. */
/* Every key is the program's, ESC too: stepping back is the framework's
   rule, written in Filo (back-on-esc), so a program that does not follow
   it keeps the key. */
static void test_every_key_is_the_programs(void) {
    static const uint8_t init[] = "(def got 0)";
    static const uint8_t draw[] = "(print-at 0 0 (string got))";
    static const uint8_t key[] = "(set got KEY)";
    static const tree_file tree[] = {
        {"own/init.filo", init, sizeof(init) - 1},
        {"own/draw.filo", draw, sizeof(draw) - 1},
        {"own/key.filo", key, sizeof(key) - 1},
    };
    mock_host h;
    roc *m = boot_menu(&h);
    tree_programs(m, tree, 3);
    screen_enter(m, "own");
    drain(m);
    send(m, "\x1b");
    roc_tick(m, 1000); /* a bare ESC is told apart from a sequence by time */
    drain(m);
    CHECK(strcmp(m->scr.name, "own") == 0);
    CHECK(scr_num(m, "got") == (double)FT_KEY_ESC);
    tree_set_source(NULL, 0);
}

static void test_screen_errors_say_where(void) {
    static const uint8_t init[] = "#t";
    static const uint8_t draw[] = "(print-at 0 0 \"ok\")\n(print-at 1\n  (+ 1 \"a\") \"x\")";
    static const tree_file bad[] = {
        {"bad/init.filo", init, sizeof(init) - 1},
        {"bad/draw.filo", draw, sizeof(draw) - 1},
    };
    mock_host h;
    roc *m = boot_menu(&h);
    tree_programs(m, bad, 2);
    CHECK(!screen_compose(m, "bad"));
    CHECK(strncmp(screen_error(m), "bad/draw.filo:3:3: ", 19) == 0);
    CHECK(strstr(screen_error(m), "expected number, got string") != NULL);
    tree_set_source(NULL, 0);
}

/* The decoder alone: function keys in the three spellings terminals use —
   SS3 for F1 to F4, CSI n~ with its gaps for the rest, CSI 1;mod with a
   modifier held. */
static uint32_t decoded[8];
static int ndecoded = 0;

static void collect(void *user, keyin_event ev, uint32_t cp) {
    (void)user;
    if (ev == KEYIN_KEY && ndecoded < 8) {
        decoded[ndecoded] = cp;
        ndecoded++;
    }
}

static void test_keyin_function_keys(void) {
    keyin k;
    keyin_init(&k);
    const char *in = "\x1bOP\x1b[15~\x1b[17~\x1b[24~\x1b[1;2Q\x1b[23;5~";
    for (const char *p = in; *p != '\0'; p++) {
        keyin_feed(&k, (uint8_t)*p, collect, NULL);
    }
    CHECK(ndecoded == 6);
    CHECK(decoded[0] == FT_KEY_F1);
    CHECK(decoded[1] == FT_KEY_F1 + 4);                   /* F5 */
    CHECK(decoded[2] == FT_KEY_F1 + 5);                   /* F6, after the gap */
    CHECK(decoded[3] == FT_KEY_F1 + 11);                  /* F12 */
    CHECK(decoded[4] == (FT_KEY_F1 + 1U | FT_KEY_SHIFT)); /* Shift-F2 */
    CHECK(decoded[5] == (FT_KEY_F1 + 10U | FT_KEY_CTRL)); /* Ctrl-F11 */
    CHECK(FT_KEY_BASE(decoded[4]) == FT_KEY_F1 + 1U);
}

/* A terminal's answers (colour queries, cursor reports) reach the prompt as
   input nobody asked for: swallowed, never typed into the line. One cut off
   mid-string ends at the ESC timeout and the next key is a key again. */
static void test_terminal_replies_never_reach_the_line(void) {
    mock_host h;
    roc *m = boot(&h);
    send(m, "\x1b]11;rgb:0a0a/0a0a/0a0a\x1b\\"); /* OSC, ST-terminated */
    send(m, "\x1b]10;rgb:ffff/ffff/ffff\x07");   /* OSC, BEL-terminated */
    send(m, "\x1b[24;80R");                      /* DSR cursor report */
    send(m, "pwd\r");
    const char *o = drain(m);
    CHECK(strstr(o, "/\r\n") != NULL);
    CHECK(strstr(o, "rgb") == NULL && strstr(o, "80R") == NULL);
    send(m, "\x1b]11;truncat");
    roc_tick(m, ROC_ESC_TIMEOUT_MS);
    drain(m);
    send(m, "pwd\r");
    o = drain(m);
    CHECK(strstr(o, "/\r\n") != NULL);
    CHECK(strstr(o, "truncat") == NULL);
}

/* Registering a builtin in a full table fails, and the shell ignores the
   result: a builtin would go missing without a word (Core War's did, once,
   when the hex view's arrived). The screen context keeps room to spare. */
static void test_builtin_table_has_room(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    CHECK(m->scr.ctx.nbuiltins + 8U <= FILO_BUILTINS_MAX);
    CHECK(m->sc.ctx.nbuiltins + 8U <= FILO_BUILTINS_MAX);
}

/* The tools' memory comes from the host's scratch, as much as the file
   needs: a source near the home's limit formats (fixed buffers of 8 MB
   refused one), and a host with no scratch says so instead. */
static void test_tools_scratch(void) {
    static char big[500 * 1024];
    size_t n = 0;
    while (n + 10 < sizeof(big)) {
        memcpy(big + n, "(def x 1)\n", 10);
        n += 10;
    }
    mock_host h;
    roc *m = boot(&h);
    CHECK(roc_write_file(m, "t", "/home/guest/big.filo", (const uint8_t *)big, n));
    send(m, "filo fmt -w ~/big.filo; echo $?\r");
    const char *o = drain(m);
    CHECK(strstr(o, "Too large") == NULL && strstr(o, "\r\n0\r\n") != NULL);
    CHECK(m->scratch_used > n * 17U); /* it took what the file needed */

    memset(&h, 0, sizeof(h));
    roc_host bare = {.filo_extend = roc_host_extend,
                     .ctx = &h,
                     .host_name = "shell.test",
                     .site_base = "https://shell.test",
                     .request = mock_request};
    roc_init(&M, &bare, 80, 24, ROC_F_NO_SPLASH);
    h.pending = 0;
    roc_feed(&M, h.req_id, (const uint8_t *)test_index, strlen(test_index));
    roc_feed_eof(&M, h.req_id);
    drain(&M);
    send(&M, "s\r");
    drain(&M);
    send(&M, "filo fmt ~/x.filo; diff ~/a ~/b\r");
    o = drain(&M);
    CHECK(strstr(o, "Not enough memory here for filo's tools") != NULL);
    CHECK(strstr(o, "Not enough memory here to compare") != NULL);
}

static void test_screen_paints(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    drain(m);
    screen_reset(m);
    CHECK(set_draw(m, draw_src));
    CHECK(screen_paint(m));

    const char *o = drain(m);
    CHECK(strstr(o, "hello") != NULL);
    CHECK(strstr(o, "80 x 24") != NULL); /* the size reached the script */

    const canvas *c = &m->cmp.target;
    CHECK(c->cells[0][0].cp == 'h');
    CHECK(c->cells[0][0].fg == 3); /* the pen held across the call */
    CHECK(c->cells[0][4].cp == 'o');
    CHECK(c->cells[0][4].fg == 3);
    CHECK(c->cells[1][0].cp == ' '); /* nothing bled onto the next row */
    CHECK(c->cells[2][0].cp == '+');
    CHECK(c->cells[2][1].cp == '-');
    CHECK(c->cells[2][9].cp == '+');
    CHECK(c->cells[3][0].cp == '|');
    CHECK(c->cells[3][1].cp == ' '); /* a box is its border, not a fill */
    CHECK(c->cells[4][0].cp == '+');
    CHECK(c->cells[0][0].bg == CV_COLOR_DEFAULT);
}

/* Immediate mode earns its keep here: the same program lays the screen out
   again at the new size, with no anchors and no resize machinery. */
static void test_screen_repaints_on_resize(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    drain(m);
    screen_reset(m);
    CHECK(set_draw(m, draw_src));
    CHECK(screen_paint(m));
    drain(m);

    roc_resize(m, 40, 12);
    CHECK(screen_paint(m));
    const char *o = drain(m);
    CHECK(strstr(o, "40 x 12") != NULL);
    CHECK(m->cmp.target.cols == 40);
    CHECK(m->cmp.target.cells[11][0].cp == '4'); /* the last row followed H */
}

static void test_screen_failure_keeps_the_last_frame(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    drain(m);
    screen_reset(m);
    CHECK(set_draw(m, "(print-at 0 0 \"good\")"));
    CHECK(screen_paint(m));
    drain(m);

    /* a run that dies halfway must not leave a half-painted screen */
    CHECK(set_draw(m, "(do (print-at 0 0 \"bad!\") (error \"boom\"))"));
    CHECK(!screen_paint(m));
    const char *o = drain(m);
    CHECK(o[0] == '\0'); /* nothing went out */
    CHECK(strstr(screen_error(m), "boom") != NULL);
    CHECK(m->cmp.shown.cells[0][0].cp == 'g'); /* the terminal still shows it */

    /* a name the script never defined is caught when the screen loads */
    CHECK(!set_draw(m, "(print-at 0 0"));
    CHECK(screen_error(m)[0] != '\0');
}

static void test_screen_arguments_are_checked(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    drain(m);
    screen_reset(m);

    /* half a column does not exist: a position takes the cell it falls in,
       so dividing by three still places text on any terminal */
    CHECK(set_draw(m, "(do (print-at 0.5 1.9 \"x\") (print-at (/ 7 3) 0 \"y\"))"));
    CHECK(screen_paint(m));
    CHECK(m->cmp.target.cells[0][1].cp == 'x');
    CHECK(m->cmp.target.cells[2][0].cp == 'y');

    /* an attribute or a colour is not a position, and stays strict */
    CHECK(set_draw(m, "(fg 1.5)"));
    CHECK(!screen_paint(m));
    CHECK(strstr(screen_error(m), "whole number") != NULL);

    CHECK(set_draw(m, "(print-at 0 0 42)"));
    CHECK(!screen_paint(m));
    CHECK(strstr(screen_error(m), "string") != NULL);

    CHECK(set_draw(m, "(box 0 0 3 3 (list \"+\" \"-\"))"));
    CHECK(!screen_paint(m));
    CHECK(strstr(screen_error(m), "8 border pieces") != NULL);

    CHECK(set_draw(m, "(fg 999)"));
    CHECK(!screen_paint(m));
    CHECK(strstr(screen_error(m), "colour") != NULL);

    /* off the canvas is clipped, not refused: layout arithmetic goes
       negative on a narrow terminal and that must not be an error */
    CHECK(set_draw(m, "(do (print-at -5 -5 \"far\") (print-at 0 (+ W 10) \"far\")"
                      " (fill (- H 2) 0 40 200 \"#\") (print-at 0 0 \"ok\"))"));
    CHECK(screen_paint(m));
    CHECK(m->cmp.target.cells[0][0].cp == 'o');
}

/* A screen that measures its own text can centre it, which is the one thing
   every screen does and the reason text-width exists at all. */
static void test_screen_centres_text(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    drain(m);
    screen_reset(m);
    const char *src = "(let ((s \"caf\xc3\xa9\"))"
                      "  (print-at 0 (/ (- W (text-width s)) 2) s))";
    CHECK(set_draw(m, src));
    CHECK(screen_paint(m));
    CHECK(m->cmp.target.cells[0][38].cp == 'c');
    CHECK(m->cmp.target.cells[0][41].cp == 0xE9);
}

/* ---- the screens tree ---- */

static void test_screen_field_limits_and_masking(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    drain(m);
    screen_reset(m);

    /* the maximum is in runes, and hiding shows one star per rune */
    const char *src = "(do (print-at 0 0 \"pw:\") (input-at 0 4 10 4 #t))";
    CHECK(screen_set_draw(m, (const uint8_t *)src, strlen(src)));
    CHECK(screen_paint(m));
    for (int i = 0; i < 8; i++) {
        CHECK(screen_key(m, 'x'));
    }
    CHECK(m->cmp.target.cells[0][4].cp == '*');
    CHECK(m->cmp.target.cells[0][7].cp == '*');
    CHECK(m->cmp.target.cells[0][8].cp == ' '); /* four runes, no more */
    CHECK(m->scr.in.runes == 4);
}

/* A screen of the test's own, shaped as a shell's screens are: a common file
   ahead of it, init, hooks, art beside it, and a C app it opens by name. */
static const char fix_common[] =
    "(def centred (fn (row text) (print-at row (floor (/ (- W (text-width text)) 2)) text)))\n"
    "(def single (list \"\xe2\x94\x8c\" \"\xe2\x94\x80\" \"\xe2\x94\x90\" \"\xe2\x94\x82\""
    " \"\xe2\x94\x98\" \"\xe2\x94\x80\" \"\xe2\x94\x94\" \"\xe2\x94\x82\"))\n";
static const char fix_init[] = "(def title \"the fixture\")\n(def who \"\")\n(def hits 0)\n";
static const char fix_draw[] = "(box 0 0 H W single)\n"
                               "(blit 2 2 \"art.ans\")\n"
                               "(fg 6)\n"
                               "(centred (/ H 3) title)\n"
                               "(fg C_DEFAULT)\n"
                               "(if (is-empty who)\n"
                               "  (centred (+ (/ H 3) 2) (str-fmt \"%d keys pressed\" hits))\n"
                               "  (centred (+ (/ H 3) 2) (str-concat \"hello, \" who)))\n"
                               "(print-at (- H 2) 2 NOTE)\n"
                               "(let ((label \"name: \") (left (floor (/ (- W 26) 2))))\n"
                               "  (print-at (- H 3) left label)\n"
                               "  (input-at (- H 3) (+ left (text-width label)) 20 16 #f))\n";
static const char fix_key[] =
    "(cond\n"
    "  ((= KEY KEY_DOWN) (exec \"fix\"))\n"
    "  ((= KEY KEY_RIGHT)\n"
    "    (exec \"fight\" \"/lib/roc/warriors/imp.red /lib/roc/warriors/dwarf.red\"))\n"
    "  (else (set hits (+ hits 1))))\n";
static const char fix_input[] = "(set who (input-text))\n";
/* relative moves and a background colour, as a drawing program writes them */
static const char fix_art[] = "\x1b[102m          \x1b[B\x1b[10D          \x1b[2C  \x1b[m";

static void fixture_screen(roc *m) {
    const uint8_t *imp = NULL;
    const uint8_t *dwarf = NULL;
    size_t imp_len = 0;
    size_t dwarf_len = 0;
    CHECK(tree_find_file("warriors/imp.red", &imp, &imp_len));
    CHECK(tree_find_file("warriors/dwarf.red", &dwarf, &dwarf_len));
    const tree_file tree[] = {
        {"common.filo", (const uint8_t *)fix_common, sizeof(fix_common) - 1},
        {"fix/init.filo", (const uint8_t *)fix_init, sizeof(fix_init) - 1},
        {"fix/draw.filo", (const uint8_t *)fix_draw, sizeof(fix_draw) - 1},
        {"fix/key.filo", (const uint8_t *)fix_key, sizeof(fix_key) - 1},
        {"fix/input.filo", (const uint8_t *)fix_input, sizeof(fix_input) - 1},
        {"fix/art.ans", (const uint8_t *)fix_art, sizeof(fix_art) - 1},
        {"warriors/imp.red", imp, imp_len},
        {"warriors/dwarf.red", dwarf, dwarf_len},
    };
    tree_programs(m, tree, sizeof(tree) / sizeof(tree[0]));
}

static void test_screen_loads_from_the_tree(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    drain(m);
    fixture_screen(m);

    CHECK(screen_load(m, "fix"));
    CHECK(screen_error(m)[0] == '\0');
    const canvas *c = &m->cmp.target;

    /* the common file defined the helper and the border the screen used, so
       both files ran before init */
    CHECK(c->cells[0][0].cp == 0x250C);
    CHECK(c->cells[0][1].cp == 0x2500);
    CHECK(c->cells[23][0].cp == 0x2514);
    CHECK(strstr(canvas_row(c, 8), "the fixture") != NULL);
    CHECK(strstr(canvas_row(c, 10), "0 keys pressed") != NULL);
    CHECK(strstr(canvas_row(c, 21), "name:") != NULL);

    /* centred means centred, counted in cells: the border is three bytes a
       column, so only the grid can answer this */
    CHECK(c->cells[8][34].cp == 't');
    CHECK(c->cells[8][44].cp == 'e');
    CHECK(c->cells[8][33].cp == ' ');
    tree_set_source(NULL, 0);
}

/* The globals are closed once init has run, so a name typed wrong in a hook
   cannot reach the user: it fails while the screen is loading. */
static void test_screen_seals_its_globals(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    drain(m);
    fixture_screen(m);
    CHECK(screen_load(m, "fix"));

    filo_prog p;
    const char *bad = "(set htis 1)";
    CHECK(filo_compile(&m->scr.ctx, (const uint8_t *)bad, strlen(bad), &p) != FILO_OK);
    CHECK(strstr(filo_error(&m->scr.ctx), "htis") != NULL);

    const char *good = "(set hits 1)";
    CHECK(filo_compile(&m->scr.ctx, (const uint8_t *)good, strlen(good), &p) == FILO_OK);
    tree_set_source(NULL, 0);
}

static void test_screen_key_hook_and_navigation(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    drain(m);
    fixture_screen(m);
    CHECK(screen_load(m, "fix"));
    drain(m);

    CHECK(screen_key(m, FT_KEY_UP));
    CHECK(strstr(canvas_row(&m->cmp.target, 10), "1 keys pressed") != NULL);
    /* one digit changed, so one digit goes out: the rest of the line is
       already on the terminal */
    const char *o = drain(m);
    CHECK(strstr(o, "1") != NULL);
    CHECK(strstr(o, "keys") == NULL);
    CHECK(strlen(o) < 48);

    CHECK(screen_key(m, FT_KEY_UP));
    CHECK(strstr(canvas_row(&m->cmp.target, 10), "2 keys pressed") != NULL);

    /* navigating starts the screen over, so its state goes with the context */
    CHECK(screen_key(m, FT_KEY_DOWN));
    CHECK(strcmp(m->scr.name, "fix") == 0);
    CHECK(strstr(canvas_row(&m->cmp.target, 10), "0 keys pressed") != NULL);
    tree_set_source(NULL, 0);
}

static void test_screen_load_failures_are_named(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    drain(m);
    CHECK(!screen_load(m, "nosuch"));
    CHECK(strstr(screen_error(m), "nosuch/init.filo") != NULL);
}

/* A name that is not a screen fails on its missing init, before the common
   file is compiled: on a board the arena for that compile may not exist, and
   "out of memory" would hide what actually went wrong. */
static void test_missing_screen_names_its_init(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    fixture_screen(m);
    CHECK(!screen_compose(m, "no-such-screen"));
    CHECK(strstr(screen_error(m), "no-such-screen/init.filo") != NULL);
    filo_value v = {0};
    CHECK(!filo_get_global(&m->scr.ctx, "centred", &v)); /* common.filo's */
    tree_set_source(NULL, 0);
}

/* Art becomes cells: a drawing program writes relative cursor moves and the
   sixteen colours, and all of it has to survive the trip into the grid. */
static void test_screen_blits_art(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    drain(m);
    fixture_screen(m);
    CHECK(screen_load(m, "fix"));

    int colored = 0;
    uint16_t y = 0;
    while (y < m->cmp.target.rows) {
        uint16_t x = 0;
        while (x < m->cmp.target.cols) {
            if (m->cmp.target.cells[y][x].bg == 10) { /* bright green, SGR 102 */
                colored++;
            }
            x++;
        }
        y++;
    }
    CHECK(colored == 22);
    CHECK(m->cmp.target.cells[3][2].bg == 10 && m->cmp.target.cells[3][13].bg != 10);
    CHECK(m->cmp.target.cells[3][14].bg == 10); /* two columns skipped, not painted */

    /* it lands as a fragment: the box the screen drew around it survives */
    CHECK(m->cmp.target.cells[0][0].cp == 0x250C);
    CHECK(m->cmp.target.cells[23][0].cp == 0x2514);

    /* a file the tree does not have names itself */
    const char *src = "(blit 0 0 \"nope.ans\")";
    CHECK(screen_set_draw(m, (const uint8_t *)src, strlen(src)));
    CHECK(!screen_paint(m));
    CHECK(strstr(screen_error(m), "nope.ans") != NULL);
    tree_set_source(NULL, 0);
}

/* The field: the shell owns the text, a screen reads it when enter closes
   the line, and the terminal's own cursor sits where the caret is. */
static void test_screen_field_typing(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    drain(m);
    fixture_screen(m);
    CHECK(screen_load(m, "fix"));
    drain(m);

    const char *word = "ana";
    for (const char *p = word; *p != '\0'; p++) {
        CHECK(screen_key(m, (uint32_t)*p));
    }
    CHECK(strstr(canvas_row(&m->cmp.target, 21), "name: ana") != NULL);

    /* backspace takes a whole rune, and Ctrl-U takes the line */
    CHECK(screen_key(m, 0x7F));
    CHECK(strstr(canvas_row(&m->cmp.target, 21), "name: an ") != NULL);
    CHECK(screen_key(m, 'a'));
    CHECK(screen_key(m, 0x15));
    CHECK(strstr(canvas_row(&m->cmp.target, 21), "name:  ") != NULL);

    /* a multibyte rune goes in and comes out whole */
    CHECK(screen_key(m, 'c'));
    CHECK(screen_key(m, 'a'));
    CHECK(screen_key(m, 'f'));
    CHECK(screen_key(m, 0xE9));
    CHECK(strstr(canvas_row(&m->cmp.target, 21), "caf\xc3\xa9") != NULL);

    /* enter hands it to the screen, which keeps it, and clears the field */
    CHECK(screen_key(m, '\r'));
    CHECK(strstr(canvas_row(&m->cmp.target, 10), "hello, caf\xc3\xa9") != NULL);
    CHECK(strstr(canvas_row(&m->cmp.target, 21), "name:  ") != NULL);

    /* the cursor is shown and parked at the caret */
    const char *o = drain(m);
    CHECK(strstr(o, "\x1b[?25h") != NULL || m->scr.cursor_shown);
    CHECK(m->scr.caret_row == 21);
    tree_set_source(NULL, 0);
}

/* the field of a screen edits like a line: caret, insert, delete, ends */
static void test_field_editing(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    fixture_screen(m);
    screen_enter(m, "fix");
    drain(m);
    send(m, "abc");
    send(m, "\x1b[D\x1b[D"); /* two left */
    send(m, "X");
    drain(m);
    CHECK(m->scr.in.len == 4 && memcmp(m->scr.in.buf, "aXbc", 4) == 0);
    CHECK(m->scr.in.cur == 2);
    CHECK(canvas_has(&m->cmp.target, "aXbc"));
    send(m, "\x1b[H");  /* Home */
    send(m, "\x1b[3~"); /* Delete */
    drain(m);
    CHECK(m->scr.in.len == 3 && memcmp(m->scr.in.buf, "Xbc", 3) == 0 && m->scr.in.cur == 0);
    send(m, "\x1b[F"); /* End */
    send(m, "\x7f");   /* Backspace */
    drain(m);
    CHECK(m->scr.in.len == 2 && memcmp(m->scr.in.buf, "Xb", 2) == 0);
    send(m, "\x1b[D\xc3\xa9"); /* é before b */
    drain(m);
    CHECK(m->scr.in.len == 4 && memcmp(m->scr.in.buf,
                                       "X\xc3\xa9"
                                       "b",
                                       4) == 0);
    CHECK(m->scr.caret_col > 0);
    send(m, "\x15"); /* Ctrl-U */
    drain(m);
    CHECK(m->scr.in.len == 0 && m->scr.in.cur == 0);
    tree_set_source(NULL, 0);
}

/* A screen loaded by a key with the caret left inside the text of the one
   before: the field starts over, caret too. */
static void test_screen_field_reset(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    fixture_screen(m);
    screen_enter(m, "fix");
    drain(m);
    send(m, "abc\x1b[D"); /* the caret before the c */
    drain(m);
    send(m, "\x1b[B"); /* down: the screen again */
    drain(m);
    send(m, "x"); /* ASan: the insert moved a negative length */
    drain(m);
    CHECK(m->scr.in.len == 1 && m->scr.in.cur == 1);
    tree_set_source(NULL, 0);
}

/* A key that takes the shell to a screen that fails to start: not a
   half-built screen that answers nothing, but the front screen again with
   the error as its note, or the prompt when there is no front screen. */
static void test_screen_failed_key_recovers(void) {
    static const char main_init[] = "(def x 0)";
    static const char main_draw[] = "(do (print-at 0 0 NOTE) (input-at 2 0 10 5 #f))";
    static const char main_input[] = "(exec \"boom\")";
    static const char boom_init[] = "(error \"boom at init\")";
    static const char boom_draw[] = "(print-at 0 0 \"never\")";
    static const tree_file tree[] = {
        {"main/init.filo", (const uint8_t *)main_init, sizeof(main_init) - 1},
        {"main/draw.filo", (const uint8_t *)main_draw, sizeof(main_draw) - 1},
        {"main/input.filo", (const uint8_t *)main_input, sizeof(main_input) - 1},
        {"boom/init.filo", (const uint8_t *)boom_init, sizeof(boom_init) - 1},
        {"boom/draw.filo", (const uint8_t *)boom_draw, sizeof(boom_draw) - 1},
    };
    mock_host h;
    roc *m = boot_menu(&h);
    drain(m);
    tree_programs(m, tree, 5);
    screen_enter(m, "main");
    drain(m);
    send(m, "go\r");
    const char *o = drain(m);
    if (roc_layer_spec.home != NULL) {
        CHECK(strcmp(m->scr.name, roc_layer_spec.home) == 0);
        CHECK(strstr(canvas_row(&m->cmp.target, 0), "boom at init") != NULL);
    } else {
        CHECK(m->t.napps == 0 && m->mode == ROC_MODE_LINE);
        CHECK(strstr(o, "boom at init") != NULL);
    }
    tree_set_source(NULL, 0);
}

/* Not every screen is written in Filo: Core War's arena is a C app, and a
   script opens it by name like any other screen. */
static void test_screen_opens_an_app_and_comes_back(void) {
    mock_host h;
    roc *m = boot_menu(&h);
    drain(m);
    fixture_screen(m);
    screen_enter(m, "fix");
    CHECK(screen_error(m)[0] == '\0');
    size_t depth = m->t.napps;
    drain(m);

    CHECK(screen_key(m, FT_KEY_RIGHT));
    CHECK(m->t.napps == depth + 1); /* the arena is on top */
    const char *o = drain(m);
    CHECK(strstr(o, "the fixture") == NULL); /* the screen did not paint under it */

    /* coming back repaints in full, since the app painted over everything */
    roc_app_leave(m, NULL);
    CHECK(m->t.napps == depth);
    o = drain(m);
    CHECK(strstr(o, "the fixture") != NULL);
    CHECK(strstr(o, "name:") != NULL);
    tree_set_source(NULL, 0);
}

/* A program that a screen opened goes back to that screen when it is
   done, not to the shell under it: a game chosen from a list returns to
   the list. (edt names its way back in RET; this is what a program with
   no RET gets.) */
static void test_done_goes_back(void) {
    static const char list_init[] = "(def x 0)";
    static const char list_draw[] = "(print-at 0 0 \"the list\")";
    static const char game_init[] = "(def y 0)";
    static const char game_draw[] = "(print-at 0 0 \"the game\")";
    static const tree_file tree[] = {
        {"list/init.filo", (const uint8_t *)list_init, sizeof(list_init) - 1},
        {"list/draw.filo", (const uint8_t *)list_draw, sizeof(list_draw) - 1},
        {"game/init.filo", (const uint8_t *)game_init, sizeof(game_init) - 1},
        {"game/draw.filo", (const uint8_t *)game_draw, sizeof(game_draw) - 1},
    };
    mock_host h;
    roc *m = boot_menu(&h);
    tree_programs(m, tree, 4);
    screen_enter(m, "list");
    size_t depth = m->t.napps;
    CHECK(screen_load(m, "game")); /* led here from the list */
    drain(m);
    CHECK(set_draw(m, "(do (done) (print-at 0 0 \"x\"))"));
    send(m, "a"); /* the paint after it runs the draw, which asks to leave */
    send(m, "b"); /* the next event settles it */
    drain(m);
    CHECK(strcmp(m->scr.name, "list") == 0);
    CHECK(m->t.napps == depth);
    tree_set_source(NULL, 0);
}

static char picked[VFS_PATH_MAX];

static void mock_pick_file(void *ctx, const char *dest) {
    (void)ctx;
    snprintf(picked, sizeof(picked), "%s", dest);
}

static void test_upload(void) {
    mock_host h;
    roc *m = boot(&h);
    drain(m);

    /* dropped at the site's root, a file goes to ~/uploads, made for it */
    send(m, "cat ");
    drain(m);
    drop(m, "notes.txt", "", "hello\n");
    const char *o = drain(m);
    CHECK(strstr(o, "uploaded /home/guest/uploads/notes.txt (6)") != NULL);
    CHECK(strstr(o, "$ cat ") != NULL); /* the line came back */
    send(m, "~/uploads/notes.txt\r");
    o = drain(m);
    CHECK(strstr(o, "hello\r\n") != NULL);
    send(m, "ls -l ~/uploads\r");
    o = drain(m);
    CHECK(strstr(o, "notes.txt") != NULL);
    CHECK(strstr(o, " 6 ") != NULL);

    /* in the home, a drop lands where the person is; a space in the name
       becomes an underscore so the shell can say it */
    send(m, "cd ~\r");
    drain(m);
    CHECK(strcmp(m->cwd, "/home/guest") == 0);
    drop(m, "my file.txt", "", "x");
    o = drain(m);
    CHECK(strstr(o, "uploaded /home/guest/my_file.txt (1)") != NULL);
    send(m, "ls -F\r");
    o = drain(m);
    CHECK(strstr(o, "my_file.txt") != NULL);
    CHECK(strstr(o, "uploads/") != NULL);

    /* a script of the person's own runs like any other */
    drop(m, "hi.filo", "", "(echo \"hi from\" (pwd))");
    drain(m);
    send(m, "filo hi.filo\r");
    o = drain(m);
    CHECK(strstr(o, "hi from /home/guest\r\n") != NULL);
    send(m, "filo ~/hi.filo\r");
    o = drain(m);
    CHECK(strstr(o, "hi from /home/guest\r\n") != NULL);

    /* the same name again is the new file, nothing kept of the old */
    size_t before = m->uf.used;
    drop(m, "my file.txt", "", "yz");
    drain(m);
    CHECK(m->uf.used == before + 1);
    send(m, "cat my_file.txt\r");
    o = drain(m);
    CHECK(strstr(o, "yz\r\n") != NULL);
    CHECK(vfs_lookup(&m->fs, "/home/guest/my_file.txt")->size == 2);

    /* refusals, each said once, in the words Unix has always used */
    {
        static uint8_t big[UFS_FILE_MAX];
        memset(big, 'a', sizeof(big));
        const char *names[] = {"b1", "b2", "b3"};
        size_t i = 0;
        while (i < 3) {
            CHECK(roc_upload_begin(m, names[i], "", sizeof(big)));
            roc_upload_data(m, big, sizeof(big));
            roc_upload_end(m);
            i++;
        }
        drain(m);
        /* the home already holds a few bytes: a fourth 512K does not fit */
        CHECK(!roc_upload_begin(m, "b4", "", sizeof(big)));
        o = drain(m);
        CHECK(strstr(o, "rocchetto: upload: b4: Disc quota exceeded") != NULL);
        send(m, "rm b1\r");
        send(m, "rm b2\r");
        send(m, "rm b3\r");
        drain(m);
        CHECK(m->uf.nfiles == 3); /* my_file.txt, hi.filo, uploads/notes.txt */
    }
    CHECK(!roc_upload_begin(m, "big.bin", "", UFS_FILE_MAX + 1));
    o = drain(m);
    CHECK(strstr(o, "rocchetto: upload: big.bin: File too large") != NULL);
    CHECK(!roc_upload_begin(m, "x", "/pub", 1));
    o = drain(m);
    CHECK(strstr(o, "Read-only file system") != NULL);
    CHECK(!roc_upload_begin(m, "..", "", 1));
    o = drain(m);
    CHECK(strstr(o, "rocchetto: upload: ..: Invalid argument") != NULL);
    CHECK(!roc_upload_begin(m, "uploads", "", 1));
    o = drain(m);
    CHECK(strstr(o, "rocchetto: upload: uploads: Is a directory") != NULL);
    drop(m, "in.txt", "~/uploads", "q");
    o = drain(m);
    CHECK(strstr(o, "uploaded /home/guest/uploads/in.txt (1)") != NULL);

    /* rm takes the person's files and nothing else */
    send(m, "rm ~/uploads/notes.txt\r");
    drain(m);
    send(m, "ls uploads\r");
    o = drain(m);
    CHECK(strstr(o, "notes.txt") == NULL);
    CHECK(strstr(o, "in.txt") != NULL);
    send(m, "rm /readme.txt\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: rm: /readme.txt: Read-only file system") != NULL);
    send(m, "rm nothere\r");
    o = drain(m);
    CHECK(strstr(o, "rm: nothere: No such file or directory") != NULL);
    send(m, "rm\r");
    o = drain(m);
    CHECK(strstr(o, "usage: rm [-fRr] file...") != NULL);

    /* upload asks the host for files, with the directory settled first */
    send(m, "upload\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: upload: No such device") != NULL);
    m->host.pick_file = mock_pick_file;
    picked[0] = '\0';
    send(m, "upload\r");
    drain(m);
    CHECK(strcmp(picked, "/home/guest") == 0);
    send(m, "cd /\r");
    drain(m);
    send(m, "upload\r");
    drain(m);
    CHECK(strcmp(picked, "/home/guest/uploads") == 0);
    send(m, "upload ~/uploads\r");
    drain(m);
    CHECK(strcmp(picked, "/home/guest/uploads") == 0);
    picked[0] = '\0';
    send(m, "upload /pub\r");
    o = drain(m);
    CHECK(strstr(o, "Read-only file system") != NULL);
    CHECK(picked[0] == '\0');
    send(m, "upload ~/nothere\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto: upload: /home/guest/nothere: No such file or directory") != NULL);

    /* with a screen up, the word goes to the screen's note line */
    fixture_screen(m);
    screen_enter(m, "fix");
    drain(m);
    size_t depth = m->t.napps;
    drop(m, "late.txt", "", "z");
    drain(m);
    CHECK(canvas_has(&m->cmp.target, "uploaded /home/guest/uploads/late.txt (1)"));
    CHECK(m->t.napps == depth);
    tree_set_source(NULL, 0);
}

/* The tree that ships in the binary is the factory copy, and what the user
   writes shadows it. That is the whole recovery story for a system that
   edits itself: a bad edit is undone by removing the copy, and nothing the
   user does can reach what is in flash. */
static void test_user_files_shadow_the_tree(void) {
    mock_host h;
    roc *m = boot(&h);

    send(m, "ver\r");
    const char *o = drain(m);
    CHECK(strstr(o, "rocchetto " ROC_VERSION) != NULL);

    /* a program of the user's own in /bin: from here the shell runs mine */
    static uint8_t mine[4096];
    size_t mine_len = 0;
    CHECK(script_build(m, (const uint8_t *)"(echo \"mine\")\n", 14, mine, sizeof(mine), &mine_len));
    CHECK(roc_write_check(m, "/bin/ver", mine, mine_len) == NULL);
    send(m, "ver\r");
    o = drain(m);
    CHECK(strstr(o, "mine") != NULL);

    /* and removing it brings the shell's program back, not a hole */
    send(m, "rm /bin/ver\r");
    drain(m);
    send(m, "ver\r");
    o = drain(m);
    CHECK(strstr(o, "rocchetto " ROC_VERSION) != NULL);

    /* a board file written over is a copy; removing it uncovers the tree's */
    CHECK(roc_write_check(m, "/lib/roc/warriors/imp.red", (const uint8_t *)"x", 1) == NULL);
    send(m, "cat /lib/roc/warriors/imp.red\r");
    o = drain(m);
    CHECK(strstr(o, ";name Imp") == NULL);
    send(m, "rm /lib/roc/warriors/imp.red\r");
    drain(m);
    send(m, "cat /lib/roc/warriors/imp.red\r");
    o = drain(m);
    CHECK(strstr(o, ";name Imp") != NULL);

    /* a path that is neither the home nor the tree stays read-only */
    CHECK(roc_write_check(m, "/pub/kutta.md", (const uint8_t *)"x", 1) != NULL);
}

/* ed: the rescue editor. Line-oriented on purpose — no canvas, no screen
   system — so it is what a build without a board has, and what is left when
   an edit breaks the one that has. */
static void test_ed_edits_by_line(void) {
    mock_host h;
    roc *m = boot(&h);

    send(m, "ed ~/t.filo\r");
    const char *o = drain(m);
    CHECK(strstr(o, "0\r\n") != NULL); /* a name not there yet: zero bytes */
    CHECK(ed_active(m));

    send(m, "a\r");
    drain(m);
    send(m, "(echo \"um\")\r");
    send(m, "  (echo \"dois\")\r"); /* the indentation is text, not padding */
    send(m, ".\r");
    drain(m);
    CHECK(!m->ed_l.inserting);

    send(m, ",n\r");
    o = drain(m);
    CHECK(strstr(o, "1\t(echo \"um\")") != NULL);
    CHECK(strstr(o, "2\t  (echo \"dois\")") != NULL);

    /* an address alone moves there and shows the line */
    send(m, "1\r");
    o = drain(m);
    CHECK(strstr(o, "(echo \"um\")") != NULL);

    send(m, "2s/dois/DOIS/\r");
    drain(m);
    send(m, "2p\r");
    o = drain(m);
    CHECK(strstr(o, "  (echo \"DOIS\")") != NULL);

    /* q refuses while there is unwritten work; w says the byte count */
    send(m, "q\r");
    o = drain(m);
    CHECK(strstr(o, "? not written") != NULL);
    CHECK(ed_active(m));
    send(m, "w\r");
    o = drain(m);
    CHECK(strstr(o, "28\r\n") != NULL);
    send(m, "q\r");
    drain(m);
    CHECK(!ed_active(m));

    /* what it wrote is a file, and a script that runs */
    send(m, "filo ~/t.filo\r");
    o = drain(m);
    CHECK(strstr(o, "um") != NULL && strstr(o, "DOIS") != NULL);

    /* deleting a range, and the refusals */
    send(m, "ed ~/t.filo\r");
    drain(m);
    send(m, "1d\r");
    drain(m);
    send(m, ",p\r");
    o = drain(m);
    CHECK(strstr(o, "DOIS") != NULL && strstr(o, "\"um\"") == NULL);
    send(m, "9p\r");
    o = drain(m);
    CHECK(strstr(o, "? line out of range") != NULL);
    send(m, "z\r");
    o = drain(m);
    CHECK(strstr(o, "? unknown command") != NULL);
    send(m, "s/nada/x/\r");
    o = drain(m);
    CHECK(strstr(o, "? no match") != NULL);
    send(m, "Q\r"); /* leaves with the change unwritten */
    drain(m);
    CHECK(!ed_active(m));
    send(m, "cat ~/t.filo\r");
    o = drain(m);
    CHECK(strstr(o, "(echo \"um\")") != NULL); /* Q really did not write */
}

/* A login changes who is at the keyboard: the prompt says so from the next
   line on, and a name no prompt could hold is a guest. */
static void test_set_user(void) {
    mock_host h;
    roc *m = boot(&h);
    roc_set_user(m, "ana");
    send(m, "\r");
    CHECK(strstr(drain(m), "ana@shell.test") != NULL);
    send(m, "echo $USER\r");
    CHECK(strstr(drain(m), "\r\nana\r\n") != NULL);
    roc_set_user(m, "not a name");
    send(m, "\r");
    CHECK(strstr(drain(m), "guest@shell.test") != NULL);
}

/* A line the host has for the person, unasked: above what is being typed,
   which comes back whole; over an app, on the app's own line. */
static void test_notice(void) {
    mock_host h;
    roc *m = boot(&h);
    send(m, "ech");
    drain(m);
    roc_notice(m, "ana says hi");
    const char *o = drain(m);
    CHECK(strstr(o, "\r\nana says hi\r\n") != NULL);
    CHECK(strstr(o, "ech") != NULL && strstr(o, "ech") > strstr(o, "ana says hi"));
    send(m, "o ok\r");
    CHECK(strstr(drain(m), "\r\nok\r\n") != NULL); /* the line went on where it was */
    send(m, "help\r");                             /* the pager: an app on top */
    drain(m);
    roc_notice(m, "a note");
    CHECK(strcmp(m->t.note, "a note") == 0);
}

/* A test, named when it failed: a CHECK says the line, which may be a
   helper's. */
#define RUN(t)                                                                                     \
    do {                                                                                           \
        int before = failures;                                                                     \
        t();                                                                                       \
        if (failures > before) {                                                                   \
            printf("  in %s\n", #t);                                                               \
        }                                                                                          \
    } while (0)

/* Every test of rocchetto. A layer's build runs these and its own: it
   defines ROC_TEST_NO_MAIN, includes this file, and calls roc_tests. */
static void roc_tests(void) {
    RUN(test_prompt_flag);
    RUN(test_set_user);
    RUN(test_notice);
    RUN(test_host_runs_commands);
    RUN(test_utf8_decode);
    RUN(test_utf8_width);
    RUN(test_vfs_resolve);
    RUN(test_vfs_parse);
    RUN(test_boot_and_pwd);
    RUN(test_shell_starts_at_home);
    RUN(test_cd_ls);
    RUN(test_cat_strips_front_matter);
    RUN(test_cat_plain_body_and_bom);
    RUN(test_ctrl_c_aborts_cat);
    RUN(test_line_editing_multibyte);
    RUN(test_esc_and_arrows);
    RUN(test_line_editing);
    RUN(test_ls_posix);
    RUN(test_tab_completion);
    RUN(test_history);
    RUN(test_ufs);
    RUN(test_key_modifiers);
    RUN(test_home_unpack);
    RUN(test_profile);
    RUN(test_home_other_tab);
    RUN(test_home_carry);
    RUN(test_tbuf);
    RUN(test_editor);
    RUN(test_editor_without_a_file);
    RUN(test_commands_run_from_units);
    RUN(test_repl_and_site_files);
    RUN(test_filo_tools);
    RUN(test_filo_debug);
    RUN(test_shell_words);
    RUN(test_shell_expansions);
    RUN(test_shell_lists);
    RUN(test_shell_pipes);
    RUN(test_shell_redirections);
    RUN(test_shell_substitution);
    RUN(test_repl_tab);
    RUN(test_history_search);
    RUN(test_pager_search);
    RUN(test_diff);
    RUN(test_sort_uniq);
    RUN(test_shell_glob);
    RUN(test_shell_compound);
    RUN(test_shell_arith);
    RUN(test_shell_case);
    RUN(test_shell_functions);
    RUN(test_shell_read);
    RUN(test_shell_printf);
    RUN(test_shell_scripts);
    RUN(test_shell_braces);
    RUN(test_shell_block_errors);
    RUN(test_shell_backquotes);
    RUN(test_shell_eval);
    RUN(test_shell_heredocs);
    RUN(test_shell_options);
    RUN(test_shell_local_getopts);
    RUN(test_shell_textutils);
    RUN(test_shell_trap_type_date);
    RUN(test_shell_subshells);
    RUN(test_shell_ifs_alias);
    RUN(test_tools_scratch);
    RUN(test_regex);
    RUN(test_grep_sed);
    RUN(test_seq_touch_cmp);
    RUN(test_redirect);
    RUN(test_paste_into_editor);
    RUN(test_filters);
    RUN(test_mars_command);
    RUN(test_corewar_door);
    RUN(test_corewar_pick);
    RUN(test_corewar_many);
    RUN(test_pager_basics);
    RUN(test_pager_scrolling);
    RUN(test_pager_wrap_and_resize);
    RUN(test_pager_wide_runes_wrap);
    RUN(test_md_inline);
    RUN(test_md_links);
    RUN(test_md_shortcodes);
    RUN(test_md_code_fence);
    RUN(test_md_only_for_markdown);
    RUN(test_md_strips_source_escapes);
    RUN(test_md_pager_wraps_styles);
    RUN(test_md_pager_hangs_lists);
    RUN(test_builtin_table_has_room);
    RUN(test_keyin_function_keys);
    RUN(test_terminal_replies_never_reach_the_line);
    RUN(test_screen_errors_say_where);
    RUN(test_every_key_is_the_programs);
    RUN(test_commands_start_fresh);
    RUN(test_clipboard);
    RUN(test_script_status_and_data);
    RUN(test_script_data);
    RUN(test_script_files);
    RUN(test_script_calls);
    RUN(test_script_file_handles);
    RUN(test_filo_utilities);
    RUN(test_filo_find);
    RUN(test_filo_touch);
    RUN(test_filo_rm_mv_cp);
    RUN(test_filo_classics);
    RUN(test_filo_sed);
    RUN(test_pipe_spool);
    RUN(test_fixtures);
    RUN(test_api_examples);
    RUN(test_script_interrupt);
    canvas_test_setup();
    RUN(test_canvas_draws);
    RUN(test_canvas_clips);
    RUN(test_canvas_flush_is_incremental);
    RUN(test_canvas_keeps_colour_255);
    RUN(test_canvas_flush_replays_to_the_same_screen);
    RUN(test_screen_paints);
    RUN(test_screen_repaints_on_resize);
    RUN(test_screen_failure_keeps_the_last_frame);
    RUN(test_screen_arguments_are_checked);
    RUN(test_screen_centres_text);
    RUN(test_screen_field_limits_and_masking);
    RUN(test_user_files_shadow_the_tree);
    RUN(test_ed_edits_by_line);
    RUN(test_small_commands);
    RUN(test_scripts);
    RUN(test_upload);
    RUN(test_fs_commands);
    RUN(test_home_kept);
    RUN(test_narrow_ls);
    RUN(test_user_name);
    RUN(test_index_failure);
    RUN(test_builtins_shadow_nothing);
    RUN(test_cardputer_screens);
    RUN(test_screens_run_from_units);
    RUN(test_missing_screen_names_its_init);
    RUN(test_screen_loads_from_the_tree);
    RUN(test_screen_seals_its_globals);
    RUN(test_screen_key_hook_and_navigation);
    RUN(test_screen_load_failures_are_named);
    RUN(test_screen_blits_art);
    RUN(test_screen_field_typing);
    RUN(test_field_editing);
    RUN(test_screen_field_reset);
    RUN(test_screen_failed_key_recovers);
    RUN(test_screen_opens_an_app_and_comes_back);
    RUN(test_done_goes_back);
}

#ifndef ROC_TEST_NO_MAIN
int main(void) {
    roc_tests();
    if (failures > 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
#endif
