#include "corewar.h"

#include <string.h>

#include "roc.h"
#include "screen.h"
#include "sh.h"

static const char manual[] = "/lib/roc/corewar/redcode.md";
static const char classics[] = "/lib/roc/warriors";

/* The warrior at path, as the shell has it: the tree, the home, the
   storage. */
static bool read_warrior(void *user, const char *arg, const uint8_t **data, size_t *len, char *why,
                         size_t cap) {
    roc *m = user;
    char path[VFS_PATH_MAX];
    const char *err = NULL;
    if (!roc_resolve_arg(m, arg, path, sizeof(path))) {
        err = "File name too long";
    } else if (vfs_lookup(&m->fs, path) == NULL) {
        err = "No such file or directory";
    } else if (vfs_lookup(&m->fs, path)->dir) {
        err = "Is a directory";
    } else if (!roc_find_file(m, path, data, len)) {
        err = "Operation not supported"; /* the site's: cp it first */
    }
    if (err == NULL) {
        return true;
    }
    size_t n = strlen(err) < cap - 1 ? strlen(err) : cap - 1;
    memcpy(why, err, n);
    why[n] = '\0';
    return false;
}

static void leave(void *user, const char *note) {
    roc_app_leave(user, note);
}

/* The shell's own terminal app around the arena: its ctx is the session,
   the arena's is the arena. */
static void on_key(void *ctx, uint32_t cp) {
    arena_app.on_key(&((roc *)ctx)->cw, cp);
}

static void on_resize(void *ctx) {
    arena_app.on_resize(&((roc *)ctx)->cw);
}

static void on_tick(void *ctx, uint32_t ms) {
    arena_app.on_tick(&((roc *)ctx)->cw, ms);
}

static const term_app board_arena = {
    .on_key = on_key,
    .on_resize = on_resize,
    .on_tick = on_tick,
};

static arena *cw(roc *m) {
    if (m->cw.host.read == NULL) {
        arena_host host = {
            .user = m,
            .read = read_warrior,
            .leave = leave,
            .first = {"/lib/roc/warriors/imp.red", "/lib/roc/warriors/dwarf.red"},
        };
        arena_init(&m->cw, &host, &m->t, &m->cmp.target, &m->cmp.shown);
    }
    return &m->cw;
}

static uint32_t seed_of(const roc *m) {
    return m->ticks ^ (m->up_ms * 2654435761U);
}

static void show(roc *m) {
    roc_app_enter(m, &board_arena);
    arena_show(&m->cw);
}

const char *corewar_fight(roc *m) {
    static char why[CW_NOTE_MAX];
    if (!arena_ready(cw(m), seed_of(m), why, sizeof(why))) {
        return why;
    }
    show(m);
    return "";
}

/* ---- the commands ---- */

static bool is_digits(const char *s) {
    if (s[0] == '\0') {
        return false;
    }
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return true;
}

static uint32_t to_u32(const char *s) {
    uint32_t v = 0;
    for (const char *p = s; *p != '\0' && v < 100000000U; p++) {
        v = (v * 10U) + (uint32_t)(*p - '0');
    }
    return v;
}

/* corewar and mars: one command under two names, cmd saying which. */
static void command(roc *m, const char *cmd, const char *rest) {
    if (!m->indexed) {
        term_puts(&m->t, "rocchetto: no index\r\n");
        return;
    }
    static sh_line sl;
    char why[CW_REPORT_MAX];
    if (!sh_split(rest, &sl, why, sizeof(why))) {
        roc_err(m, cmd, "", why);
        return;
    }
    arena *a = cw(m);
    uint32_t rounds = CW_ROUNDS;
    bool rounds_given = false;
    bool fixed = false;
    uint32_t at = 0;
    bool batch = roc_output_captured(m); /* a redirect wants the text, not the screen */
    uint32_t nf = 0;
    for (int i = 0; i < sl.argc; i++) {
        const char *w = sl.argv[i];
        if ((strcmp(w, "-r") == 0 || strcmp(w, "-F") == 0) && i + 1 < sl.argc &&
            is_digits(sl.argv[i + 1])) {
            if (w[1] == 'r') {
                rounds = to_u32(sl.argv[i + 1]);
                rounds_given = true;
            } else {
                fixed = true;
                at = to_u32(sl.argv[i + 1]);
            }
            i++;
        } else if (strcmp(w, "-b") == 0) {
            batch = true;
        } else if (w[0] == '-') {
            roc_err(m, cmd, w, "Invalid argument");
            return;
        } else {
            if (nf == 0) {
                arena_clear(a);
            }
            (void)arena_add(a, w);
            nf++;
        }
    }
    if (nf == 0 && !batch && strcmp(cmd, "corewar") == 0) {
        screen_enter(m, "corewar"); /* pick them first */
        return;
    }
    if (nf < 2 || nf > CW_FIGHTERS_MAX) {
        term_puts(&m->t, "usage: ");
        term_puts(&m->t, cmd);
        term_puts(&m->t, " <a.red> <b.red> [more...] [-r rounds] [-F position] [-b]\r\n");
        return;
    }
    if (fixed && !rounds_given) {
        rounds = 1; /* one position, one round, unless asked for more */
    }
    if (batch) {
        static char report[CW_REPORT_MAX];
        if (!arena_batch(a, seed_of(m), rounds, fixed, at, report, sizeof(report), why,
                         sizeof(why))) {
            roc_err(m, cmd, "", why);
            return;
        }
        term_puts(&m->t, report);
        return;
    }
    if (!arena_ready_with(a, seed_of(m), rounds, fixed, at, why, sizeof(why))) {
        roc_err(m, cmd, "", why);
        return;
    }
    a->s.report = strcmp(cmd, "mars") == 0; /* mars says the score on leaving */
    show(m);
}

void roc_cmd_corewar(roc *m, const char *rest) {
    command(m, "corewar", rest);
}

void roc_cmd_mars(roc *m, const char *rest) {
    command(m, "mars", rest);
}

/* ---- the pick's builtins past the list ---- */

static roc *roc_of(filo_ctx *ctx) {
    return ctx->host.user;
}

static arena *arena_of(filo_ctx *ctx) {
    return cw(roc_of(ctx));
}

static bool red(const char *path) {
    size_t n = strlen(path);
    if (n <= 4) {
        return false;
    }
    return strcmp(path + n - 4, ".red") == 0;
}

/* The .red files right under dir, as (path tag), appended to items. */
static int list_dir(filo_ctx *ctx, const roc *m, const char *dir, const char *tag,
                    filo_value *items, uint32_t cap, uint32_t *k) {
    for (size_t i = 0; i < m->fs.nnodes && *k < cap; i++) {
        const vfs_node *n = &m->fs.nodes[i];
        if (n->dir || !vfs_is_child(n, dir) || !red(n->path)) {
            continue;
        }
        filo_value parts[2] = {filo_cstring(n->path), filo_cstring(tag)};
        if (filo_tuple(ctx, parts, 2, &items[*k]) != FILO_OK) {
            return FILO_ERR;
        }
        (*k)++;
    }
    return FILO_OK;
}

/* (cw-library): the shell's warriors, then the .red files of the home. */
static int b_cw_library(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "cw-library takes no argument");
    }
    const roc *m = roc_of(ctx);
    enum { MAX = 64 };
    filo_value items[MAX];
    uint32_t k = 0;
    char home[VFS_PATH_MAX];
    if (list_dir(ctx, m, classics, "classic", items, MAX, &k) != FILO_OK) {
        return FILO_ERR;
    }
    if (roc_home_path(m, "", home, sizeof(home)) &&
        list_dir(ctx, m, home, "yours", items, MAX, &k) != FILO_OK) {
        return FILO_ERR;
    }
    return filo_list(ctx, items, k, out);
}

/* (cw-fight): the arena, once this run is over. */
static int b_cw_fight(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "cw-fight takes no argument");
    }
    static char why[CW_NOTE_MAX];
    if (!arena_ready(arena_of(ctx), seed_of(roc_of(ctx)), why, sizeof(why))) {
        *out = filo_cstring(why);
        return FILO_OK;
    }
    (void)screen_pend(roc_of(ctx), "fight");
    *out = filo_cstring("");
    return FILO_OK;
}

/* (cw-edit path): the editor on it, a name alone being one of the home's,
   back to the pick when it closes. */
static int b_cw_edit(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1 || a[0].kind != FILO_STRING || a[0].u.str.len == 0) {
        return filo_fail(ctx, "cw-edit expects a path");
    }
    roc *m = roc_of(ctx);
    char target[VFS_PATH_MAX + 8] = "edt:";
    size_t at = 4;
    if (a[0].u.str.ptr[0] != '/') {
        char home[VFS_PATH_MAX];
        if (!roc_home_path(m, "", home, sizeof(home))) {
            home[0] = '\0';
        }
        size_t h = strlen(home);
        if (at + h + 1 >= sizeof(target)) {
            *out = filo_cstring("File name too long");
            return FILO_OK;
        }
        memcpy(target + at, home, h);
        at += h;
        target[at++] = '/';
    }
    if (at + a[0].u.str.len + 1 > sizeof(target)) {
        *out = filo_cstring("File name too long");
        return FILO_OK;
    }
    memcpy(target + at, a[0].u.str.ptr, a[0].u.str.len);
    target[at + a[0].u.str.len] = '\0';
    (void)screen_pend(m, target);
    *out = filo_cstring("");
    return FILO_OK;
}

/* (cw-manual): the Redcode manual in the pager, over the pick. */
static int b_cw_manual(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "cw-manual takes no argument");
    }
    char target[sizeof(manual) + 4] = "man:";
    memcpy(target + 4, manual, sizeof(manual));
    (void)screen_pend(roc_of(ctx), target);
    *out = filo_cstring("");
    return FILO_OK;
}

void corewar_register(filo_ctx *ctx) {
    (void)arena_register(ctx, arena_of);
    (void)filo_register_builtin(ctx, "cw-library", b_cw_library);
    (void)filo_register_builtin(ctx, "cw-fight", b_cw_fight);
    (void)filo_register_builtin(ctx, "cw-edit", b_cw_edit);
    (void)filo_register_builtin(ctx, "cw-manual", b_cw_manual);
}
