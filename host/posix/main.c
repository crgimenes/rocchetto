#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "../../src/roc.h"

enum { POLL_MS = 50 };

static struct termios saved_termios;
static int termios_saved = 0;
static volatile sig_atomic_t winch = 0; /* set by SIGWINCH only */

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

/* Pending request queue (single slot: the core has one request in flight).
   The request callback must not call back into the core, so it only records. */
typedef struct {
    char root[1024];
    char home[1024]; /* --home: the kept home's file; empty = kept nowhere */
    uint32_t req_id;
    char path[VFS_PATH_MAX];
    int pending;
} host_state;

/* download: the file lands in the current directory, like curl -O; the
   name is the shell's own base name, never a path */
static void download(void *ctx, const char *name, const uint8_t *data, size_t len) {
    (void)ctx;
    if (name[0] == '\0' || strchr(name, '/') != NULL || name[0] == '.') {
        return;
    }
    FILE *f = fopen(name, "wb");
    if (f == NULL) {
        return;
    }
    (void)fwrite(data, 1, len, f);
    (void)fclose(f);
}

/* the callback's shape is the host contract's */
// cppcheck-suppress constParameterCallback
static uint32_t store_put(void *ctx, const uint8_t *data, size_t len) {
    const host_state *h = ctx;
    FILE *f = fopen(h->home, "wb");
    if (f == NULL) {
        return 1;
    }
    size_t w = fwrite(data, 1, len, f);
    if (fclose(f) != 0 || w != len) {
        return 1;
    }
    return 0;
}

static void on_request(void *ctx, uint32_t req_id, const char *path) {
    host_state *h = ctx;
    h->req_id = req_id;
    snprintf(h->path, sizeof(h->path), "%s", path);
    h->pending = 1;
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

static int path_is_safe(const char *path) {
    if (path[0] != '/') {
        return 0;
    }
    return strstr(path, "..") == NULL;
}

/* Opens the file backing a logical path. Mirrors the web glue's mapping so
   a Hugo output tree (public/) serves the same content as the site: the
   index may be mshindex.tsv and /x/y.md may live at /x/y/index.md. */
static int open_logical(const host_state *h, const char *path) {
    char full[1280];
    if (strcmp(path, ROC_HOME_PATH) == 0) {
        return h->home[0] != '\0' ? open(h->home, O_RDONLY) : -1;
    }
    if (strcmp(path, ROC_INDEX_PATH) == 0) {
        snprintf(full, sizeof(full), "%s/.index", h->root);
        int fd = open(full, O_RDONLY);
        if (fd >= 0) {
            return fd;
        }
        snprintf(full, sizeof(full), "%s/mshindex.tsv", h->root);
        return open(full, O_RDONLY);
    }
    snprintf(full, sizeof(full), "%s%s", h->root, path);
    int fd = open(full, O_RDONLY);
    if (fd >= 0) {
        return fd;
    }
    size_t len = strlen(path);
    if (len > 3 && strcmp(path + len - 3, ".md") == 0) {
        snprintf(full, sizeof(full), "%s%.*s/index.md", h->root, (int)(len - 3), path);
        return open(full, O_RDONLY);
    }
    return -1;
}

static void serve_request(roc *m, host_state *h) {
    h->pending = 0;
    if (!path_is_safe(h->path)) {
        roc_feed_fail(m, h->req_id);
        return;
    }
    int fd = open_logical(h, h->path);
    if (fd < 0) {
        roc_feed_fail(m, h->req_id);
        drain(m);
        return;
    }
    uint8_t buf[ROC_FEED_MAX];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            close(fd);
            roc_feed_fail(m, h->req_id);
            drain(m);
            return;
        }
        if (n == 0) {
            break;
        }
        roc_feed(m, h->req_id, buf, (size_t)n);
        drain(m); /* the core's overflow guard depends on draining between feeds */
    }
    close(fd);
    roc_feed_eof(m, h->req_id);
    drain(m);
}

/* The wall clock, and how far east of UTC this machine's zone is. */
static bool clock_now(void *ctx, int64_t *secs, int32_t *tz_minutes) {
    (void)ctx;
    time_t now = time(NULL);
    struct tm local;
    if (localtime_r(&now, &local) == NULL) {
        return false;
    }
    *secs = (int64_t)now;
    *tz_minutes = (int32_t)(local.tm_gmtoff / 60);
    return true;
}

/* The tools' scratch: a reservation the system pages in only as it is
   touched, so it costs what a tool uses. */
static void *scratch(void *ctx, size_t need) {
    static uint8_t region[64U << 20U];
    (void)ctx;
    if (need > sizeof(region)) {
        return NULL;
    }
    return region;
}

int main(int argc, char **argv) {
    static roc m; /* ~1.5MB of context: static, not on the stack */
    static host_state h;

    const char *root = getenv("ROC_ROOT") != NULL ? getenv("ROC_ROOT") : ".";
    const char *home = NULL;
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("rocchetto %s\n", ROC_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("usage: rocchetto [--home <file>] [root-dir]\n"
                   "Runs the rocchetto shell over the tree rooted at root-dir\n"
                   "(default: $ROC_ROOT or '.').\n"
                   "The best root is a Hugo output tree (a site's public/,\n"
                   "after running 'hugo'): its mshindex.tsv gives the same\n"
                   "logical tree the web version browses. Any plain directory\n"
                   "works too via a '.index' from tools/mkindex.sh.\n"
                   "--home <file>  keep the user's home in this file between\n"
                   "               runs (what the browser does in its storage).\n"
                   "--version      the version, and nothing else\n"
                   "ROC_HOST_NAME  the prompt's @ (default: this machine's name)\n"
                   "ROC_SITE_BASE  the origin the tree's links are relative to\n");
            return 0;
        }
        if (strcmp(argv[i], "--home") == 0 && i + 1 < argc) {
            home = argv[i + 1];
            i += 2;
            continue;
        }
        root = argv[i];
        i++;
    }
    snprintf(h.root, sizeof(h.root), "%s", root);
    if (home != NULL) {
        snprintf(h.home, sizeof(h.home), "%s", home);
    }

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

    static char machine[256];
    const char *name = getenv("ROC_HOST_NAME");
    if (name == NULL && gethostname(machine, sizeof(machine) - 1) == 0) {
        for (size_t k = 0; k < sizeof(machine) && machine[k] != '\0'; k++) {
            if (machine[k] == '.') {
                machine[k] = '\0'; /* "box", not "box.local" */
                break;
            }
        }
        name = machine;
    }
    roc_host host = {
        .filo_extend = roc_host_extend,
        .ctx = &h,
        .host_name = name,
        .site_base = getenv("ROC_SITE_BASE"),
        .request = on_request,
        .user = getenv("USER"),
        .download = download,
        .clock = clock_now,
        .scratch = scratch,
    };
    if (home != NULL) {
        host.store_put = store_put;
    }
    uint32_t flags = 0;
    if (getenv("ROC_NO_SPLASH") != NULL) {
        flags |= ROC_F_NO_SPLASH;
    }
    roc_init(&m, &host, cols, rows, flags);
    drain(&m);
    if (h.pending) {
        serve_request(&m, &h); /* the boot-time index request */
    }

    while (!roc_exited(&m)) {
        if (winch) {
            winch = 0;
            term_size(&cols, &rows);
            roc_resize(&m, cols, rows);
        }
        struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
        int pr = poll(&pfd, 1, POLL_MS);
        if (pr < 0) {
            continue; /* EINTR (e.g. SIGWINCH) */
        }
        if (pr == 0) {
            roc_tick(&m, POLL_MS);
            drain(&m);
            continue;
        }
        uint8_t buf[1024];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n < 0) {
            continue;
        }
        if (n == 0) {
            break; /* stdin closed (piped input) */
        }
        roc_input(&m, buf, (size_t)n);
        drain(&m);
        if (h.pending) {
            serve_request(&m, &h);
        }
    }
    drain(&m);
    if (tty) {
        /* whatever the show left behind, zsh gets a clean terminal */
        static const char reset[] = "\x1b[0m\x1b[?25h\x1b[?1007l";
        ssize_t rc = write(STDOUT_FILENO, reset, sizeof(reset) - 1);
        (void)rc;
    }
    return 0;
}
