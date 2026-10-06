/* mkunits: compiles every Filo screen of the tree into the unit the shell
   runs in its place, and puts them in one bundle, OUTDIR/screens.fbb, each
   named by its screen; and every command of bin/ into its own,
   OUTDIR/bin/NAME (no extension: a program is known by its first bytes),
   whose one member NAME has the entry "main" — what
   the shell's /bin holds, a program a person can run, open or download. It is the shell itself
   doing the compiling — the same builtins, the same globals — linked against the tree of sources
   alone; the tree the shell carries is those sources plus the bundle. A
   screen that does not compile fails the build here instead of on someone's
   terminal.

   --strip leaves out the debug sections (where each instruction came from
   in the source), for a board that has no use for them: a Cardputer shows
   no positions and pays for them in flash. --strip-screens leaves them out
   of the screens alone: the web's visitors download the bundle and never
   see a screen's positions, while a command of bin/ is theirs to open.

   Beside them, the profiles of the two VMs they run in, the names each
   gives a program (its builtins, then the globals the shell sets):
   OUTDIR/bin.vm for a command, OUTDIR/screens.vm for a screen or an app.
   filo check -vm compares a unit's imports with one.

   usage: mkunits [--strip | --strip-screens] OUTDIR */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../src/roc.h"

enum {
    UNITS_BYTES = 1U << 20U,
    SCREENS_MAX = 64,
};

static roc M; /* too large for the stack */
static uint8_t units[UNITS_BYTES];
static uint8_t bundle[UNITS_BYTES];
static uint8_t full[UNITS_BYTES];
static filo_bundle_member members[SCREENS_MAX];
static char names[SCREENS_MAX][TREE_PATH_MAX];

static void no_request(void *ctx, uint32_t req_id, const char *path) {
    (void)ctx;
    (void)req_id;
    (void)path;
}

/* Writes ctx's profile to dir/name: its builtins, then its defined
   globals, one a line, through the bundle buffer. */
static bool write_profile(const char *dir, const char *name, const filo_ctx *ctx, const char *what);

static bool write_bundle(const char *dir, const char *name, size_t len) {
    char path[1024];
    (void)snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    size_t n = fwrite(bundle, 1, len, f);
    if (fclose(f) != 0) {
        return false;
    }
    return n == len;
}

static void say(const char *what, const char *name, const char *why) {
    char line[512];
    (void)snprintf(line, sizeof(line), "mkunits: %s: %s%s\n", name, why, what);
    (void)fputs(line, stderr);
}

static bool write_profile(const char *dir, const char *name, const filo_ctx *ctx,
                          const char *what) {
    size_t len = 0;
    int n = snprintf((char *)bundle, sizeof(bundle),
                     "# what roc gives %s: builtins, then globals (\"global NAME\")\n"
                     "# written by mkunits; filo check -vm %s FILE\n",
                     what, name);
    if (n < 0) {
        return false;
    }
    len = (size_t)n;
    for (uint32_t i = 0; i < ctx->nbuiltins + ctx->nsymbols; i++) {
        const char *s =
            i < ctx->nbuiltins ? ctx->builtins[i].name : ctx->symbols[i - ctx->nbuiltins];
        if (i >= ctx->nbuiltins && !ctx->defined[i - ctx->nbuiltins]) {
            continue;
        }
        n = snprintf((char *)bundle + len, sizeof(bundle) - len, "%s%s\n",
                     i < ctx->nbuiltins ? "" : "global ", s);
        if (n < 0 || (size_t)n >= sizeof(bundle) - len) {
            return false;
        }
        len += (size_t)n;
    }
    return write_bundle(dir, name, len);
}

int main(int argc, char **argv) {
    bool strip = false;
    bool strip_screens = false;
    if (argc == 3 && strcmp(argv[1], "--strip") == 0) {
        strip = true;
        strip_screens = true;
        argc--;
        argv++;
    } else if (argc == 3 && strcmp(argv[1], "--strip-screens") == 0) {
        strip_screens = true;
        argc--;
        argv++;
    }
    if (argc != 2 || argv[1][0] == '-') {
        bool help = false;
        if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
            help = true;
        }
        (void)fputs("usage: mkunits [--strip | --strip-screens] OUTDIR\n"
                    "Compiles each screen of the tree (a directory with init.filo) into a\n"
                    "unit, and writes them as one bundle, OUTDIR/" SCR_BUNDLE "; and each\n"
                    "command of bin/ into its own, OUTDIR/bin/NAME.\n"
                    "  --strip          no debug sections anywhere (a board)\n"
                    "  --strip-screens  none in the screens' bundle (the web)\n",
                    help ? stdout : stderr);
        return help ? 0 : 2;
    }
    roc_host host = {.filo_extend = roc_host_extend, .request = no_request};
    roc_init(&M, &host, 80, 24, ROC_F_NO_SPLASH);
    size_t nfiles = 0;
    const tree_file *files = tree_all(&nfiles);
    int failed = 0;
    uint32_t n = 0;
    size_t used = 0;
    for (size_t i = 0; i < nfiles; i++) {
        const char *slash = strchr(files[i].path, '/');
        if (slash == NULL || strcmp(slash, "/init.filo") != 0) {
            continue;
        }
        if (n >= SCREENS_MAX) {
            say("", "mkunits", "more screens than a bundle here holds");
            return 1;
        }
        size_t nl = (size_t)(slash - files[i].path);
        memcpy(names[n], files[i].path, nl);
        names[n][nl] = '\0';
        size_t len = 0;
        if (!screen_build(&M, names[n], strip_screens ? full : units + used,
                          strip_screens ? sizeof(full) : sizeof(units) - used, &len)) {
            say("", names[n], M.scr.error);
            failed++;
            continue;
        }
        if (strip_screens && filo_bc_strip(&M.scr.ctx, full, len, units + used,
                                           sizeof(units) - used, &len) != FILO_OK) {
            say("", names[n], filo_error(&M.scr.ctx));
            failed++;
            continue;
        }
        members[n].name = names[n];
        members[n].data = units + used;
        members[n].len = len;
        used += len;
        n++;
    }
    if (failed > 0) {
        return 1;
    }
    size_t len = 0;
    /* a tree with no screens (the shell alone) has no bundle of them */
    if (n > 0 &&
        filo_bundle_build(&M.scr.ctx, members, n, bundle, sizeof(bundle), &len) != FILO_OK) {
        say("", "bundle", filo_error(&M.scr.ctx));
        return 1;
    }
    if (n > 0 && !write_bundle(argv[1], SCR_BUNDLE, len)) {
        say(argv[1], "bundle", "cannot write under ");
        return 1;
    }
    char done[96];
    (void)snprintf(done, sizeof(done), "%u screens, a bundle of %zu bytes", n, len);
    say("", "done", done);

    /* the commands: bin/NAME.filo, each a bundle of its own */
    char bin[1024];
    (void)snprintf(bin, sizeof(bin), "%s/bin", argv[1]);
    (void)mkdir(bin, 0755);
    uint32_t commands = 0;
    for (size_t i = 0; i < nfiles; i++) {
        const char *p = files[i].path;
        size_t pl = strlen(p);
        if (strncmp(p, "bin/", 4) != 0 || strchr(p + 4, '/') != NULL || pl <= 9 ||
            strcmp(p + pl - 5, ".filo") != 0) {
            continue;
        }
        char name[TREE_PATH_MAX];
        memcpy(name, p + 4, pl - 9);
        name[pl - 9] = '\0';
        size_t ulen = 0;
        if (!script_build(&M, files[i].data, files[i].len, strip ? full : units,
                          strip ? sizeof(full) : sizeof(units), &ulen)) {
            say("", p, filo_error(&M.sc.ctx));
            failed++;
            continue;
        }
        if (strip && filo_bc_strip(&M.sc.ctx, full, ulen, units, sizeof(units), &ulen) != FILO_OK) {
            say("", p, filo_error(&M.sc.ctx));
            failed++;
            continue;
        }
        filo_bundle_member one = {name, units, ulen};
        char file[TREE_PATH_MAX + 8];
        (void)snprintf(file, sizeof(file), "bin/%s", name);
        if (filo_bundle_build(&M.sc.ctx, &one, 1, bundle, sizeof(bundle), &len) != FILO_OK ||
            !write_bundle(argv[1], file, len)) {
            say(argv[1], p, "cannot write under ");
            failed++;
            continue;
        }
        commands++;
    }
    if (failed > 0) {
        return 1;
    }
    (void)snprintf(done, sizeof(done), "%u commands, one bundle each in bin/", commands);
    say("", "done", done);

    /* the script context as the last command's build left it: reset, and
       nothing defined but what the shell sets */
    screen_reset(&M);
    if (!write_profile(argv[1], "bin.vm", &M.sc.ctx, "a command") ||
        !write_profile(argv[1], "screens.vm", &M.scr.ctx, "a screen or an app")) {
        say(argv[1], "profiles", "cannot write under ");
        return 1;
    }
    return 0;
}
