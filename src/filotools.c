#include "filotools.h"

#include <stdio.h>
#include <string.h>

#include "debug.h"
#include "fbc_decompile.h"
#include "fbc_dump.h"
#include "filo_fmt.h"
#include "filo_nolibc.h"
#include "roc.h"
#include "script.h"
#include "sh.h"
#if ROC_APP_SCREENS
#include "screen.h"
#endif

enum {
    ARGS_MAX = 64,
    FILE_CAP = 1U << 20U,
    WORK_MAX = 32U << 20U, /* the most one formatting or decompiling may take */
};

/* In the scratch, taken when a tool starts: what build and bundle write. */
static uint8_t *out_buf;
static fbc_unit listing;

/* src formatted as filofmt does, in scratch memory: from what it takes (17.4
   times the source, measured, linear) up, doubling while it does not fit.
   NULL when it cannot be. The caller gives the memory back to its mark. */
static const char *format(roc *m, const char *src, size_t len, size_t *out_len) {
    size_t mark = roc_scratch_mark(m);
    for (size_t cap = (len * 18U) + 65536U; cap <= WORK_MAX; cap *= 2U) {
        void *mem = roc_scratch_take(m, cap);
        if (mem == NULL) {
            return NULL;
        }
        const char *text = filo_fmt(src, len, 2, 80, mem, cap, out_len);
        if (text != NULL) {
            return text;
        }
        roc_scratch_release(m, mark);
    }
    return NULL;
}

typedef struct {
    const char *name;
    const char *synopsis;
    const char *text;
} tool;

static const tool tools[] = {
    {
        "run",
        "filo run FILE [ARGS]",
        "runs a file: a program by its first bytes, anything else as Filo source (filo FILE does "
        "the same).",
    },
    {
        "build",
        "filo build -o OUT FILE...",
        "compiles programs into one unit of bytecode, each an entry named by its file:\r\n"
        "the same bytes the desktop's filo build writes. OUT is in your home.",
    },
    {
        "bundle",
        "filo bundle -o OUT UNIT...",
        "puts units into one bundle, each a member named by its file.",
    },
    {
        "dump",
        "filo dump FILE",
        "lists a unit or a bundle: the names it imports, its globals, constants,\r\n"
        "entry points, and every function, instruction by instruction. A source is\r\n"
        "compiled first.",
    },
    {
        "show",
        "filo show tree|folded|ir FILE",
        "writes one stage of what the compiler makes of a source, each line with\r\n"
        "the line:column it came from.",
    },
    {
        "check",
        "filo check [-vm PROFILE] FILE",
        "says of each unit whether this board's commands give what it imports and\r\n"
        "the extern globals it reads (or the VM PROFILE lists, a name a line).",
    },
    {
        "fmt",
        "filo fmt [-w] FILE",
        "lays Filo source out as the desktop's filofmt does, byte for byte: on\r\n"
        "the terminal, or with -w back into the file (in your home).",
    },
    {
        "size",
        "filo size FILE",
        "says where a unit's bytes go: its header and sections.",
    },
    {
        "decompile",
        "filo decompile [-o DIR] FILE [MEMBER]",
        "writes a unit back as Filo, a top-level form a line, or with -o as\r\n"
        "DIR/NAME.filo in your home, which filo build makes the same unit of.\r\n"
        "Comments, layout and local names are not in the bytes: x y z, a b c here.",
    },
    {
        "debug",
        "filo debug [-src DIR] FILE [MEMBER] [ENTRY]",
        "steps an entry point of a unit, a bundle or a source: ENTRY, else main,\r\n"
        "else the first (of MEMBER, chosen the same way, for a bundle). The source\r\n"
        "on the left, the instructions or the bytes on the right, the calls below;\r\n"
        "s step, n next, i instruction, c continue, b back, r restart, space a\r\n"
        "breakpoint, x the bytes, h the help. The source of an entry NAME is\r\n"
        "NAME.filo, beside FILE or in DIR.",
    },
};

enum { NTOOLS = sizeof(tools) / sizeof(tools[0]) };

/* The listing's numbers as this shell writes them (FBC_HOST_NUMBERS): the
   runtime's own writer, exact without a C library. */
void fbc_number(double x, char *dst, size_t cap) {
    size_t n = filo_nolibc_num_to_str(NULL, x, dst, cap - 1);
    if (n == 0) {
        dst[n++] = '?';
    }
    dst[n] = '\0';
}

static void say(roc *m, const char *text) {
    term_puts(&m->t, text);
    term_puts(&m->t, "\r\n");
}

static void help(roc *m, const char *name) {
    if (name == NULL) {
        say(m, "usage: filo [FILE [ARGS]]");
        for (int i = 0; i < NTOOLS; i++) {
            term_puts(&m->t, "       ");
            say(m, tools[i].synopsis);
        }
        say(m, "\r\nfilo alone is the REPL; with a file it runs it. The rest are its tools,\r\n"
               "as the desktop's filo has them; filo TOOL -h says what one does.");
        return;
    }
    for (int i = 0; i < NTOOLS; i++) {
        if (strcmp(tools[i].name, name) == 0) {
            term_puts(&m->t, "usage: ");
            say(m, tools[i].synopsis);
            term_puts(&m->t, "\r\n");
            term_puts(&m->t, tools[i].name);
            term_puts(&m->t, " ");
            say(m, tools[i].text);
        }
    }
}

/* A listing's line on the terminal. */
static void line_out(void *user, const char *line) {
    say(user, line);
}

/* The file at the path typed, from the tree or the home. False (said) when
   it is not here: one of the site's is fetched only when run. */
static bool read_file(roc *m, const char *typed, char *full, size_t cap, const uint8_t **data,
                      size_t *len) {
    if (!roc_resolve_arg(m, typed, full, cap)) {
        return false;
    }
    const vfs_node *node = vfs_lookup(&m->fs, full);
    if (node == NULL || node->dir) {
        roc_err(m, "filo", typed, "No such file or directory");
        return false;
    }
    if (!roc_find_file(m, full, data, len)) {
        roc_err(m, "filo", typed, "On the site, not here: cp it home first");
        return false;
    }
    return true;
}

static const char *base_of(const char *path) {
    const char *b = strrchr(path, '/');
    return b != NULL ? b + 1 : path;
}

/* "lib/hello.filo" is the entry "hello". */
static void entry_of(const char *path, const char *ext, char *dst, size_t cap) {
    const char *b = base_of(path);
    size_t n = strlen(b);
    size_t e = strlen(ext);
    if (n > e && strcmp(b + n - e, ext) == 0) {
        n -= e;
    }
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, b, n);
    dst[n] = '\0';
}

static void say_ctx_error(roc *m, const char *where, const filo_ctx *ctx) {
    char text[FILO_ERROR_MAX + VFS_PATH_MAX + 8];
    uint32_t line = 0;
    uint32_t col = 0;
    const char *msg = filo_error(ctx);
    if (strncmp(msg, "parse error", 11) != 0 && filo_error_at(ctx, &line, &col)) {
        (void)snprintf(text, sizeof(text), "filo: %s:%u:%u: %s", where, line, col, msg);
    } else {
        (void)snprintf(text, sizeof(text), "filo: %s: %s", where, msg);
    }
    say(m, text);
}

/* Compiles the files into one unit in out_buf, each an entry named by its
   file; its length, 0 when it failed (said). */
static size_t build_units(roc *m, char **paths, int n) {
    static filo_prog progs[ARGS_MAX];
    static filo_bc_entry entries[ARGS_MAX];
    static char names[ARGS_MAX][64];
    filo_ctx *ctx = script_context(m);
    for (int i = 0; i < n; i++) {
        char full[VFS_PATH_MAX];
        const uint8_t *src = NULL;
        size_t len = 0;
        if (!read_file(m, paths[i], full, sizeof(full), &src, &len)) {
            return 0;
        }
        if (filo_compile(ctx, src, len, &progs[i]) != FILO_OK) {
            say_ctx_error(m, paths[i], ctx);
            return 0;
        }
        entry_of(full, ".filo", names[i], sizeof(names[i]));
        entries[i].name = names[i];
        entries[i].prog = &progs[i];
    }
    size_t len = 0;
    if (filo_bc_build(ctx, entries, (uint32_t)n, out_buf, FILE_CAP, &len) != FILO_OK) {
        say_ctx_error(m, "build", ctx);
        return 0;
    }
    return len;
}

/* out_buf's first len bytes to the path typed, in the home. */
static bool write_out(roc *m, const char *typed, size_t len) {
    char full[VFS_PATH_MAX];
    if (!roc_resolve_arg(m, typed, full, sizeof(full))) {
        return false;
    }
    return roc_write_file(m, "filo", full, out_buf, len);
}

static void cmd_build(roc *m, int argc, char **argv) {
    if (argc < 3 || strcmp(argv[0], "-o") != 0 || argc - 2 > ARGS_MAX) {
        help(m, "build");
        return;
    }
    size_t len = build_units(m, argv + 2, argc - 2);
    if (len > 0) {
        (void)write_out(m, argv[1], len);
    }
}

static void cmd_bundle(roc *m, int argc, char **argv) {
    static filo_bundle_member members[ARGS_MAX];
    static char names[ARGS_MAX][64];
    if (argc < 3 || strcmp(argv[0], "-o") != 0 || argc - 2 > ARGS_MAX) {
        help(m, "bundle");
        return;
    }
    int n = argc - 2;
    for (int i = 0; i < n; i++) {
        char full[VFS_PATH_MAX];
        const uint8_t *data = NULL;
        size_t len = 0;
        if (!read_file(m, argv[2 + i], full, sizeof(full), &data, &len)) {
            return;
        }
        if (fbc_kind(data, len) != 1) {
            roc_err(m, "filo", argv[2 + i], "Not a unit");
            return;
        }
        entry_of(full, ".fbc", names[i], sizeof(names[i]));
        members[i].name = names[i];
        members[i].data = data;
        members[i].len = len;
    }
    filo_ctx *ctx = script_context(m);
    size_t len = 0;
    if (filo_bundle_build(ctx, members, (uint32_t)n, out_buf, FILE_CAP, &len) != FILO_OK) {
        say_ctx_error(m, "bundle", ctx);
        return;
    }
    (void)write_out(m, argv[1], len);
}

/* The bytes a tool looks at: the file, or the unit its source compiles to.
   NULL when there are none (said). */
static const uint8_t *unit_of(roc *m, const char *typed, size_t *len) {
    char full[VFS_PATH_MAX];
    const uint8_t *data = NULL;
    if (!read_file(m, typed, full, sizeof(full), &data, len)) {
        return NULL;
    }
    if (fbc_kind(data, *len) != 0) {
        return data;
    }
    char *paths[1] = {(char *)typed};
    *len = build_units(m, paths, 1);
    return *len > 0 ? out_buf : NULL;
}

static void cmd_dump(roc *m, int argc, char **argv) {
    if (argc != 1) {
        help(m, "dump");
        return;
    }
    size_t len = 0;
    const uint8_t *data = unit_of(m, argv[0], &len);
    if (data == NULL) {
        return;
    }
    char why[128];
    if (fbc_kind(data, len) == 2) {
        static fbc_bundle b;
        if (!fbc_read_bundle(&b, data, len, why, sizeof(why))) {
            roc_err(m, "filo", argv[0], why);
            return;
        }
        fbc_dump_bundle(&b, line_out, m);
        return;
    }
    if (!fbc_read(&listing, data, len, why, sizeof(why))) {
        roc_err(m, "filo", argv[0], why);
        return;
    }
    fbc_dump(&listing, line_out, m);
}

static void cmd_show(roc *m, int argc, char **argv) {
    if (argc != 2) {
        help(m, "show");
        return;
    }
    char full[VFS_PATH_MAX];
    const uint8_t *src = NULL;
    size_t len = 0;
    if (!read_file(m, argv[1], full, sizeof(full), &src, &len)) {
        return;
    }
    filo_ctx *ctx = script_context(m);
    if (filo_show(ctx, src, len, argv[0], line_out, m) != FILO_OK) {
        say_ctx_error(m, argv[1], ctx);
    }
}

typedef struct {
    const filo_ctx *ctx;
    const char *profile;
    size_t len;
} offers;

/* Whether the VM gives name: the profile lists it (a name a line, "global
   NAME" for a value, "#" a comment), or with no profile, the commands' context has it, a builtin or
   a global the shell sets (USER, VERSION, ARGS). */
/* cppcheck-suppress constParameterCallback ; fbc_offers' shape */
static bool offered(void *user, const uint8_t *name, uint32_t n) {
    const offers *o = user;
    if (o->profile == NULL) {
        for (uint32_t i = 0; i < o->ctx->nbuiltins; i++) {
            const char *b = o->ctx->builtins[i].name;
            if (strlen(b) == n && memcmp(b, name, n) == 0) {
                return true;
            }
        }
        char global[64];
        filo_value v;
        if (n >= sizeof(global)) {
            return false;
        }
        memcpy(global, name, n);
        global[n] = '\0';
        return filo_get_global(o->ctx, global, &v);
    }
    for (size_t at = 0; at < o->len;) {
        size_t end = at;
        while (end < o->len && o->profile[end] != '\n') {
            end++;
        }
        size_t a = at;
        size_t z = end;
        while (a < z && (o->profile[a] == ' ' || o->profile[a] == '\t' || o->profile[a] == '\r')) {
            a++;
        }
        while (z > a && (o->profile[z - 1] == ' ' || o->profile[z - 1] == '\t' ||
                         o->profile[z - 1] == '\r')) {
            z--;
        }
        static const char global[] = "global ";
        if (z - a > sizeof(global) - 1 && memcmp(o->profile + a, global, sizeof(global) - 1) == 0) {
            a += sizeof(global) - 1;
        }
        if (z > a && o->profile[a] != '#' && z - a == n && memcmp(o->profile + a, name, n) == 0) {
            return true;
        }
        at = end + 1;
    }
    return false;
}

/* Whether a program is an app (an entry draw, which runs as a screen) and
   not a command: the unit, or a member of the bundle. */
static bool is_app(roc *m, const uint8_t *data, size_t len) {
    if (fbc_kind(data, len) == 1) {
        return filo_bc_declares(data, len, "draw");
    }
    filo_ctx *ctx = script_context(m);
    uint32_t count = 1;
    for (uint32_t i = 0; i < count; i++) {
        filo_str name = {NULL, 0};
        const uint8_t *unit = NULL;
        size_t ulen = 0;
        if (filo_bundle_at(ctx, data, len, i, &count, &name, &unit, &ulen) != FILO_OK) {
            return false;
        }
        if (filo_bc_declares(unit, ulen, "draw")) {
            return true;
        }
    }
    return false;
}

/* The context a program runs in on this shell: a screen's for an app, the
   commands' for the rest. */
static const filo_ctx *context_for(roc *m, const uint8_t *data, size_t len) {
#if ROC_APP_SCREENS
    if (is_app(m, data, len)) {
        static filo_ctx app;
        static uint8_t persistent[64U * 1024U];
        static uint8_t run[16U * 1024U];
        screen_context_init(m, &app, persistent, sizeof(persistent), run, sizeof(run));
        return &app;
    }
#else
    (void)data;
    (void)len;
#endif
    return script_context(m);
}

static void cmd_check(roc *m, int argc, char **argv) {
    offers o = {NULL, NULL, 0};
    int i = 0;
    if (argc >= 2 && strcmp(argv[0], "-vm") == 0) {
        char full[VFS_PATH_MAX];
        const uint8_t *text = NULL;
        if (!read_file(m, argv[1], full, sizeof(full), &text, &o.len)) {
            return;
        }
        o.profile = (const char *)text;
        i = 2;
    }
    if (argc - i != 1) {
        help(m, "check");
        return;
    }
    char full[VFS_PATH_MAX];
    const uint8_t *data = NULL;
    size_t len = 0;
    if (!read_file(m, argv[i], full, sizeof(full), &data, &len)) {
        return;
    }
    if (o.profile == NULL) {
        o.ctx = context_for(m, data, len);
    }
    char why[160];
    if (fbc_check(data, len, base_of(full), offered, &o, line_out, m, why, sizeof(why)) < 0) {
        roc_err(m, "filo", argv[i], why);
    }
}

static void cmd_size(roc *m, int argc, char **argv) {
    if (argc != 1) {
        help(m, "size");
        return;
    }
    char full[VFS_PATH_MAX];
    const uint8_t *data = NULL;
    size_t len = 0;
    if (!read_file(m, argv[0], full, sizeof(full), &data, &len)) {
        return;
    }
    char why[160];
    if (!fbc_size(data, len, base_of(full), line_out, m, why, sizeof(why))) {
        roc_err(m, "filo", argv[0], why);
    }
}

/* text, a line at a time, on the terminal. */
static void say_text(roc *m, const char *text, size_t len) {
    for (size_t at = 0; at < len;) {
        size_t end = at;
        while (end < len && text[end] != '\n') {
            end++;
        }
        char line[1024];
        size_t n = end - at < sizeof(line) - 1 ? end - at : sizeof(line) - 1;
        memcpy(line, text + at, n);
        line[n] = '\0';
        say(m, line); /* a longer line is cut on the screen, not in a file */
        at = end + 1;
    }
}

static void cmd_fmt(roc *m, int argc, char **argv) {
    bool rewrite = false;
    if (argc == 2) {
        rewrite = strcmp(argv[0], "-w") == 0;
    }
    if (argc != (rewrite ? 2 : 1)) {
        help(m, "fmt");
        return;
    }
    const char *typed = argv[rewrite ? 1 : 0];
    char full[VFS_PATH_MAX];
    const uint8_t *src = NULL;
    size_t len = 0;
    if (!read_file(m, typed, full, sizeof(full), &src, &len)) {
        return;
    }
    size_t n = 0;
    const char *text = format(m, (const char *)src, len, &n);
    if (text == NULL) {
        roc_err(m, "filo", typed, "Too large to format");
        return;
    }
    if (!rewrite) {
        say_text(m, text, n);
        return;
    }
    if (n != len || memcmp(text, src, n) != 0) {
        (void)roc_write_file(m, "filo", full, (const uint8_t *)text, n);
    }
}

/* ---- decompile ---- */

typedef struct {
    roc *m;
    const char *dir; /* -o, typed; NULL for the terminal */
    uint32_t total;
    uint32_t count;
    bool failed;
    bool dry; /* a run only to learn how much memory the real one takes */
} sources;

static void put_text(sources *w, const char *name, size_t nlen, const char *text, size_t len);

static void first_line(void *user, const char *line) {
    char *keep = user;
    if (keep[0] == '\0') {
        (void)snprintf(keep, 128, "%s", line);
    }
}

/* Whether the commands' folder turns call into a constant: its folded tree
   is a number, a bool or a string. */
static bool folds(void *user, const char *call, size_t len) {
    filo_ctx *ctx = user;
    char root[128] = "";
    if (filo_show(ctx, (const uint8_t *)call, len, "folded", first_line, root) != FILO_OK) {
        return false;
    }
    const char *what = strchr(root, ' ');
    while (what != NULL && *what == ' ') {
        what++;
    }
    if (what == NULL) {
        return false;
    }
    if (strncmp(what, "number ", 7) == 0 || strncmp(what, "bool ", 5) == 0) {
        return true;
    }
    return strncmp(what, "string ", 7) == 0;
}

/* An entry point's text: on the terminal, a line at a time, or as a file in
   the directory, its path said. */
static void put_source(void *user, const char *name, size_t nlen, const char *raw, size_t rawlen) {
    sources *w = user;
    if (w->failed) {
        return;
    }
    if (w->dry) {
        return;
    }
    size_t len = 0;
    size_t mark = roc_scratch_mark(w->m); /* each entry's text, then the room again */
    const char *text = format(w->m, raw, rawlen, &len);
    if (text == NULL) {
        roc_err(w->m, "filo", name, "Too large to format");
        w->failed = true;
        return;
    }
    put_text(w, name, nlen, text, len);
    roc_scratch_release(w->m, mark);
}

/* A formatted entry: its file, or its lines on the terminal. */
static void put_text(sources *w, const char *name, size_t nlen, const char *text, size_t len) {
    if (w->dir != NULL) {
        char path[VFS_PATH_MAX];
        (void)snprintf(path, sizeof(path), "%s/%.*s.filo", w->dir, (int)nlen, name);
        char full[VFS_PATH_MAX];
        if (!roc_resolve_arg(w->m, path, full, sizeof(full)) ||
            !roc_write_file(w->m, "filo", full, (const uint8_t *)text, len)) {
            w->failed = true;
            return;
        }
        say(w->m, full);
        return;
    }
    if (w->total > 1) {
        char head[96];
        (void)snprintf(head, sizeof(head), "%s; %.*s.filo", w->count > 0 ? "\r\n" : "", (int)nlen,
                       name);
        say(w->m, head);
    }
    say_text(w->m, text, len);
    w->count++;
}

static void cmd_decompile(roc *m, int argc, char **argv) {
    sources w = {m, NULL, 0, 0, false, false};
    int i = 0;
    if (argc >= 2 && strcmp(argv[0], "-o") == 0) {
        w.dir = argv[1];
        i = 2;
    }
    if (argc - i < 1 || argc - i > 2) {
        help(m, "decompile");
        return;
    }
    char full[VFS_PATH_MAX];
    const uint8_t *data = NULL;
    size_t len = 0;
    if (!read_file(m, argv[i], full, sizeof(full), &data, &len)) {
        return;
    }
    filo_ctx *ctx = script_context(m);
    const uint8_t *unit = data;
    size_t ulen = len;
    if (fbc_kind(data, len) == 2) {
        char member[64];
        if (argc - i == 2) {
            (void)snprintf(member, sizeof(member), "%s", argv[i + 1]);
        } else {
            entry_of(full, ".fbb", member, sizeof(member));
        }
        if (!roc_program_unit(ctx, data, len, member, &unit, &ulen)) {
            say_ctx_error(m, argv[i], ctx);
            return;
        }
    } else if (fbc_kind(data, len) != 1) {
        roc_err(m, "filo", argv[i], "Not a unit or a bundle: a source is Filo already");
        return;
    }
    char why[128];
    if (!fbc_read(&listing, unit, ulen, why, sizeof(why))) {
        roc_err(m, "filo", argv[i], why);
        return;
    }
    w.total = listing.nexports;
    /* what it writes goes out as it goes, so the memory it takes is found
       first by runs that write nothing, doubling from 64 times the unit */
    size_t mark = roc_scratch_mark(m);
    void *mem = NULL;
    size_t cap = (ulen * 64U) + 65536U;
    w.dry = true;
    for (; cap <= WORK_MAX; cap *= 2U) {
        mem = roc_scratch_take(m, cap);
        if (mem == NULL ||
            fbc_decompile(&listing, mem, cap, folds, ctx, put_source, &w, why, sizeof(why))) {
            break;
        }
        roc_scratch_release(m, mark);
        mem = NULL;
    }
    w.dry = false;
    if (mem == NULL) {
        roc_err(m, "filo", argv[i], "Too large to decompile here");
        return;
    }
    if (!fbc_decompile(&listing, mem, cap, folds, ctx, put_source, &w, why, sizeof(why))) {
        roc_err(m, "filo", argv[i], why);
    }
}

/* ---- debug ---- */

/* The bundle's member named want, or "main", or the first. */
static bool member_of(roc *m, const uint8_t *data, size_t len, const char *want, const char *typed,
                      const uint8_t **unit, size_t *ulen) {
    filo_ctx *ctx = script_context(m);
    const char *name = want != NULL ? want : "main";
    uint32_t count = 1;
    for (uint32_t i = 0; i < count; i++) {
        filo_str n = {NULL, 0};
        const uint8_t *u = NULL;
        size_t ul = 0;
        if (filo_bundle_at(ctx, data, len, i, &count, &n, &u, &ul) != FILO_OK) {
            say_ctx_error(m, typed, ctx);
            return false;
        }
        bool named = false;
        if (n.len == strlen(name) && memcmp(n.ptr, name, n.len) == 0) {
            named = true;
        }
        if (named || (i == 0 && want == NULL)) {
            *unit = u;
            *ulen = ul;
        }
        if (named) {
            return true;
        }
    }
    if (want == NULL && *unit != NULL) {
        return true;
    }
    roc_err(m, "filo", typed, "No bundle member by that name");
    return false;
}

static void cmd_debug(roc *m, int argc, char **argv) {
    const char *src = NULL;
    int i = 0;
    if (argc >= 2 && strcmp(argv[0], "-src") == 0) {
        src = argv[1];
        i = 2;
    }
    int n = argc - i;
    if (n < 1 || n > 3) {
        help(m, "debug");
        return;
    }
    char full[VFS_PATH_MAX];
    const uint8_t *data = NULL;
    size_t len = 0;
    if (!read_file(m, argv[i], full, sizeof(full), &data, &len)) {
        return;
    }
    if (fbc_kind(data, len) == 0) { /* a source: compiled as build compiles it */
        char *paths[1] = {argv[i]};
        len = build_units(m, paths, 1);
        if (len == 0) {
            return;
        }
        data = out_buf;
    }
    const char *entry = n >= 2 ? argv[i + 1] : NULL;
    const uint8_t *unit = data;
    size_t ulen = len;
    if (fbc_kind(data, len) == 2) {
        unit = NULL;
        entry = n == 3 ? argv[i + 2] : NULL;
        if (!member_of(m, data, len, n >= 2 ? argv[i + 1] : NULL, argv[i], &unit, &ulen)) {
            return;
        }
    } else if (n == 3) {
        help(m, "debug");
        return;
    }
    char dir[VFS_PATH_MAX];
    if (src != NULL) {
        if (!roc_resolve_arg(m, src, dir, sizeof(dir))) {
            return;
        }
    } else {
        (void)snprintf(dir, sizeof(dir), "%s", full);
        char *slash = strrchr(dir, '/');
        if (slash != NULL) {
            slash[slash == dir ? 1 : 0] = '\0';
        }
    }
    (void)debug_enter(m, unit, ulen, entry, dir, argv[i]);
}

/* run: what filo FILE does, the words after FILE its arguments */
static void cmd_run(roc *m, int argc, char **argv) {
    static char args[(16 * SH_LINE_MAX) + 1];
    if (argc < 1) {
        help(m, "run");
        return;
    }
    if (!sh_join(argv + 1, argc - 1, args, sizeof(args))) {
        term_puts(&m->t, "rocchetto: Line too long\r\n");
        return;
    }
    script_run_path(m, argv[0], args);
}

/* ---- the dispatch ---- */

bool roc_filo_tool(roc *m, const char *arg) {
    static sh_line sl; /* the words as sh reads them */
    char why[64];
    if (!sh_split(arg, &sl, why, sizeof(why)) || sl.argc == 0) {
        return false;
    }
    char **argv = sl.argv;
    int argc = sl.argc;
    if (strcmp(argv[0], "-h") == 0 || strcmp(argv[0], "--help") == 0) {
        help(m, NULL);
        return true;
    }
    int t = -1;
    for (int i = 0; i < NTOOLS; i++) {
        if (strcmp(argv[0], tools[i].name) == 0) {
            t = i;
        }
    }
    if (t < 0) {
        return false;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            help(m, tools[t].name);
            return true;
        }
    }
    if (!m->indexed) {
        term_puts(&m->t, "rocchetto: no index\r\n");
        return true;
    }
    roc_scratch_reset(m); /* the tool running has it all */
    out_buf = roc_scratch_take(m, FILE_CAP);
    if (out_buf == NULL) {
        roc_err(m, "filo", tools[t].name, "Not enough memory here for filo's tools");
        return true;
    }
    argc--;
    char **rest = argv + 1;
    switch (t) {
    case 0:
        cmd_run(m, argc, rest);
        return true;
    case 1:
        cmd_build(m, argc, rest);
        return true;
    case 2:
        cmd_bundle(m, argc, rest);
        return true;
    case 3:
        cmd_dump(m, argc, rest);
        return true;
    case 4:
        cmd_show(m, argc, rest);
        return true;
    case 5:
        cmd_check(m, argc, rest);
        return true;
    case 6:
        cmd_fmt(m, argc, rest);
        return true;
    case 7:
        cmd_size(m, argc, rest);
        return true;
    case 8:
        cmd_decompile(m, argc, rest);
        return true;
    default:
        cmd_debug(m, argc, rest);
        return true;
    }
}
