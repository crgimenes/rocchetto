#include "edit.h"

#include <string.h>

#include "roc.h"
#include "tbx.h"

static roc *roc_of(filo_ctx *ctx) {
    return ctx->host.user;
}

static tbuf *buf(filo_ctx *ctx) {
    return &roc_of(ctx)->ed.tb;
}

static int b_tb_path(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "tb-path takes no argument");
    }
    *out = filo_cstring(roc_of(ctx)->ed.path);
    return FILO_OK;
}

/* (tb-save) or (tb-save "path"): "" when saved, else why not — the words
   of the shell, for the status line. Saving under a new name keeps it. */
static int b_tb_save(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    roc *m = roc_of(ctx);
    char path[VFS_PATH_MAX];
    if (n > 1) {
        return filo_fail(ctx, "tb-save expects at most a path");
    }
    if (n == 1) {
        if (a[0].kind != FILO_STRING || a[0].u.str.len >= sizeof(path)) {
            return filo_fail(ctx, "tb-save expects a path");
        }
        char typed[VFS_PATH_MAX];
        memcpy(typed, a[0].u.str.ptr, a[0].u.str.len);
        typed[a[0].u.str.len] = '\0';
        if (typed[0] == '\0' || !roc_resolve_arg(m, typed, path, sizeof(path))) {
            *out = filo_cstring("File name too long");
            return FILO_OK;
        }
    } else {
        memcpy(path, m->ed.path, strlen(m->ed.path) + 1);
    }
    if (path[0] == '\0') {
        *out = filo_cstring("No such file or directory"); /* a new file needs a name */
        return FILO_OK;
    }
    tbuf *t = &m->ed.tb;
    const char *why = roc_write_check(m, path, t->text, t->len);
    if (why != NULL) {
        *out = filo_cstring(why);
        return FILO_OK;
    }
    memcpy(m->ed.path, path, strlen(path) + 1);
    t->dirty = false;
    *out = filo_cstring("");
    return FILO_OK;
}

/* (clipboard text) hands text to the terminal's clipboard (term_clipboard
   says how). Nothing else changes on screen. */
static int b_clipboard(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1 || a[0].kind != FILO_STRING) {
        return filo_fail(ctx, "clipboard expects a string");
    }
    size_t len = a[0].u.str.len;
    term_clipboard(&roc_of(ctx)->t, a[0].u.str.ptr, len > TB_CLIP ? TB_CLIP : len);
    *out = filo_bool(true);
    return FILO_OK;
}

/* The text is what the file holds: an edit undone by hand leaves nothing
   to save. A file not there holds nothing. */
static bool saved(filo_ctx *ctx) {
    const roc *m = roc_of(ctx);
    const tbuf *t = &m->ed.tb;
    const uint8_t *data = NULL;
    size_t len = 0;
    if (m->ed.path[0] == '\0' || !roc_find_file(m, m->ed.path, &data, &len)) {
        return t->len == 0;
    }
    if (len != t->len) {
        return false;
    }
    if (len == 0) {
        return true;
    }
    return memcmp(data, t->text, len) == 0;
}

void edit_register(filo_ctx *ctx) {
    tbx_register(ctx, buf, saved);
    (void)filo_register_builtin(ctx, "clipboard", b_clipboard);
    (void)filo_register_builtin(ctx, "tb-path", b_tb_path);
    (void)filo_register_builtin(ctx, "tb-save", b_tb_save);
}

/* A screen is already on top when the shell sent us here: the editor
   takes its place instead of stacking a second screen over the same
   state, and RET is how it finds its way back. */
static void edit_show(roc *m) {
    if (screen_is_top(m)) {
        (void)screen_load(m, m->ed.app);
        return;
    }
    screen_enter(m, m->ed.app);
}

bool edit_open(roc *m, const char *arg) {
    return edit_open_from(m, arg, "");
}

bool edit_open_from(roc *m, const char *arg, const char *ret) {
    return edit_open_app(m, "edt", "/bin/edt", arg, ret);
}

bool edit_open_app(roc *m, const char *app, const char *file, const char *arg, const char *ret) {
    if (strlen(app) + 1 > sizeof(m->ed.app) || strlen(file) + 1 > sizeof(m->ed.app_file)) {
        roc_err(m, app, arg, "File name too long");
        return false;
    }
    strcpy(m->ed.app, app);
    strcpy(m->ed.app_file, file);
    m->ed.ret[0] = '\0';
    if (strlen(ret) + 1 <= sizeof(m->ed.ret)) {
        strcpy(m->ed.ret, ret);
    }
    if (!m->indexed) {
        term_puts(&m->t, "rocchetto: no index\r\n");
        return false;
    }
    if (arg[0] == '\0') {
        /* a text with no name yet: the first save asks for one */
        tb_init(&m->ed.tb);
        m->ed.path[0] = '\0';
        edit_show(m);
        return true;
    }
    char path[VFS_PATH_MAX];
    if (!roc_resolve_arg(m, arg, path, sizeof(path))) {
        return false;
    }
    tbuf *t = &m->ed.tb;
    const vfs_node *node = roc_lookup(m, path);
    if (node == NULL) {
        /* a new file: it must be somewhere a save can go */
        const char *why = roc_write_check(m, path, (const uint8_t *)"", 0);
        if (why != NULL) {
            roc_err(m, m->ed.app, arg, why);
            return false;
        }
        (void)ufs_remove(&m->uf, path); /* the probe: not a file until saved */
        (void)vfs_remove(&m->fs, path);
        tb_init(t);
    } else if (node->dir) {
        roc_err(m, m->ed.app, arg, "Is a directory");
        return false;
    } else {
        const uint8_t *data = NULL;
        size_t len = 0;
        if (!roc_find_file(m, path, &data, &len)) {
            /* the site's: fetched, then opened by edit_open_bytes */
            return script_load_begin(m, SC_LOAD_EDIT, path, "");
        }
        if (!tb_load(t, data, len)) {
            roc_err(m, m->ed.app, arg, "File too large");
            return false;
        }
    }
    memcpy(m->ed.path, path, strlen(path) + 1);
    edit_show(m);
    return true;
}

void edit_open_bytes(roc *m, const char *path, const uint8_t *data, size_t len) {
    if (!tb_load(&m->ed.tb, data, len)) {
        roc_err(m, m->ed.app, path, "File too large");
        return;
    }
    memcpy(m->ed.path, path, strlen(path) + 1);
    edit_show(m);
}
