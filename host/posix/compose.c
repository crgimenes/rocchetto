/* compose: the shell's screens, read from a directory and redrawn the moment
   you save one. It runs the same C and the same Filo the BBS runs, so what
   shows here is what the shell will show; the only difference is where the
   screens come from and what happens when one is broken. The shell keeps the
   last good frame and says nothing; here a broken screen is the whole point,
   so it takes the terminal and names the file. */
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "../../src/roc.h"

enum {
    POLL_MS = 50,
    WATCH_MS = 250, /* how often the tree is checked for a save */
    FILES_MAX = 256,
    TREE_BYTES = 1U << 20U,
    WALK_DEPTH = 3,
};

static struct termios saved_termios;
static int termios_saved = 0;
static volatile sig_atomic_t winch = 0;

static void on_winch(int sig) {
    (void)sig;
    winch = 1;
}

static void restore_termios(void) {
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
    }
}

static void term_size(uint16_t *cols, uint16_t *rows) {
    struct winsize ws;
    *cols = 80;
    *rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    }
}

static void out(const char *s) {
    ssize_t w = write(STDOUT_FILENO, s, strlen(s));
    (void)w;
}

/* cfmakeraw turns off ISIG, so Ctrl-C is a byte like any other and leaving is
   this tool's job. */
static int has_ctrl_c(const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (b[i] == 0x03) {
            return 1;
        }
    }
    return 0;
}

static void discard(roc *m) {
    uint8_t buf[TERM_OUT_CAP];
    term_out_read(&m->t, buf, sizeof(buf));
}

static void drain(roc *m) {
    uint8_t buf[TERM_OUT_CAP];
    size_t n = term_out_read(&m->t, buf, sizeof(buf));
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(STDOUT_FILENO, buf + off, n - off);
        if (w <= 0) {
            return;
        }
        off += (size_t)w;
    }
}

/* ---- the tree on disk ---- */

typedef struct {
    tree_file files[FILES_MAX];
    char paths[FILES_MAX][TREE_PATH_MAX];
    long long stamp; /* every file's size and mtime, folded together */
    size_t nfiles;
    size_t used;
    uint8_t bytes[TREE_BYTES];
    char root[1024];
    char problem[256];
} tree;

static tree T;

static void walk(tree *t, const char *dir, const char *prefix, int depth) {
    DIR *d = opendir(dir);
    if (d == NULL) {
        return;
    }
    for (;;) {
        const struct dirent *e = readdir(d);
        if (e == NULL) {
            break;
        }
        if (e->d_name[0] == '.') {
            continue;
        }
        char full[1280];
        char rel[TREE_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        snprintf(rel, sizeof(rel), "%s%s", prefix, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0) {
                char sub[TREE_PATH_MAX];
                snprintf(sub, sizeof(sub), "%s/", rel);
                walk(t, full, sub, depth - 1);
            }
            continue;
        }
        if (t->nfiles >= FILES_MAX || (size_t)st.st_size + 1 > TREE_BYTES - t->used) {
            snprintf(t->problem, sizeof(t->problem), "the tree does not fit: %s", rel);
            continue;
        }
        int fd = open(full, O_RDONLY);
        if (fd < 0) {
            continue;
        }
        ssize_t n = read(fd, t->bytes + t->used, (size_t)st.st_size);
        close(fd);
        if (n < 0) {
            continue;
        }
        snprintf(t->paths[t->nfiles], TREE_PATH_MAX, "%s", rel);
        t->files[t->nfiles].path = t->paths[t->nfiles];
        t->files[t->nfiles].data = t->bytes + t->used;
        t->files[t->nfiles].len = (size_t)n;
        t->nfiles++;
        t->used += (size_t)n;
        t->stamp += ((long long)st.st_mtime * 31) + st.st_size;
    }
    closedir(d);
}

/* Reads the whole tree. The stamp folds every file's size and mtime, so a
   save, a new file or a deleted one all change it. */
static void tree_load(tree *t) {
    t->nfiles = 0;
    t->used = 0;
    t->stamp = 0;
    t->problem[0] = '\0';
    walk(t, t->root, "", WALK_DEPTH);
    if (t->nfiles == 0 && t->problem[0] == '\0') {
        snprintf(t->problem, sizeof(t->problem),
                 "no screens in '%s' - expected a directory holding main/init.filo", t->root);
    }
    tree_set_source(t->files, t->nfiles);
}

static long long tree_stamp(const char *root, int depth) {
    long long stamp = 0;
    DIR *d = opendir(root);
    if (d == NULL) {
        return -1;
    }
    for (;;) {
        const struct dirent *e = readdir(d);
        if (e == NULL) {
            break;
        }
        if (e->d_name[0] == '.') {
            continue;
        }
        char full[1280];
        snprintf(full, sizeof(full), "%s/%s", root, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0) {
                stamp += tree_stamp(full, depth - 1);
            }
            continue;
        }
        stamp += ((long long)st.st_mtime * 31) + st.st_size;
    }
    closedir(d);
    return stamp;
}

/* ---- serving content, so the file areas have something to list ---- */

typedef struct {
    char root[1024];
    uint32_t req_id;
    char path[VFS_PATH_MAX];
    int pending;
} content;

static void on_request(void *ctx, uint32_t req_id, const char *path) {
    content *c = ctx;
    c->req_id = req_id;
    snprintf(c->path, sizeof(c->path), "%s", path);
    c->pending = 1;
}

static int open_logical(const content *c, const char *path) {
    char full[1280];
    if (strcmp(path, ROC_INDEX_PATH) == 0) {
        snprintf(full, sizeof(full), "%s/mshindex.tsv", c->root);
        int fd = open(full, O_RDONLY);
        if (fd >= 0) {
            return fd;
        }
        snprintf(full, sizeof(full), "%s/.index", c->root);
        return open(full, O_RDONLY);
    }
    snprintf(full, sizeof(full), "%s%s", c->root, path);
    int fd = open(full, O_RDONLY);
    if (fd >= 0) {
        return fd;
    }
    size_t len = strlen(path);
    if (len > 3 && strcmp(path + len - 3, ".md") == 0) {
        snprintf(full, sizeof(full), "%s%.*s/index.md", c->root, (int)(len - 3), path);
        return open(full, O_RDONLY);
    }
    return -1;
}

static void serve(roc *m, content *c) {
    c->pending = 0;
    if (c->path[0] != '/' || strstr(c->path, "..") != NULL) {
        roc_feed_fail(m, c->req_id);
        return;
    }
    int fd = open_logical(c, c->path);
    if (fd < 0) {
        roc_feed_fail(m, c->req_id);
        drain(m);
        return;
    }
    uint8_t buf[ROC_FEED_MAX];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        roc_feed(m, c->req_id, buf, (size_t)n);
        drain(m); /* the core's overflow guard depends on draining between feeds */
    }
    close(fd);
    roc_feed_eof(m, c->req_id);
    drain(m);
}

/* ---- showing a screen, and showing why it did not load ---- */

static char shown_name[SCR_NAME_MAX] = "main";
static char shown_arg[VFS_PATH_MAX] = "";

/* The shell would keep the last good frame and stay quiet. Here the error is
   what the author needs, so it takes the screen. */
static char problem_text[256];

static void show_problem(const char *what) {
    if (what != problem_text) {
        snprintf(problem_text, sizeof(problem_text), "%s", what);
    }
    out("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
    out("\x1b[1;31m screen did not load\x1b[0m\r\n\r\n ");
    out(problem_text);
    out("\r\n\r\n \x1b[2msave the file again to retry \xc2\xb7 Ctrl-C to leave\x1b[0m\r\n");
}

static void show_screen(roc *m) {
    if (T.problem[0] != '\0') {
        show_problem(T.problem);
        return;
    }
    if (!screen_load_with(m, shown_name, shown_arg)) {
        show_problem(screen_error(m));
        return;
    }
    problem_text[0] = '\0';
    drain(m);
}

/* Which screen is up, so a reload comes back to the same place, opened on the
   same thing. The argument lives in a global the screen can read, and the
   reload wipes the context, so it is copied out first. */
static void remember_screen(roc *m) {
    snprintf(shown_name, sizeof(shown_name), "%s", m->scr.name);
    shown_arg[0] = '\0';
    filo_value v;
    memset(&v, 0, sizeof(v));
    if (filo_get_global(&m->scr.ctx, "ARG", &v) && v.kind == FILO_STRING && v.u.str.len > 0) {
        size_t n = v.u.str.len;
        if (n > sizeof(shown_arg) - 1) {
            n = sizeof(shown_arg) - 1;
        }
        memcpy(shown_arg, v.u.str.ptr, n);
        shown_arg[n] = '\0';
    }
}

int main(int argc, char **argv) {
    static roc m;
    static content c;
    const char *dir = "screens";
    const char *start = "main";
    snprintf(c.root, sizeof(c.root), "%s", ".");

    int opt = 0;
    while ((opt = getopt(argc, argv, "r:s:h")) != -1) {
        if (opt == 'r') {
            snprintf(c.root, sizeof(c.root), "%s", optarg);
        } else if (opt == 's') {
            start = optarg;
        } else {
            printf("usage: compose [-r content-root] [-s screen] [screens-dir]\n"
                   "Draws the board's screens straight from a directory and\n"
                   "redraws them as they are saved. Same runtime the BBS runs.\n\n"
                   "  -r content-root  where mshindex.tsv and the articles are,\n"
                   "                   so the file areas list something real\n"
                   "                   (a Hugo output tree; default '.')\n"
                   "  -s screen        which screen to open (default 'main')\n"
                   "  screens-dir      the tree of screens (default 'screens')\n\n"
                   "Keys go to the screen, so the board is navigable. A screen\n"
                   "that fails to load takes the terminal and names the reason.\n"
                   "ROC_HOST_NAME names the board in the prompt.\n"
                   "Example: compose -r ../site/public screens\n");
            return opt == 'h' ? 0 : 2;
        }
    }
    if (optind < argc) {
        dir = argv[optind];
    }
    snprintf(T.root, sizeof(T.root), "%s", dir);

    int tty = isatty(STDIN_FILENO);
    if (tty) {
        if (tcgetattr(STDIN_FILENO, &saved_termios) == 0) {
            termios_saved = 1;
            atexit(restore_termios);
            struct termios raw = saved_termios;
            cfmakeraw(&raw);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_winch;
        sigaction(SIGWINCH, &sa, NULL);
    }

    uint16_t cols;
    uint16_t rows;
    term_size(&cols, &rows);
    tree_load(&T);

    roc_host host = {
        .filo_extend = roc_host_extend,
        .ctx = &c,
        .host_name = getenv("ROC_HOST_NAME"),
        .request = on_request,
        .user = getenv("USER"),
    };
    roc_init(&m, &host, cols, rows, ROC_F_NO_SPLASH);
    drain(&m);
    if (c.pending) {
        serve(&m, &c); /* the index, so the areas have entries */
    }
    /* the shell opens on "main" by itself once the index is in, so only a
       different screen needs asking for */
    snprintf(shown_name, sizeof(shown_name), "%s", start);
    if (strcmp(start, "main") != 0 || screen_error(&m)[0] != '\0' || T.problem[0] != '\0') {
        show_screen(&m);
    } else {
        drain(&m);
    }

    uint32_t since_watch = 0;
    while (!roc_exited(&m)) {
        if (winch) {
            winch = 0;
            term_size(&cols, &rows);
            roc_resize(&m, cols, rows);
            if (problem_text[0] != '\0') {
                discard(&m);
                show_problem(problem_text);
            } else {
                drain(&m);
            }
        }
        struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
        int pr = poll(&pfd, 1, POLL_MS);
        if (pr < 0) {
            continue;
        }
        if (pr == 0) {
            roc_tick(&m, POLL_MS);
            if (problem_text[0] != '\0') {
                discard(&m);
            } else {
                drain(&m);
            }
            since_watch += POLL_MS;
            if (since_watch >= WATCH_MS) {
                since_watch = 0;
                /* an unreadable root stamps -1 forever: reloading on that would
                   repaint the error over whatever the author is doing */
                long long stamp = tree_stamp(T.root, WALK_DEPTH);
                if (stamp >= 0 && stamp != T.stamp) {
                    remember_screen(&m);
                    tree_load(&T);
                    show_screen(&m);
                }
            }
            continue;
        }
        uint8_t buf[1024];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0 || has_ctrl_c(buf, (size_t)n)) {
            break;
        }
        if (problem_text[0] != '\0') {
            continue; /* the message stays put until the file is saved again */
        }
        roc_input(&m, buf, (size_t)n);
        drain(&m);
        if (c.pending) {
            serve(&m, &c);
        }
    }
    drain(&m);
    if (tty) {
        out("\x1b[0m\x1b[?25h\x1b[?1007l\x1b[?1049l");
    }
    return 0;
}
