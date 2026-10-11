#include <string.h>

#include "field.h"
#include "filo_math.h"
#include "filo_nolibc.h"
#include "paint.h"
#include "rng.h"
#include "roc.h"

/* Whole seconds connected: the bar shows a clock, not milliseconds. */
static uint32_t uptime_secs(const roc *m) {
    return m->up_ms / 1000;
}

static roc *roc_of(filo_ctx *ctx) {
    return (roc *)ctx->host.user;
}

static canvas *board_canvas(filo_ctx *ctx) {
    return &roc_of(ctx)->cmp.target;
}

/* ---- arguments ---- */

static const char *const hook_names[SCR_HOOK_COUNT] = {"draw", "key", "input", "tick"};

static bool copy_text(char *dst, size_t cap, cv_text t) {
    if (t.len + 1 > cap) {
        return false;
    }
    memcpy(dst, t.ptr, t.len);
    dst[t.len] = '\0';
    return true;
}

static bool path_join(char *dst, size_t cap, const char *dir, const char *file) {
    size_t n = strlen(dir);
    size_t k = strlen(file);
    if (n + 1 + k + 1 > cap) {
        return false;
    }
    memcpy(dst, dir, n);
    dst[n] = '/';
    memcpy(dst + n + 1, file, k);
    dst[n + 1 + k] = '\0';
    return true;
}

/* ---- the builtins a screen paints with ---- */

/* Drops a piece of ANSI art into the canvas at a position. The file is read
   from the screen's own directory, or from the root of the tree, and lands as
   a fragment: clipped at the edges, and unable to erase what is around it. */
static int b_blit(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 3) {
        return filo_fail(ctx, "blit expects a row, a column and a file");
    }
    int32_t row = 0;
    int32_t col = 0;
    cv_text file = {NULL, 0};
    if (paint_arg_cell(ctx, &a[0], "row", &row) != FILO_OK ||
        paint_arg_cell(ctx, &a[1], "column", &col) != FILO_OK ||
        paint_arg_text(ctx, &a[2], &file) != FILO_OK) {
        return FILO_ERR;
    }
    char name[TREE_PATH_MAX];
    if (!copy_text(name, sizeof(name), file)) {
        return filo_fail(ctx, "that file name is too long");
    }
    roc *m = roc_of(ctx);
    char path[TREE_PATH_MAX];
    const uint8_t *data = NULL;
    size_t len = 0;
    bool found = false;
    if (path_join(path, sizeof(path), m->scr.name, name) && tree_find_file(path, &data, &len)) {
        found = true;
    }
    if (!found && !tree_find_file(name, &data, &len)) {
        return filo_fail2(ctx, "no such art in the screens tree: ", name);
    }
    cv_capture cap;
    cv_capture_begin_at(&cap, &m->cmp.target, row, col);
    cv_capture_feed(&cap, data, len);
    *out = filo_bool(true);
    return FILO_OK;
}

/* random: a whole number in [0, n). It is a builtin of the shell and not
   of the language — the corpus is shared with the Go engine and a corpus
   case cannot have an answer that changes every run. The stream is the
   session's, seeded from the ticks, so two boards do not play the same
   game. */
static int b_random(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "random expects one bound");
    }
    int32_t bound = 0;
    if (paint_arg_whole(ctx, &a[0], "bound", &bound) != FILO_OK) {
        return FILO_ERR;
    }
    if (bound <= 0) {
        return filo_fail(ctx, "random expects a bound above zero");
    }
    roc *m = roc_of(ctx);
    m->scr.seed = splitmix64(&m->scr.seed);
    *out = filo_num((double)(m->scr.seed % (uint64_t)bound));
    return FILO_OK;
}

/* ---- the index ---- */

/* A directory name from a script, as the vfs wants it. */
static int arg_dir(filo_ctx *ctx, const filo_value *v, char *dst, size_t cap) {
    cv_text t = {NULL, 0};
    if (paint_arg_text(ctx, v, &t) != FILO_OK) {
        return FILO_ERR;
    }
    if (t.len + 1 > cap) {
        return filo_fail(ctx, "that directory name is too long");
    }
    memcpy(dst, t.ptr, t.len);
    dst[t.len] = '\0';
    return FILO_OK;
}

static const vfs_node *nth_child(const roc *m, const char *dir, uint32_t want, uint32_t *total) {
    const vfs_node *found = NULL;
    uint32_t seen = 0;
    size_t i = 0;
    while (i < m->fs.nnodes) {
        const vfs_node *n = &m->fs.nodes[i];
        i++;
        if (n->dir || !vfs_is_child(n, dir)) {
            continue;
        }
        if (seen == want && found == NULL) {
            found = n;
        }
        seen++;
    }
    if (total != NULL) {
        *total = seen;
    }
    return found;
}

static int b_entry_count(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "entry-count expects a directory");
    }
    char dir[VFS_PATH_MAX];
    if (arg_dir(ctx, &a[0], dir, sizeof(dir)) != FILO_OK) {
        return FILO_ERR;
    }
    uint32_t total = 0;
    (void)nth_child(roc_of(ctx), dir, UINT32_MAX, &total);
    *out = filo_num(total);
    return FILO_OK;
}

/* One entry as a tuple, which letv takes apart:
   (letv (path title date size) (entry-at "/pub" i) ...) */
static int b_entry_at(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 2) {
        return filo_fail(ctx, "entry-at expects a directory and a position");
    }
    char dir[VFS_PATH_MAX];
    int32_t at = 0;
    if (arg_dir(ctx, &a[0], dir, sizeof(dir)) != FILO_OK ||
        paint_arg_cell(ctx, &a[1], "position", &at) != FILO_OK) {
        return FILO_ERR;
    }
    if (at < 0) {
        return filo_fail(ctx, "entry-at: no entry there");
    }
    const vfs_node *node = nth_child(roc_of(ctx), dir, (uint32_t)at, NULL);
    if (node == NULL) {
        return filo_fail(ctx, "entry-at: no entry there");
    }
    filo_value parts[4];
    parts[0] = filo_cstring(node->path);
    parts[1] = filo_cstring(node->title);
    parts[2] = filo_cstring(node->date);
    parts[3] = filo_num(node->size);
    return filo_tuple(ctx, parts, 4, out);
}

/* (cursor-at row col): the terminal's cursor sits there after the paint,
   with no field — an editor's caret. */
static int b_cursor_at(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 2) {
        return filo_fail(ctx, "cursor-at expects a row and a column");
    }
    int32_t row = 0;
    int32_t col = 0;
    if (paint_arg_cell(ctx, &a[0], "row", &row) != FILO_OK ||
        paint_arg_cell(ctx, &a[1], "column", &col) != FILO_OK) {
        return FILO_ERR;
    }
    screen_state *s = &roc_of(ctx)->scr;
    s->has_cursor = true;
    s->caret_row = row;
    s->caret_col = col;
    *out = filo_bool(true);
    return FILO_OK;
}

/* The pager over the program, once this run is over: the pending move the
   board settles, as exec's. */
static bool pend(screen_state *s, const char *a, const uint8_t *b, size_t blen) {
    size_t alen = strlen(a);
    if (alen + blen + 1 > sizeof(s->pending)) {
        return false;
    }
    memcpy(s->pending, a, alen);
    if (blen > 0) {
        memcpy(s->pending + alen, b, blen);
    }
    s->pending[alen + blen] = '\0';
    s->nfx = 0;
    return true;
}

bool screen_pend(roc *m, const char *target) {
    return pend(&m->scr, target, NULL, 0);
}

/* (pager-file path): "" when the file is about to be shown, else why not. */
static int b_pager_file(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1 || a[0].kind != FILO_STRING) {
        return filo_fail(ctx, "pager-file expects a path");
    }
    roc *m = roc_of(ctx);
    char typed[VFS_PATH_MAX];
    char path[VFS_PATH_MAX];
    if (a[0].u.str.len == 0 || a[0].u.str.len >= sizeof(typed)) {
        *out = filo_cstring("No such file or directory");
        return FILO_OK;
    }
    memcpy(typed, a[0].u.str.ptr, a[0].u.str.len);
    typed[a[0].u.str.len] = '\0';
    const uint8_t *data = NULL;
    size_t len = 0;
    if (!roc_resolve_arg(m, typed, path, sizeof(path)) || !roc_find_file(m, path, &data, &len)) {
        *out = filo_cstring("No such file or directory");
        return FILO_OK;
    }
    if (!pend(&m->scr, "man:", (const uint8_t *)path, strlen(path))) {
        *out = filo_cstring("File name too long");
        return FILO_OK;
    }
    *out = filo_cstring("");
    return FILO_OK;
}

#if ROC_APP_EDIT
/* (pager-buffer): the text being edited, as the pager shows a file of that
   name — markdown rendered when it is one. */
static int b_pager_buffer(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "pager-buffer takes no argument");
    }
    (void)pend(&roc_of(ctx)->scr, "preview", NULL, 0);
    *out = filo_bool(true);
    return FILO_OK;
}
#endif

/* (done): the program is over. The shell goes back to whoever opened it:
   the screen RET names, else the screen that led here (a game chosen from
   a list goes back to the list), else what was under the shell. */
static int b_done(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "done takes no argument");
    }
    screen_state *s = &roc_of(ctx)->scr;
    filo_value ret = {0};
    const char *to = "back";
    size_t len = 4;
    if (filo_get_global(ctx, "RET", &ret) && ret.kind == FILO_STRING && ret.u.str.len > 0) {
        to = (const char *)ret.u.str.ptr;
        len = ret.u.str.len;
    } else if (s->back[0] != '\0') {
        to = s->back;
        len = strlen(s->back);
    }
    if (len + 1 > sizeof(s->pending)) {
        return filo_fail(ctx, "the screen name is too long");
    }
    memcpy(s->pending, to, len);
    s->pending[len] = '\0';
    s->nfx = 0;
    *out = filo_bool(true);
    return FILO_OK;
}

/* ---- the canvas as memory ---- */

/* (keep-canvas): the cells stay from one paint to the next instead of being
   blanked before each draw, so a screen can keep its state in them. */
static int b_keep_canvas(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "keep-canvas takes no argument");
    }
    roc_of(ctx)->scr.keep_canvas = true;
    *out = filo_bool(true);
    return FILO_OK;
}

/* Places the field the draw hook asked for and puts the caret in it. Both
   kinds of screen land here. */
void scr_field_at(roc *m, int32_t row, int32_t col, int32_t width, uint32_t most, bool secret) {
    screen_state *s = &m->scr;
    field_draw(&s->in, &m->cmp.target, row, col, width, most, secret, &s->caret_row, &s->caret_col);
}

static int b_input_at(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 5) {
        return filo_fail(ctx, "input-at expects a row, a column, a width, a maximum and "
                              "whether to hide what is typed");
    }
    int32_t row = 0;
    int32_t col = 0;
    int32_t width = 0;
    int32_t most = 0;
    if (paint_arg_cell(ctx, &a[0], "row", &row) != FILO_OK ||
        paint_arg_cell(ctx, &a[1], "column", &col) != FILO_OK ||
        paint_arg_cell(ctx, &a[2], "width", &width) != FILO_OK ||
        paint_arg_cell(ctx, &a[3], "maximum", &most) != FILO_OK) {
        return FILO_ERR;
    }
    if (a[4].kind != FILO_BOOL) {
        return filo_fail(ctx, "input-at expects #t or #f for hiding what is typed");
    }
    if (width < 1) {
        return filo_fail(ctx, "a field needs at least one column");
    }
    scr_field_at(roc_of(ctx), row, col, width, most > 0 ? (uint32_t)most : 0, a[4].u.b);
    *out = filo_bool(true);
    return FILO_OK;
}

/* (input-set TEXT): the field's text becomes TEXT, caret at the end: a form
   moving its one field to the next value. Inside the input hook it also
   stops the line from being cleared when the hook returns. */
static int b_input_set(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    cv_text text = {NULL, 0};
    if (n != 1 || paint_arg_text(ctx, &a[0], &text) != FILO_OK) {
        return n != 1 ? filo_fail(ctx, "input-set expects the text") : FILO_ERR;
    }
    screen_state *s = &roc_of(ctx)->scr;
    field_clear(&s->in);
    s->in.max = 0; /* the last paint's limit was another field's; the next paint sets this one's */
    size_t i = 0;
    while (i < text.len) {
        uint8_t b = (uint8_t)text.ptr[i];
        size_t more = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
        if (i + more >= text.len) {
            break; /* a rune cut at the end: stop before it */
        }
        uint32_t cp = more == 0 ? b : b & (uint32_t)(0x3F >> more);
        for (size_t k = 1; k <= more; k++) {
            cp = (cp << 6) | ((uint8_t)text.ptr[i + k] & 0x3FU);
        }
        field_key(&s->in, cp);
        i += more + 1;
    }
    s->in_seeded = true;
    *out = filo_bool(true);
    return FILO_OK;
}

/* The text as it stands. It lives in the shell, so a screen that wants to
   keep it says so by putting it in one of its own globals. */
static int b_input_text(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "input-text takes no arguments");
    }
    const screen_state *s = &roc_of(ctx)->scr;
    /* a copy: the field goes on changing under a value that pointed into it
       (a form keeping the text of one field while the next is typed) */
    uint8_t *copy = filo_alloc(ctx, s->in.len > 0 ? s->in.len : 1);
    if (copy == NULL) {
        return filo_fail(ctx, "input-text: out of memory");
    }
    memcpy(copy, s->in.buf, s->in.len);
    *out = filo_string(copy, (uint32_t)s->in.len);
    return FILO_OK;
}

/* (exec name) or (exec name arg): the program ends when this run does and
   name takes its place, a fresh one — its own globals, ARG set to arg. What
   name is, the shell decides: a screen, an app, or one of its own doors.
   The one move a framework written in Filo cannot write itself. */
static int b_exec(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    cv_text name = {NULL, 0};
    cv_text arg = {NULL, 0};
    if (n < 1 || n > 2 || paint_arg_text(ctx, &a[0], &name) != FILO_OK ||
        (n == 2 && paint_arg_text(ctx, &a[1], &arg) != FILO_OK)) {
        return n < 1 || n > 2 ? filo_fail(ctx, "exec expects a program and maybe an argument")
                              : FILO_ERR;
    }
    screen_state *s = &roc_of(ctx)->scr;
    size_t need = name.len + (n == 2 ? 1 + arg.len : 0) + 1;
    if (name.len == 0 || need > sizeof(s->pending)) {
        return filo_fail(ctx, "exec: the program's name is too long");
    }
    memcpy(s->pending, name.ptr, name.len);
    size_t k = name.len;
    if (n == 2) {
        s->pending[k++] = ':'; /* a program's name never holds a colon */
        memcpy(s->pending + k, arg.ptr, arg.len);
        k += arg.len;
    }
    s->pending[k] = '\0';
    *out = filo_bool(true);
    return FILO_OK;
}

/* (fx-next effect...): the effects the next exec plays on its way, from
   what the terminal shows to what the new program paints. The layer's:
   only one with effects registers it. */
static int b_fx_next(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    screen_state *s = &roc_of(ctx)->scr;
    if (n > SCR_FX_MAX) {
        return filo_fail(ctx, "that is more effects than a move can chain");
    }
    s->nfx = 0;
    for (uint32_t i = 0; i < n; i++) {
        cv_text one = {NULL, 0};
        if (paint_arg_text(ctx, &a[i], &one) != FILO_OK) {
            return FILO_ERR;
        }
        if (!copy_text(s->fx[s->nfx], SCR_FX_NAME, one) || !roc_layer_spec.effect(s->fx[s->nfx])) {
            return filo_fail2(ctx, "no such effect: ", s->fx[s->nfx]);
        }
        s->nfx++;
    }
    *out = filo_bool(true);
    return FILO_OK;
}

/* ---- the context ---- */

static bool fail_here(roc *m, const char *what) {
    screen_state *s = &m->scr;
    size_t n = 0;
    while (what[n] != '\0' && n + 1 < sizeof(s->error)) {
        s->error[n] = what[n];
        n++;
    }
    s->error[n] = '\0';
    return false;
}

static void error_put(screen_state *s, size_t *at, const char *text) {
    while (*text != '\0' && *at + 1 < sizeof(s->error)) {
        s->error[*at] = *text;
        (*at)++;
        text++;
    }
    s->error[*at] = '\0';
}

static void error_num(screen_state *s, size_t *at, uint32_t v) {
    char digits[12];
    size_t n = 0;
    do {
        digits[n] = (char)('0' + (v % 10U));
        n++;
        v /= 10U;
    } while (v > 0);
    while (n > 0 && *at + 1 < sizeof(s->error)) {
        n--;
        s->error[*at] = digits[n];
        (*at)++;
    }
    s->error[*at] = '\0';
}

/* A Filo error said the way a compiler says one: the file it came from and,
   when the runtime knows it, the line and column — "main/draw.filo:12:5:
   undefined global: x". A parse error already carries its place. */
static bool fail_filo(roc *m, const char *file) {
    screen_state *s = &m->scr;
    const char *msg = filo_error(&s->ctx);
    uint32_t line = 0;
    uint32_t col = 0;
    size_t at = 0;
    error_put(s, &at, file);
    if (strncmp(msg, "parse error", 11) != 0 && filo_error_at(&s->ctx, &line, &col)) {
        error_put(s, &at, ":");
        error_num(s, &at, line);
        error_put(s, &at, ":");
        error_num(s, &at, col);
    }
    error_put(s, &at, ": ");
    error_put(s, &at, msg);
    return false;
}

/* The file an entry was compiled from, for an error to name and for a
   screen running from its source to be read from: the shell's common file
   at the root of the tree, a screen's in its directory ("main/draw.filo"),
   an app's by its own name ("draw.filo"). */
static bool entry_file(const screen_state *s, const char *entry, char *dst, size_t cap) {
    char file[SCR_ENTRY_MAX + sizeof(".filo")];
    size_t n = strlen(entry);
    if (n >= SCR_ENTRY_MAX) {
        return false;
    }
    memcpy(file, entry, n);
    memcpy(file + n, ".filo", sizeof(".filo"));
    if (s->app || strcmp(entry, "common") == 0) {
        if (strlen(file) + 1 > cap) {
            return false;
        }
        strcpy(dst, file);
        return true;
    }
    return path_join(dst, cap, s->name, file);
}

/* The same, for a hook. */
static bool fail_hook(roc *m, const scr_hook *h) {
    char path[TREE_PATH_MAX];
    if (!entry_file(&m->scr, h->entry, path, sizeof(path))) {
        return fail_filo(m, h->entry);
    }
    return fail_filo(m, path);
}

void screen_context_init(roc *m, filo_ctx *ctx, uint8_t *persistent, size_t plen, uint8_t *run,
                         size_t rlen) {
    filo_host host = filo_nolibc_host;
    host.user = m;
    filo_init(ctx, &host, persistent, plen, run, rlen);
    (void)filo_strings_register(ctx, &filo_nolibc_strings);
    /* no libm here, so the pack registers only what it can compute itself:
       floor and its neighbours, which is what laying out a screen needs */
    (void)filo_math_register(ctx, NULL);
    paint_register(ctx, board_canvas);
    (void)filo_register_builtin(ctx, "blit", b_blit);
    (void)filo_register_builtin(ctx, "random", b_random);
    (void)filo_register_builtin(ctx, "exec", b_exec);
    if (roc_layer_spec.effect != NULL && roc_layer_spec.transition != NULL) {
        (void)filo_register_builtin(ctx, "fx-next", b_fx_next);
    }
    (void)filo_register_builtin(ctx, "input-at", b_input_at);
    (void)filo_register_builtin(ctx, "input-text", b_input_text);
    (void)filo_register_builtin(ctx, "input-set", b_input_set);
    (void)filo_register_builtin(ctx, "entry-count", b_entry_count);
    (void)filo_register_builtin(ctx, "entry-at", b_entry_at);
    (void)filo_register_builtin(ctx, "cursor-at", b_cursor_at);
    (void)filo_register_builtin(ctx, "done", b_done);
    (void)filo_register_builtin(ctx, "pager-file", b_pager_file);
#if ROC_APP_EDIT
    (void)filo_register_builtin(ctx, "pager-buffer", b_pager_buffer);
#endif
    (void)filo_register_builtin(ctx, "keep-canvas", b_keep_canvas);
#if ROC_APP_EDIT
    edit_register(ctx);
#endif
#if ROC_APP_COREWAR
    corewar_register(ctx);
#endif
    (void)filo_set_global(ctx, "W", filo_num(m->t.cols));
    (void)filo_set_global(ctx, "H", filo_num(m->t.rows));
    (void)filo_set_global(ctx, "A_BOLD", filo_num(CV_A_BOLD));
    (void)filo_set_global(ctx, "A_DIM", filo_num(CV_A_DIM));
    (void)filo_set_global(ctx, "A_REV", filo_num(CV_A_REV));
    (void)filo_set_global(ctx, "C_DEFAULT", filo_num(-1));
    (void)filo_set_global(ctx, "KEY", filo_num(0));
    (void)filo_set_global(ctx, "MS", filo_num(0));
    (void)filo_set_global(ctx, "ARG", filo_cstring(""));
    (void)filo_set_global(ctx, "BACK", filo_cstring(""));
    (void)filo_set_global(ctx, "VERSION", filo_cstring(ROC_VERSION));
    (void)filo_set_global(ctx, "NOTE", filo_cstring(""));
    (void)filo_set_global(ctx, "USER", filo_cstring(m->user));
    char home[VFS_PATH_MAX];
    if (!roc_home_path(m, "", home, sizeof(home))) {
        home[0] = '\0';
    }
    (void)filo_set_global(ctx, "HOME", filo_cstring(home));
    /* where the editor goes when it closes: the screen that opened it. Only
       the editor's own screen learns it (screen_load_with), or the screen it
       came back to would answer its done with itself. */
    (void)filo_set_global(ctx, "RET", filo_cstring(""));
    (void)filo_set_global(ctx, "UPTIME", filo_num(uptime_secs(m)));
    /* whether this paint starts from blank cells: always, unless the screen
       keeps its canvas — then only when it has to */
    (void)filo_set_global(ctx, "FRESH", filo_bool(true));
    /* the arrows and enter have no character a script could compare against */
    (void)filo_set_global(ctx, "KEY_UP", filo_num(FT_KEY_UP));
    (void)filo_set_global(ctx, "KEY_DOWN", filo_num(FT_KEY_DOWN));
    (void)filo_set_global(ctx, "KEY_LEFT", filo_num(FT_KEY_LEFT));
    (void)filo_set_global(ctx, "KEY_RIGHT", filo_num(FT_KEY_RIGHT));
    (void)filo_set_global(ctx, "KEY_ENTER", filo_num('\r'));
    (void)filo_set_global(ctx, "KEY_HOME", filo_num(FT_KEY_HOME));
    (void)filo_set_global(ctx, "KEY_END", filo_num(FT_KEY_END));
    (void)filo_set_global(ctx, "KEY_PGUP", filo_num(FT_KEY_PGUP));
    (void)filo_set_global(ctx, "KEY_PGDN", filo_num(FT_KEY_PGDN));
    (void)filo_set_global(ctx, "KEY_DEL", filo_num(FT_KEY_DEL));
    (void)filo_set_global(ctx, "KEY_INS", filo_num(FT_KEY_INS));
    (void)filo_set_global(ctx, "KEY_ESC", filo_num(FT_KEY_ESC));
    /* added to a key: Shift+Up is (+ KEY_UP KEY_SHIFT) */
    (void)filo_set_global(ctx, "KEY_SHIFT", filo_num(FT_KEY_SHIFT));
    (void)filo_set_global(ctx, "KEY_ALT", filo_num(FT_KEY_ALT));
    (void)filo_set_global(ctx, "KEY_CTRL", filo_num(FT_KEY_CTRL));
    if (m->host.filo_extend != NULL) {
        /* one that did not register is missing, and a program using it
           refuses to load, saying its name */
        (void)m->host.filo_extend(m->host.ctx, ctx);
    }
}

void screen_reset(roc *m) {
    screen_state *s = &m->scr;
    screen_context_init(m, &s->ctx, s->persistent, sizeof(s->persistent), s->run, sizeof(s->run));
    memset(s->hooks, 0, sizeof(s->hooks));
    s->unit = NULL;
    /* the ticks so far are all the entropy a board has, and they differ by
       how long someone took to get here */
    s->seed = ((uint64_t)m->ticks << 16U) ^ (uint64_t)m->up_ms ^ 0x9E3779B97F4A7C15ULL;
    s->key_cp = 0;
    s->nfx = 0;
    field_clear(&s->in); /* the caret too: one left past the end broke the next insert */
    s->has_cursor = false;
    s->cursor_shown = SCR_CURSOR_UNKNOWN;
    s->app = false;
    s->keep_canvas = false;
    s->fresh = true;
    s->in.on = false;
    s->pending[0] = '\0';
    s->error[0] = '\0';
    s->ready = true;
}

bool screen_set_draw(roc *m, const uint8_t *src, size_t len) {
    screen_state *s = &m->scr;
    if (!s->ready) {
        screen_reset(m);
    }
    s->hooks[SCR_HOOK_DRAW].loaded = false;
    s->hooks[SCR_HOOK_DRAW].bc = false;
    if (filo_compile(&s->ctx, src, len, &s->hooks[SCR_HOOK_DRAW].prog) != FILO_OK) {
        return fail_filo(m, "draw");
    }
    s->hooks[SCR_HOOK_DRAW].loaded = true;
    s->error[0] = '\0';
    return true;
}

/* A hook is a program compiled here or an entry of the screen's unit, named
   by the path of the file it was compiled from. */
static int hook_exec(roc *m, const scr_hook *h, uint32_t steps) {
    screen_state *s = &m->scr;
    filo_limits limits = {steps, 0};
    filo_value v = {0};
    if (!h->bc) {
        return filo_run(&s->ctx, &h->prog, &limits, &v);
    }
    return filo_bc_run(&s->ctx, s->unit, h->entry, &limits, &v);
}

/* Builds the screen in the canvas. Nothing leaves the shell here, which is
   what lets the boot show use a finished screen as its target. */
static bool compose(roc *m) {
    screen_state *s = &m->scr;
    if (!s->hooks[SCR_HOOK_DRAW].loaded) {
        return fail_here(m, "no screen to draw");
    }
    (void)filo_set_global(&s->ctx, "W", filo_num(m->t.cols));
    (void)filo_set_global(&s->ctx, "H", filo_num(m->t.rows));
    (void)filo_set_global(&s->ctx, "UPTIME", filo_num(uptime_secs(m)));
    /* A C app that closed left a line for the user ("Nobody is there.").
       It is the same line a screen writes for itself, so it arrives in the
       same global and the screen decides where to put it. */
    if (m->t.note[0] != '\0') {
        (void)filo_set_global(&s->ctx, "NOTE", filo_cstring(m->t.note));
        m->t.note[0] = '\0';
    }
    bool fresh = s->fresh;
    if (!s->keep_canvas) {
        fresh = true;
    }
    if (m->cmp.target.rows != m->t.rows || m->cmp.target.cols != m->t.cols) {
        fresh = true;
    }
    if (fresh) {
        cv_reset(&m->cmp.target, m->t.rows, m->t.cols);
    } else {
        cv_pen_reset(&m->cmp.target);
    }
    s->fresh = false;
    (void)filo_set_global(&s->ctx, "FRESH", filo_bool(fresh));
    s->has_cursor = false;
    s->in.on = false; /* the draw hook says whether this screen has one */
    if (hook_exec(m, &s->hooks[SCR_HOOK_DRAW], SCR_STEPS_DRAW) != FILO_OK) {
        if (s->hooks[SCR_HOOK_DRAW].entry[0] == '\0') {
            return fail_filo(m, "draw"); /* set with screen_set_draw, from no file */
        }
        return fail_hook(m, &s->hooks[SCR_HOOK_DRAW]);
    }
    return true;
}

bool screen_paint(roc *m) {
    screen_state *s = &m->scr;
    if (!compose(m)) {
        return false;
    }
    cv_flush(&m->t, &m->cmp.shown, &m->cmp.target);
    bool caret = s->in.on;
    if (s->has_cursor) {
        caret = true;
    }
    if (caret) {
        term_put_at(&m->t, (uint16_t)(s->caret_col + 1), (uint16_t)(s->caret_row + 1));
    }
    int8_t want = caret ? SCR_CURSOR_SHOWN : SCR_CURSOR_HIDDEN;
    if (want != s->cursor_shown) {
        term_puts(&m->t, caret ? "\x1b[?25h" : "\x1b[?25l");
        s->cursor_shown = want;
    }
    s->error[0] = '\0';
    return true;
}

static const term_app screen_app;

/* ---- loading a screen ---- */

/* Runs one entry of the program: from its unit, or its file compiled now
   when the screen runs from source. Absent is only an error when the
   caller says the program cannot do without it. */
static bool run_entry(roc *m, const char *entry, bool required) {
    screen_state *s = &m->scr;
    char path[TREE_PATH_MAX];
    if (!entry_file(s, entry, path, sizeof(path))) {
        return fail_here(m, "the program's name is too long");
    }
    const uint8_t *src = NULL;
    size_t len = 0;
    bool found = false;
    if (s->unit != NULL) {
        found = filo_bc_has(s->unit, entry);
    } else {
        found = tree_find_file(path, &src, &len);
    }
    if (!found) {
        if (!required) {
            return true;
        }
        (void)fail_here(m, "no such file in the screens tree: ");
        size_t n = strlen(s->error);
        size_t k = 0;
        while (path[k] != '\0' && n + k + 1 < sizeof(s->error)) {
            s->error[n + k] = path[k];
            k++;
        }
        s->error[n + k] = '\0';
        return false;
    }
    filo_limits limits = {SCR_STEPS_INIT, 0};
    filo_value v = {0};
    int rc = FILO_ERR;
    if (s->unit != NULL) {
        rc = filo_bc_run(&s->ctx, s->unit, entry, &limits, &v);
    } else {
        filo_prog prog;
        rc = filo_compile(&s->ctx, src, len, &prog);
        if (rc == FILO_OK) {
            rc = filo_run(&s->ctx, &prog, &limits, &v);
        }
    }
    if (rc != FILO_OK) {
        return fail_filo(m, path);
    }
    return true;
}

/* A program, from its unit or its source: the shell's common file, then its
   init, and from there the globals are closed — so a hook that names
   something the init never made fails to compile, right now. The hooks are
   the entries named for them, whichever it has; draw it must have. */
static bool start(roc *m) {
    screen_state *s = &m->scr;
    if (!run_entry(m, "common", false) || !run_entry(m, "init", true)) {
        return false;
    }
    filo_seal_globals(&s->ctx);
    for (size_t i = 0; i < SCR_HOOK_COUNT; i++) {
        scr_hook *h = &s->hooks[i];
        char path[TREE_PATH_MAX];
        strcpy(h->entry, hook_names[i]);
        if (s->unit != NULL) {
            h->bc = filo_bc_has(s->unit, h->entry);
            h->loaded = h->bc;
            continue;
        }
        const uint8_t *src = NULL;
        size_t len = 0;
        if (!entry_file(s, h->entry, path, sizeof(path)) || !tree_find_file(path, &src, &len)) {
            continue;
        }
        if (filo_compile(&s->ctx, src, len, &h->prog) != FILO_OK) {
            return fail_filo(m, path);
        }
        h->loaded = true;
    }
    if (!s->hooks[SCR_HOOK_DRAW].loaded) {
        return fail_here(m, "the program has no draw");
    }
    return true;
}

/* An app: the program NAME in PATH (the user's, or the shell's /bin), whose
   member NAME is its unit — the same bytes a desktop VM runs, so it names what it needs
   and the load refuses it when this shell lacks any. One without a draw is
   a command, not an app. False when there is no app, or when it did not
   load (s->error says). */
static bool app_load(roc *m, const char *name) {
    screen_state *s = &m->scr;
    char file[VFS_PATH_MAX];
    if (roc_path_find(m, name, file, sizeof(file)) != 'p') {
        /* the shell's own apps, which it opens by name and the shell does
           not run: a command of the same name stays the command */
        static const char apps[] = "/lib/roc/apps/";
        size_t n = strlen(name);
        file[0] = '\0';
        if (sizeof(apps) + n <= sizeof(file)) {
            memcpy(file, apps, sizeof(apps) - 1);
            memcpy(file + sizeof(apps) - 1, name, n + 1);
        }
    }
#if ROC_APP_EDIT
    /* one opened by its path, ~/mine.fbb, is that file, not /bin's */
    if (strcmp(m->ed.app, name) == 0 && m->ed.app_file[0] != '\0') {
        strcpy(file, m->ed.app_file);
    }
#endif
    const uint8_t *data = NULL;
    size_t len = 0;
    if (!roc_find_file(m, file, &data, &len)) {
        return false;
    }
    const uint8_t *unit = NULL;
    size_t unit_len = 0;
    if (!roc_program_unit(&s->ctx, data, len, name, &unit, &unit_len)) {
        return fail_filo(m, file);
    }
    if (!filo_bc_declares(unit, unit_len, "draw")) {
        return false; /* a command, not an app: a screen may have its name */
    }
    if (filo_bc_load(&s->ctx, unit, unit_len, &s->unit) != FILO_OK) {
        return fail_filo(m, file);
    }
    s->app = true;
    return true;
}

bool screen_load(roc *m, const char *name) {
    return screen_load_with(m, name, "");
}

/* A program may be opened on something: (exec "area" "/pub") hands "/pub"
   to the area screen, which reads it in ARG. The context is new every
   time, so the string costs nothing that lasts. What a name is: an app of
   its own (NAME in PATH), a screen of the bundle, or a screen from its
   source, as one being written in the composing tool. */
bool screen_load_with(roc *m, const char *name, const char *arg) {
    screen_state *s = &m->scr;
    if (strlen(name) + 1 > sizeof(s->name)) {
        return fail_here(m, "the screen name is too long");
    }
    bool quiet = s->quiet_load;
    /* the trail: a reload keeps it, stepping back shortens it, going on
       lengthens it by the screen being left */
    if (strcmp(s->name, name) != 0) {
        if (s->ntrail > 0 && strcmp(s->trail[s->ntrail - 1], name) == 0) {
            s->ntrail--;
        } else if (s->name[0] != '\0') {
            if (s->ntrail == SCR_TRAIL) {
                memmove(s->trail[0], s->trail[1], sizeof(s->trail) - sizeof(s->trail[0]));
                s->ntrail--;
            }
            strcpy(s->trail[s->ntrail], s->name);
            s->ntrail++;
        }
    }
    screen_reset(m);
    s->quiet_load = quiet;
    strcpy(s->name, name);
    strcpy(s->back, s->ntrail > 0 ? s->trail[s->ntrail - 1] : "");
#if ROC_APP_EDIT
    if (strcmp(name, m->ed.app) == 0) {
        (void)filo_set_global(&s->ctx, "RET", filo_cstring(m->ed.ret));
    }
#endif
    (void)filo_set_global(&s->ctx, "ARG", filo_cstring(arg));
    (void)filo_set_global(&s->ctx, "BACK", filo_cstring(s->back));
    if (!s->quiet_load) {
        m->cmp.shown.rows = 0; /* whatever was on the terminal is not ours */
    }

    bool program = app_load(m, name);
    if (s->error[0] != '\0') {
        return false;
    }
    const uint8_t *bundle = NULL;
    size_t bundle_len = 0;
    const uint8_t *unit = NULL;
    size_t unit_len = 0;
    if (!program && tree_find_file(SCR_BUNDLE, &bundle, &bundle_len) &&
        filo_bundle_find(&s->ctx, bundle, bundle_len, name, &unit, &unit_len) == FILO_OK &&
        filo_bc_load(&s->ctx, unit, unit_len, &s->unit) != FILO_OK) {
        return fail_filo(m, SCR_BUNDLE);
    }
    /* a name that is not a screen says so before the common file is
       compiled for nothing: on a board that memory may not be there */
    char path[TREE_PATH_MAX];
    const uint8_t *init = NULL;
    size_t init_len = 0;
    if (s->unit == NULL &&
        (!entry_file(s, "init", path, sizeof(path)) || !tree_find_file(path, &init, &init_len))) {
        return run_entry(m, "init", true);
    }
    if (!start(m)) {
        return false;
    }
    if (!s->quiet_load) {
        return screen_paint(m);
    }
    return compose(m);
}

bool screen_compose(roc *m, const char *name) {
    m->scr.quiet_load = true;
    bool ok = screen_load(m, name);
    m->scr.quiet_load = false;
    return ok;
}

typedef struct {
    filo_prog progs[SCR_UNIT_FILES];
    filo_bc_entry entries[SCR_UNIT_FILES];
    char names[SCR_UNIT_FILES][SCR_ENTRY_MAX];
    uint32_t n;
} unit_src;

/* Compiles one file into the unit. The ones that run at load run here too:
   def creates a global when it runs, so the set the hooks compile against is
   the one a load leaves behind. */
/* One file of the tree as the entry named for it: draw.filo is "draw". */
static bool unit_add(roc *m, unit_src *u, const char *path, bool required, bool run) {
    screen_state *s = &m->scr;
    const uint8_t *src = NULL;
    size_t len = 0;
    if (!tree_find_file(path, &src, &len)) {
        if (!required) {
            return true;
        }
        return fail_here(m, "the screen has no init.filo");
    }
    if (u->n >= SCR_UNIT_FILES) {
        return fail_here(m, "the screen has more files than a unit holds");
    }
    if (filo_compile(&s->ctx, src, len, &u->progs[u->n]) != FILO_OK) {
        return fail_filo(m, path);
    }
    filo_limits limits = {SCR_STEPS_INIT, 0};
    filo_value v = {0};
    if (run && filo_run(&s->ctx, &u->progs[u->n], &limits, &v) != FILO_OK) {
        return fail_filo(m, path);
    }
    const char *base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;
    size_t n = strlen(base) - (sizeof(".filo") - 1); /* every file here ends so */
    if (n >= SCR_ENTRY_MAX) {
        return fail_here(m, "a file's name is too long for an entry");
    }
    memcpy(u->names[u->n], base, n);
    u->names[u->n][n] = '\0';
    u->entries[u->n].name = u->names[u->n];
    u->entries[u->n].prog = &u->progs[u->n];
    u->n++;
    return true;
}

static bool is_hook_file(const char *path, const char *name) {
    size_t n = strlen(name);
    if (strncmp(path, name, n) != 0 || path[n] != '/') {
        return false;
    }
    const char *file = path + n + 1;
    size_t len = strlen(file);
    if (strchr(file, '/') != NULL || len <= 5 || strcmp(file + len - 5, ".filo") != 0) {
        return false;
    }
    if (strcmp(file, "common.filo") == 0) {
        return false;
    }
    return strcmp(file, "init.filo") != 0;
}

bool screen_build(roc *m, const char *name, uint8_t *dst, size_t cap, size_t *len) {
    unit_src u;
    screen_state *s = &m->scr;
    screen_reset(m);
    u.n = 0;
    char path[TREE_PATH_MAX];
    if (!unit_add(m, &u, "common.filo", false, true)) {
        return false;
    }
    if (!path_join(path, sizeof(path), name, "init.filo") || !unit_add(m, &u, path, true, true)) {
        return false;
    }
    filo_seal_globals(&s->ctx);
    size_t nfiles = 0;
    const tree_file *files = tree_all(&nfiles);
    for (size_t i = 0; i < nfiles; i++) {
        if (is_hook_file(files[i].path, name) && !unit_add(m, &u, files[i].path, true, false)) {
            return false;
        }
    }
    if (filo_bc_build(&s->ctx, u.entries, u.n, dst, cap, len) != FILO_OK) {
        return fail_filo(m, name);
    }
    return true;
}

void screen_attach(roc *m) {
    roc_app_enter(m, &screen_app);
}

/* Not every screen of this shell is written in Filo: the doors and the live
   view are C apps, and a script reaches them by the same name it would use
   for a directory. Whoever wrote the screen does not have to know which is
   which. */
typedef struct {
    const char *name;
    void (*enter)(roc *m, const char *arg);
} scr_target;

static void t_read(roc *m, const char *arg) {
    roc_reader_begin_less(m, arg);
}

static void t_logoff(roc *m, const char *arg) {
    (void)arg;
    m->exited = true;
    roc_app_leave(m, NULL);
    term_puts(&m->t, "\r\nNO CARRIER\r\n");
}

static void t_shell(roc *m, const char *arg) {
    (void)arg;
    /* the whole note is 47 columns: a narrow screen gets the short one */
    roc_app_leave(m, m->t.cols >= 47
                         ? "a real shell \xe2\x80\x94 'exit' goes back to the screens\r\n"
                         : "exit: back to the screens\r\n");
}

/* back: to whatever was under the screen, saying nothing — an editor
   opened from the shell returns to its prompt */
static void t_back(roc *m, const char *arg) {
    (void)arg;
    roc_app_leave(m, NULL);
}

#if ROC_APP_EDIT
/* preview: the editor's text as the pager shows it, markdown rendered when
   the name says so; the editor repaints when the pager closes */
static void t_preview(roc *m, const char *arg) {
    (void)arg;
    roc_show_bytes(m, true, m->ed.path, m->ed.tb.text, m->ed.tb.len);
}
#endif

#if ROC_APP_COREWAR
/* fight: the arena. With no argument the two the Core War screen picked,
   or a pair of paths, which is what the shell's own command passes. */
static void t_fight(roc *m, const char *arg) {
    if (arg[0] == '\0') {
        (void)corewar_fight(m);
        return;
    }
    roc_cmd_corewar(m, arg);
}
#endif

/* man: one of the shell's own pages, over whatever is on the screen —
   the reader asks the host for a file, and these are already here. */
static void t_man(roc *m, const char *arg) {
    const uint8_t *data = NULL;
    size_t len = 0;
    if (!roc_find_file(m, arg, &data, &len)) {
        roc_flash(m, "that manual is not here");
        return;
    }
    roc_show_bytes(m, true, arg, data, len);
}

#if ROC_APP_EDIT
/* edt: the editor over a file, coming back to the screen that asked. */
static void t_edt(roc *m, const char *arg) {
    (void)edit_open_from(m, arg, m->scr.name);
}
#endif

static const scr_target targets[] = {
#if ROC_APP_COREWAR
    {"fight", t_fight},
#endif
#if ROC_APP_EDIT
    {"preview", t_preview}, {"edt", t_edt},
#endif
    {"read", t_read},       {"shell", t_shell}, {"logoff", t_logoff},
    {"back", t_back},       {"man", t_man},
};

typedef enum {
    SETTLE_NONE,   /* nobody asked to go anywhere */
    SETTLE_SCREEN, /* another screen loaded, and painted itself */
    SETTLE_APP,    /* a C app is on top now: painting under it would show */
    SETTLE_FAIL,
} settle_result;

/* exec cannot swap contexts while its own program is running, so the
   move happens here, once the run is over. */

static settle_result settle(roc *m) {
    screen_state *s = &m->scr;
    if (s->pending[0] == '\0') {
        return SETTLE_NONE;
    }
    char next[VFS_PATH_MAX];
    strcpy(next, s->pending);
    s->pending[0] = '\0';
    char fx[SCR_FX_MAX][SCR_FX_NAME];
    memcpy(fx, s->fx, sizeof(fx));
    size_t nfx = s->nfx;
    s->nfx = 0;

    /* "read:/pub/x.md" names a target and what it should open. A directory
       in the tree can never hold a colon, so there is nothing to confuse. */
    char *arg = strchr(next, ':');
    if (arg != NULL) {
        *arg = '\0';
        arg++;
    }
    if (roc_layer_spec.exec != NULL && roc_layer_spec.exec(m, next, arg != NULL ? arg : "")) {
        return SETTLE_APP; /* the layer's own: it paints itself */
    }
    size_t i = 0;
    while (i < sizeof(targets) / sizeof(targets[0])) {
        if (strcmp(targets[i].name, next) == 0) {
            targets[i].enter(m, arg != NULL ? arg : "");
            return SETTLE_APP; /* a door paints itself: no effect to play over */
        }
        i++;
    }
    if (nfx == 0) {
        if (!screen_load_with(m, next, arg != NULL ? arg : "")) {
            return SETTLE_FAIL;
        }
        /* a screen whose init sends the visitor on (a door that is not for
           them) is followed at once, a few hops at most */
        static int hops;
        if (s->pending[0] != '\0' && hops < 4) {
            hops++;
            settle_result r = settle(m);
            hops--;
            return r == SETTLE_NONE ? SETTLE_SCREEN : r;
        }
        return SETTLE_SCREEN;
    }
    /* with effects the new screen is built but not sent: the show goes from
       what the terminal still holds to what the canvas now holds */
    s->quiet_load = true;
    bool ok = screen_load_with(m, next, arg != NULL ? arg : "");
    s->quiet_load = false;
    if (!ok) {
        return SETTLE_FAIL;
    }
    const char *names[SCR_FX_MAX];
    for (size_t k = 0; k < nfx; k++) {
        names[k] = fx[k];
    }
    roc_layer_spec.transition(m, names, nfx); /* fx-next exists only with it */
    return SETTLE_APP;
}

static bool run_hook(roc *m, scr_hook_id id) {
    screen_state *s = &m->scr;
    const scr_hook *h = &s->hooks[id];
    if (!h->loaded) {
        return true;
    }
    uint32_t steps = id == SCR_HOOK_TICK ? SCR_STEPS_TICK : SCR_STEPS_EVENT;
    if (hook_exec(m, h, steps) != FILO_OK) {
        return fail_hook(m, h);
    }
    return true;
}

bool screen_key(roc *m, uint32_t cp) {
    screen_state *s = &m->scr;
    if (field_takes(&s->in, cp)) {
        if (cp == '\r' || cp == '\n') {
            s->in_seeded = false;
            if (!run_hook(m, SCR_HOOK_INPUT)) {
                return false;
            }
            if (!s->in_seeded) {
                field_clear(&s->in);
            }
            s->in_seeded = false;
        } else {
            field_key(&s->in, cp);
        }
    } else {
        s->key_cp = cp;
        (void)filo_set_global(&s->ctx, "KEY", filo_num(cp));
        if (!run_hook(m, SCR_HOOK_KEY)) {
            return false;
        }
    }
    settle_result r = settle(m);
    if (r == SETTLE_FAIL) {
        return false;
    }
    /* mid-paste the terminal waits for the closing mark, but the draw still
       runs: it is what places a field the next pasted key types into
       (":q!" pasted whole opens the colon line at ':') */
    bool painted = true;
    if (r == SETTLE_NONE && m->t.pasting) {
        painted = compose(m);
    } else if (r == SETTLE_NONE) {
        painted = screen_paint(m);
    }
    if (!painted) {
        return false;
    }
    return true;
}

/* ---- the app ---- */

static void screen_on_key(void *ctx, uint32_t cp);
static void screen_on_resize(void *ctx);

static void screen_on_tick(void *ctx, uint32_t ms);

static const term_app screen_app = {
    .on_key = screen_on_key,
    .on_resize = screen_on_resize,
    .on_tick = screen_on_tick,
};

/* The status bar carries the time connected, so once a second the screen is
   drawn again. The diff keeps that to the cells that changed — a handful —
   and the draw hook of a screen is cheap; only the wire cost was measured
   worth caring about, and the flush already minimises it. */
static void screen_on_tick(void *ctx, uint32_t ms) {
    roc *m = ctx;
    screen_state *s = &m->scr;
    if (!s->hooks[SCR_HOOK_DRAW].loaded) {
        return;
    }
    /* A screen that animates is told how much time went by and paints on
       its own cadence; one that does not still gets a second's worth so
       the clock in the bar moves. */
    if (s->hooks[SCR_HOOK_TICK].loaded) {
        (void)filo_set_global(&s->ctx, "MS", filo_num(ms));
        if (!run_hook(m, SCR_HOOK_TICK)) {
            return;
        }
        if (settle(m) != SETTLE_NONE) {
            return; /* the hook asked to go somewhere */
        }
        (void)screen_paint(m);
        return;
    }
    s->tick_ms += ms;
    if (s->tick_ms < 1000) {
        return;
    }
    s->tick_ms %= 1000;
    (void)screen_paint(m);
}

/* Every key is the program's, ESC too: stepping back through the shell is
   its framework's rule (back-on-esc in common.filo), not the shell's. */
/* A key whose run, or the screen it went to, failed: the screen is half
   there and would take keys without an answer. Back to the front screen,
   the error its note; the terminal gets it when even that one fails. */
static void screen_on_key(void *ctx, uint32_t cp) {
    roc *m = ctx;
    if (screen_key(m, cp)) {
        return;
    }
    size_t n = 0;
    while (m->scr.error[n] != '\0' && n + 1 < sizeof(m->t.note)) {
        m->t.note[n] = m->scr.error[n];
        n++;
    }
    m->t.note[n] = '\0';
    const char *home = roc_layer_spec.home;
    if (home == NULL) {
        roc_app_leave(m, m->scr.error); /* no front screen to fall back to: the prompt */
        return;
    }
    if (strcmp(m->scr.name, home) != 0 && screen_load(m, home)) {
        return;
    }
    m->t.note[0] = '\0';
    term_puts(&m->t, "\r\n");
    term_puts(&m->t, m->scr.error);
    term_puts(&m->t, "\r\n");
}

static void screen_on_resize(void *ctx) {
    roc *m = ctx;
    /* the shell calls this when an app above closes too, and that app
       painted over us, so nothing on the terminal can be trusted —
       the cursor least of all, since a door hides it to draw */
    m->cmp.shown.rows = 0;
    m->scr.cursor_shown = SCR_CURSOR_UNKNOWN;
    m->scr.fresh = true; /* the app may have drawn in our cells as well */
    (void)screen_paint(m);
}

bool screen_is_top(const roc *m) {
    return term_app_top(&m->t) == &screen_app;
}

void screen_enter(roc *m, const char *name) {
    m->scr.name[0] = '\0'; /* from outside the shell: nothing behind it */
    m->scr.ntrail = 0;
    roc_app_enter(m, &screen_app);
    (void)screen_load(m, name);
    (void)settle(m); /* an init may send the visitor elsewhere */
}

const char *screen_error(const roc *m) {
    return m->scr.error;
}
