#ifndef ROC_H
#define ROC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "keyin.h"
#include "keys.h"
#include "roc_config.h"

#include "version.h"

#include "canvas.h"
#include "ed.h"
#include "hl.h"
#include "home.h"
#include "md.h"
#include "pager.h"
#include "script.h"
#include "sh.h"
#include "term.h"
#include "tree.h"
#include "ufs.h"
#include "utf8.h"
#include "vfs.h"

#if ROC_APP_SCREENS
#include "screen.h"
#endif
#if ROC_APP_EDIT
#include "edit.h"
#endif
#if ROC_APP_COREWAR
#include "corewar.h"
#endif

enum {
    ROC_FEED_MAX = 16384, /* hosts must feed at most this per call and drain output between calls */
    ROC_LINE_MAX = 512,
    ROC_SRCH_MAX = 64,               /* what Ctrl-R looks for */
    ROC_HIST_MAX = ROC_CFG_HIST_MAX, /* lines remembered for the session */
    ROC_USER_MAX = 32,               /* name in the prompt, NUL included */
    ROC_RDLINE_MAX = 512,
    ROC_ESC_TIMEOUT_MS = KEYIN_ESC_TIMEOUT_MS,
    ROC_CAP_MAX = ROC_CFG_CAP_MAX, /* what one redirected command may write */
};

/* cmd > file: what the command prints goes to a file of the home instead,
   as text — escapes dropped, lines ending in \n. Refusals still reach
   the terminal, the way stderr does. */
typedef struct {
    bool on;
    bool paused; /* a refusal is being said: to the terminal */
    bool append;
    bool overflow;
    bool pipe;    /* to the next command of a pipeline, not to a file */
    bool discard; /* > /dev/null */
    bool keep;    /* for a $(...): kept, the words of the line that called it */
    bool to_err;  /* >&2: to where the refusals go */
    uint8_t esc;  /* 0 text, 1 after ESC, 2 in CSI, 3 in OSC, 4 ESC inside OSC */
    size_t len;
    char path[VFS_PATH_MAX];
    /* a command's > that outgrew buf: what buf held went to the file
       already (spill), through the host's handle or the store's one
       transfer, and buf takes the next piece */
    bool spilled;
    bool spill_host;
    void *spill_h;
    char spill_tmp[VFS_PATH_MAX];
    /* a | that outgrew buf: what came first is in the host's spool */
    void *spool;
    uint64_t spool_len;
    uint8_t buf[ROC_CAP_MAX];
} roc_capture;

/* 2> file: the refusals (roc_err, a program's error) go to a file of the
   home, kept until the command ends; 2>&1 where the output goes; 2>
   /dev/null nowhere. */
enum { ROC_ERRCAP_MAX = ROC_CFG_ERRCAP };
typedef struct {
    bool on;
    bool to_out;
    bool discard;
    bool append;
    bool overflow;
    bool keep; /* for roc_call: kept here for its caller, written to no file */
    size_t len;
    char path[VFS_PATH_MAX];
    uint8_t buf[ROC_ERRCAP_MAX];
} roc_errcap;

/* fi 2> f, done 2>&1: where the refusals of a whole block go, under each
   command's own 2>; a block inside another keeps its text after the outer
   one's in the same buffer. */
typedef struct {
    size_t start;
    bool to_out;
    bool discard;
    bool append;
    char path[VFS_PATH_MAX];
} roc_block_err;

#define ROC_INDEX_PATH "/.index"
#define ROC_HOME_PATH "/.home"
enum { ROC_STORE_OTHER = 2 };

/* roc_init flags */
#define ROC_F_NO_SPLASH                                                                            \
    0x1U /* boot straight into the layer's home, no boot show (reduced motion) */
/* a terminal app's shell (fosforo): the prompt from the start, quiet (the
   app has its own greeting), no layer's home, and exit ends the session */
#define ROC_F_PROMPT 0x2U

/* fh_open's size of a file that arrives as it is read (the site's, in the
   browser): read until fh_read gives 0. */
#define ROC_FH_SIZE_UNKNOWN UINT64_MAX
/* ROC_HOST_CANCELLED: a wait the person stopped (Ctrl-C); the script ends. */
enum { ROC_HOST_NO = 0, ROC_HOST_YES = 1, ROC_HOST_NOT_MINE = -1, ROC_HOST_CANCELLED = 2 };
enum { ROC_FH_READ = 0, ROC_FH_WRITE = 1, ROC_FH_APPEND = 2 };

/* What a path is, as file-stat and the hosts tell it. */
typedef struct {
    bool dir;
    bool link;      /* a symbolic link, not followed (only with follow) */
    bool has_size;  /* a directory, or a size nobody knows: false */
    bool has_mtime; /* a store that keeps no times: false (not 1970) */
    bool has_id;    /* the store tells files apart (find -L sees a cycle) */
    bool away;      /* its bytes are elsewhere (iCloud): reading it brings them */
    uint64_t size;
    int64_t mtime; /* seconds since 1970, UTC */
    uint64_t dev;  /* with ino, the file itself, whatever names it */
    uint64_t ino;
} roc_stat;

/* One entry of a directory listed live (dir_list). */
typedef void (*roc_dir_each)(void *user, const char *name, bool dir, bool link);

/* Host contract: callbacks are called by the core; the host MUST NOT call
   back into the core from inside them — defer and answer with roc_feed*/
/* roc_stream_data/event later. stream_* and term_resize are optional (NULL
   = the capability does not exist on this host). */
typedef struct {
    void *ctx;
    void (*request)(void *ctx, uint32_t req_id, const char *path);
    /* a byte stream the host connects to, for the layer (roc_layer): its
       bytes and events come back through roc_stream_data and _event */
    void (*stream_open)(void *ctx);
    void (*stream_close)(void *ctx);
    /* pin the terminal grid to cols x rows; (0, 0) unpins (back to fit) */
    void (*term_resize)(void *ctx, uint16_t cols, uint16_t rows);
    /* ask the person for files to put in dest, a directory of theirs; they
       arrive through roc_upload_*. NULL: no picker on this host */
    void (*pick_file)(void *ctx, const char *dest);
    /* The machine's storage, when it has one — a card, a disc, anything
       that outlives the session. With these three set the user's files
       live there instead of in memory, which is what makes a board behave
       like a computer rather than a session: nothing to pack, nothing to
       restore, the file is simply on the device.

       file_get returns bytes that stay valid until the next call to it,
       the way the built-in tree returns a pointer into flash. NULL on all
       three means this host keeps files in memory (the browser does). */
    const uint8_t *(*file_get)(void *ctx, const char *path, size_t *len);
    bool (*file_put)(void *ctx, const char *path, const uint8_t *data, size_t len);
    bool (*file_del)(void *ctx, const char *path);
    /* mv on the storage: a file or a whole directory to a new path, over
       a file there. NULL: mv moves only what the session keeps in memory */
    bool (*file_move)(void *ctx, const char *from, const char *to);
    /* mkdir and rmdir on the storage. NULL: a directory lives in the
       session's index until a file goes in, and an empty one is never
       on the disk */
    bool (*dir_make)(void *ctx, const char *path);
    bool (*dir_del)(void *ctx, const char *path);
    /* the machine's clipboard, for pbcopy and pbpaste (and the Filo
       functions of the same names). clipboard_get returns bytes valid
       until the next call to it, NULL when there is nothing. NULL hooks:
       this host has no clipboard, and the commands say so */
    const uint8_t *(*clipboard_get)(void *ctx, size_t *len);
    bool (*clipboard_put)(void *ctx, const uint8_t *data, size_t len);
    /* The storage as it is now, for file-stat and dir-read. Both answer
       ROC_HOST_YES (st filled; every entry right under dir given to each),
       ROC_HOST_NO (not there; for dir_list, cannot be read: an I/O error,
       not an empty directory) or ROC_HOST_NOT_MINE for a path the host does
       not keep, which the index and the in-memory files answer instead.
       follow: a link is taken to what it names. NULL: always not mine. */
    int (*file_stat)(void *ctx, const char *path, bool follow, roc_stat *st);
    int (*dir_list)(void *ctx, const char *dir, roc_dir_each each, void *user);
    /* Files a piece at a time (file-open), on the host's storage. fh_open
       answers as file_stat does (ROC_HOST_NOT_MINE: the core serves the
       path from memory); mode ROC_FH_READ gives the size, ROC_FH_WRITE is
       the old file until fh_close commits (written beside it, renamed
       over; abandoned, it stays as it was), ROC_FH_APPEND adds at the end.
       fh_read: bytes at off, 0 at the end, -1 on an error. fh_write: all
       of them or false. fh_close(commit false): the write abandoned. NULL:
       always not mine. A host may wait inside fh_open for a file that is
       not here yet (the browser fetching the site's, the core suspended
       meanwhile) and answer ROC_HOST_CANCELLED when the wait is stopped. */
    int (*fh_open)(void *ctx, const char *path, int mode, void **h, uint64_t *size);
    long long (*fh_read)(void *ctx, void *h, uint64_t off, uint8_t *buf, size_t n);
    bool (*fh_write)(void *ctx, void *h, const uint8_t *data, size_t n);
    bool (*fh_close)(void *ctx, void *h, bool commit);
    /* sets when path was last written, seconds since 1970 (UTC), for
       touch: ROC_HOST_YES, NO (could not), or NOT_MINE (the store's own) */
    int (*set_mtime)(void *ctx, const char *path, int64_t mtime);
    /* A spool for a | larger than the shell holds (ROC_CAP_MAX): an
       anonymous file, never among the person's files and gone when freed,
       written to its end, then read back by position. NULL (all four): a
       | holds ROC_CAP_MAX at most. */
    void *(*spool_new)(void *ctx);
    bool (*spool_write)(void *ctx, void *s, const uint8_t *data, size_t n);
    long long (*spool_read)(void *ctx, void *s, uint64_t off, uint8_t *buf, size_t n);
    void (*spool_free)(void *ctx, void *s);
    /* keeps the user's home (a home.h blob) between visits; 0 = kept,
       ROC_STORE_OTHER = another session of the same store (a browser tab)
       kept its home since this one read or kept it, so this one is not;
       anything else = could not. A return, not a callback: the host may
       not call the core from here. NULL: this host keeps nothing, and the
       core never asks for /.home either */
    uint32_t (*store_put)(void *ctx, const uint8_t *data, size_t len);
    /* home keep: this session's home is to win the next store_put */
    void (*store_claim)(void *ctx);
    /* polled now and then while a script runs: true stops it (a Ctrl-C the
       person typed meanwhile). A host may also hand the page its turn here
       (the browser, under Asyncify). NULL: a script runs to its end. */
    bool (*interrupted)(void *ctx);
    /* hands the person a file to keep on their machine (a download in the
       browser, a file in the current directory on posix). NULL: no way out */
    void (*download)(void *ctx, const char *name, const uint8_t *data, size_t len);
    /* who is at the keyboard, for the prompt and the status bar; NULL or
       empty means "guest", which is all the web can say until there are
       accounts */
    const char *user;
    /* what machine this is, for the prompt's "@" and the greeting. NULL or
       empty drops the @ part altogether — which is what a screen twenty
       columns wide needs. */
    const char *host_name;
    /* the origin of the site the tree is (https://example.org), which turns
       its relative links into absolute ones in what the pager shows; NULL:
       the links stay as they are */
    const char *site_base;
    /* the time now, seconds since 1970 in UTC, and the local offset east of
       UTC in minutes, for date. NULL: this host has no clock */
    bool (*clock)(void *ctx, int64_t *secs, int32_t *tz_minutes);
    /* Memory a tool needs only while it runs (filo fmt, decompile, build,
       debug, diff), so a visitor who never runs one never pays for it: at
       least need bytes at a base that never moves, the same every call;
       NULL when there is no more. NULL here: none at all (a small board). */
    void *(*scratch)(void *ctx, size_t need);
    /* A command the shell does not have, offered before "not found" (a
       terminal app's ssh): true = the host runs it on the terminal until
       roc_run_done, the shell waiting as for a sleep. Its output bypasses
       the shell, so it is never offered under a > | or $(...).
       NULL: nothing to offer. */
    bool (*run)(void *ctx, int argc, char *const argv[]);
    /* the names run takes, space-separated, so type, help and Tab know
       them; NULL or "": none to tell */
    const char *commands;
    /* This host's own builtins and globals, past roc's, into every Filo
       context the shell makes — scripts, screens, apps — so a program can
       use what only this host offers (a board's visitor count, its chat)
       and one that does refuses to load where it is not offered. False
       when one did not register. NULL: nothing past roc's. */
    bool (*filo_extend)(void *ctx, filo_ctx *f);
} roc_host;

/* The extension this build links (EXTEND_SRC in the Makefile), for a host
   to put in filo_extend: one with nothing to add returns true. */
bool roc_host_extend(void *ctx, filo_ctx *f);

typedef enum {
    ROC_MODE_LINE, /* prompt, line discipline active */
    ROC_MODE_READ, /* waiting for / streaming a requested file */
    ROC_MODE_APP,  /* a full-screen app owns the keys */
} roc_mode;

typedef enum {
    ROC_REQ_NONE,
    ROC_REQ_INDEX,
    ROC_REQ_CAT,
    ROC_REQ_LESS,
    ROC_REQ_SCRIPT,      /* a Filo file from the site: collected, then run */
    ROC_REQ_HOME,        /* the kept home, asked for once after the index */
    ROC_REQ_HOME_RELOAD, /* home reload: the kept home in place of this one */
} roc_req_kind;

typedef struct roc roc;

/* What a layer over the shell brings (a BBS: its front screen, its doors,
   its commands), linked in the file that has roc_host_extend (EXTEND_SRC):
   the shell alone links host/extend_none.c, which brings nothing. Any
   field may be NULL. */
typedef struct {
    /* the screen the session opens on and goes back to: menu, exit and
       Ctrl-D go there instead of ending it. NULL: the prompt is the session */
    const char *home;
    /* the first thing shown, which ends on home (a splash); NULL: home */
    void (*boot)(roc *m);
    /* (exec NAME) for a NAME of the layer's: true when it took the screen */
    bool (*exec)(roc *m, const char *name, const char *arg);
    /* the layer's commands, NULL-ended, and what runs one: true when it
       was one of them */
    const char *const *commands;
    bool (*command)(roc *m, int argc, char *const argv[]);
    /* (fx-next NAME...): whether NAME is an effect, and the effects played
       from what the terminal shows to the screen just loaded unsent */
    bool (*effect)(const char *name);
    void (*transition)(roc *m, const char *const names[], size_t n);
    /* what came on the host's stream (stream_open) */
    void (*stream_data)(roc *m, const uint8_t *data, size_t n);
    void (*stream_event)(roc *m, uint32_t event);
} roc_layer;

extern const roc_layer roc_layer_spec;

typedef enum {
    RD_START,   /* checking for BOM / front matter fence */
    RD_FM_SKIP, /* inside front matter, dropping lines */
    RD_BODY,    /* streaming body */
} roc_rd_state;

/* An if, while, until or for being run: what it is, and for the loops
   where their parts start, how the last round went, and for's name and
   words (in the list's words, from words on). */
typedef struct {
    int rw;
    bool taken;    /* if: a branch ran */
    bool leaving;  /* loop: it is over, done only closes it */
    bool ran_body; /* loop: the body ran once at least */
    int body_status;
    size_t cond; /* while, until: the condition */
    size_t body; /* loop: after do */
    size_t words;
    uint32_t nwords;
    uint32_t word;   /* for: the next one; a function: its $1 (shift moves it) */
    const char *src; /* a function: the text it was called from */
    char var[SH_NAME_MAX];
    size_t tail;       /* after the fi or done and what follows it: > f, the operator */
    int tail_op;       /* that operator, SH_OP_SEMI at the end */
    bool captured;     /* > f or | after it: its output is a level of bcap */
    bool fed;          /* < f or | before it: its input is a level of bin, for read */
    bool errs;         /* 2> f or 2>&1 after it: its refusals are a level of berr */
    bool script;       /* a call of a script file (sh f, . f, ./f.sh), its text in stext */
    bool subshell;     /* sh f, ./f.sh: the variables and the directory come back after */
    size_t text_from;  /* a script: where its part of stext begins */
    bool seen_through; /* eval: its $1, return, shift and break are the caller's */
    bool in_cond;      /* if, elif, while, until: the condition being run (set -e skips it) */
    bool negated;      /* ! before it: $? turned when it ends */
} roc_frame;

/* local name: what it was before, back when the function of frame goes. */
typedef struct {
    char name[SH_NAME_MAX];
    char value[SH_VALUE_MAX];
    bool was_set;
    int frame;
} roc_local;

/* NAME() { ... }: a function of the session, its body at off in ftext. */
typedef struct {
    char name[SH_NAME_MAX];
    size_t off;
    size_t len;
} roc_func;

/* fi > f, done | b: what a whole block writes, under each command's own
   capture, as its text in bcap from start on; a block inside another
   stacks its text after the outer one's, in the same buffer. */
typedef struct {
    size_t start;
    bool pipe;
    bool to_err; /* >&2 */
    bool append;
    bool discard;
    char path[VFS_PATH_MAX];
} roc_block_out;

/* A line of commands being run: its text, where the next command starts,
   the operator before it (SH_OP_*), and whether it is still going; the
   ifs and loops open in it, a jump in the text each. */
typedef struct {
    char text[SH_SCRIPT_MAX + 1];
    const char *src; /* what runs: the text, or a function's */
    roc_frame frames[SH_NEST_MAX];
    int nframes;
    char words[SH_SCRIPT_MAX];
    size_t words_used;
    size_t pos;
    int op;
    bool active;
    bool running;     /* inside roc_list_continue: a prompt there is not the end */
    bool ran;         /* the last command ran: a pipeline is skipped whole */
    bool to_file;     /* its output went to a file: the next of a | reads nothing */
    bool negate_next; /* a ! read: the pipeline or block after it is negated */
    bool negate_due;  /* the negated pipeline ended: its $? turned next */
    bool check_due;   /* set -e: the $? of the command that ended, to look at next */
} roc_list;

typedef struct roc {
    roc_host host;
    term t;
    uint32_t flags;
    bool exited;

    keyin kin; /* the terminal's bytes, decoded into keys */

    uint8_t line[ROC_LINE_MAX];
    size_t line_len;
    size_t line_cur; /* byte offset of the cursor, always on a rune boundary */

    /* Command history, a ring: hist_n entries, the newest written just
       before hist_next. Up/Down search it for lines beginning with what
       was typed left of the cursor, the zsh way: with a prefix the cursor
       stays after it, with none the line comes whole.
       hist_pos is the entry on show while navigating (hist_n when not);
       the line as it was before the first Up is kept to come back to. */
    uint8_t hist[ROC_HIST_MAX][ROC_LINE_MAX];
    uint16_t hist_len[ROC_HIST_MAX];
    size_t hist_n;
    size_t hist_next;
    size_t hist_pos;
    uint8_t hist_orig[ROC_LINE_MAX];
    size_t hist_orig_len;
    size_t hist_seed_len; /* prefix length: the cursor when the search began */

    /* Ctrl-R, readline's reverse-i-search: what is looked for, the entry it
       was found in (hist_n: none yet), whether the last try failed, and the
       rows the search line takes on screen. The line typed before it is in
       hist_orig, to go back to. */
    bool srch_on;
    bool srch_failed;
    uint8_t srch_q[ROC_SRCH_MAX];
    size_t srch_qlen;
    size_t srch_at;
    size_t srch_rows;

    char cwd[VFS_PATH_MAX];
    char user[ROC_USER_MAX];
    vfs fs;
    ufs uf; /* the user's own files, in this memory only */
    bool indexed;
    uint8_t home_buf[HOME_BLOB_CAP]; /* the blob on its way in, or out */
    size_t home_len;
    bool home_overflow;
    uint32_t stop_polls; /* steps since the host was last asked (interrupted) */
    bool stopped;        /* the host said stop: the script ends with 130 */
    bool home_conflict;  /* another tab kept the home after this one read it */

    roc_mode mode;
    roc_req_kind req_kind;
    uint32_t req_seq;
    uint32_t req_id; /* current outstanding request; 0 = none */

    roc_rd_state rd_state;
    uint8_t rd_line[ROC_RDLINE_MAX]; /* line accumulator for START/FM states */
    size_t rd_line_len;
    uint8_t rd_last;  /* last body byte emitted; avoids doubling the final newline */
    char fm_fence[4]; /* "+++" or "---" while skipping front matter */

    pager pg;
    md md;
    compositor cmp;
    script_state sc;
    ed_state ed_l; /* the line editor: no screen, so no board needed */
#if ROC_APP_SCREENS
    screen_state scr;
#endif
#if ROC_APP_EDIT
    edit_state ed;
#endif
    roc_capture cap;
#if ROC_APP_COREWAR
    arena cw; /* Core War: the arena and who fights in it */
#endif
    sh_vars vars;    /* the shell's variables: NAME=value, $NAME */
    sh_vars aliases; /* alias NAME=value: the text a command's first word stands for */
    roc_list list;
    /* a line that wants more (an if not closed, a quote open): the lines so
       far, the next one asked for with "> " */
    char more[SH_SCRIPT_MAX];
    size_t more_len;
    bool more_on;
    /* what a | hands the command running: its input, read by cat and less
       with no file, the filters, and a program as STDIN */
    const uint8_t *in; /* pipe_buf, or the file of a < */
    size_t in_len;
    uint8_t pipe_buf[ROC_CAP_MAX];
    /* the | input's first in_spool_len bytes, when it outgrew pipe_buf:
       in the host's spool, before what in holds */
    void *in_spool;
    uint64_t in_spool_len;
    /* a | of this pipeline was over ROC_CAP_MAX: what the next command read
       was cut, so the pipeline fails even when its last command does not */
    bool pipe_cut;
    roc_capture bcap;
    roc_func funcs[ROC_CFG_FUNCS];
    int nfuncs;
    int ncalls; /* functions running: none is redefined meanwhile */
    roc_local locals[ROC_CFG_LOCALS];
    int nlocals;
    int getopts_ind; /* the OPTIND getopts left, and where in its word it is */
    int getopts_sub;
    bool opt_e; /* set -e, -u (in sh_nounset) and -x */
    bool opt_x;
    /* set -- at the top: the parameters outside any function or script */
    char top_params[SH_LINE_MAX];
    size_t top_params_len;
    uint32_t top_nparams;
    /* the scripts running (sh f), one after another: what a subshell
       saved, then the text */
    char stext[ROC_CFG_SH_TEXT];
    size_t stext_used;
    char ftext[ROC_CFG_FUNC_TEXT]; /* their bodies, one after another, a NUL after each */
    size_t ftext_used;
    roc_block_out bouts[SH_NEST_MAX];
    int nbouts;
    /* < f or a | into a block or a function: what read reads there, a
       level each, the next line of it at cursor */
    uint8_t bin[ROC_CAP_MAX];
    struct {
        size_t start;
        size_t len;
        size_t cursor;
    } bins[SH_NEST_MAX];
    int nbins;
    roc_block_err berrs[SH_NEST_MAX];
    int nberrs;
    uint8_t berr_buf[4 * ROC_ERRCAP_MAX];
    size_t berr_len;
    bool berr_overflow;
    /* read at the prompt: the names it waits for a line to fill */
    uint32_t sleep_ms; /* sleep: what is left of it, the tick counting down */
    bool hosted;       /* the host is running a command: roc_run_done ends it */
    /* trap 'cmds' EXIT / INT: run when the shell (or the script it is in)
       ends, and after a ^C stops a line */
    int last_bg;   /* $!: the & commands so far, 1 on (they ran in the foreground) */
    int bg_status; /* the $? of the last of them, for wait */
    char trap_exit[SH_LINE_MAX];
    char trap_int[SH_LINE_MAX];
    bool int_pending;    /* a ^C stopped a line: trap_int runs before the prompt */
    size_t scratch_used; /* of the host's scratch, taken by the tool running */
    bool read_wait;
    bool read_raw;
    char read_names[SH_LINE_MAX];
    roc_errcap errcap;
    bool piped;
    int status;     /* how the last command ended, $?: 0 well, 1 refused, 2 misread, 127 unknown */
    uint32_t ticks; /* tick counter; doubles as session entropy for doors */
    uint32_t up_ms; /* time connected, from the host's ticks */
} roc;

void roc_init(roc *m, const roc_host *host, uint16_t cols, uint16_t rows, uint32_t flags);
void roc_input(roc *m, const uint8_t *data, size_t n);
void roc_resize(roc *m, uint16_t cols, uint16_t rows);
void roc_tick(roc *m, uint32_t ms);

/* The list the line typed is (a; b && c || d), run a command at a time:
   one that leaves the shell busy (a file from the site, a screen, the REPL)
   has the rest wait, and roc_prompt, which says it finished, goes on. */
void roc_list_continue(roc *m);
/* Whether a list is waiting for the command that just finished. */
bool roc_list_waiting(const roc *m);

/* Answers to a request() call. Feeds larger than ROC_FEED_MAX are refused. */
void roc_feed(roc *m, uint32_t req_id, const uint8_t *data, size_t n);
void roc_feed_eof(roc *m, uint32_t req_id);
/* What came on the stream the layer asked the host to open, in any
   chunks: handed to the layer, dropped when it has no use for it. */
void roc_stream_data(roc *m, const uint8_t *data, size_t n);
void roc_stream_event(roc *m, uint32_t event);
void roc_feed_fail(roc *m, uint32_t req_id);

bool roc_exited(const roc *m);

/* internal, shared with cmds.c and apps */
void roc_reader_begin_cat(roc *m, const char *path);
void roc_reader_begin_less(roc *m, const char *path);
void roc_app_enter(roc *m, const term_app *app);
void roc_app_leave(roc *m, const char *note);
void roc_reader_abort_quiet(roc *m);
void roc_body_emit(roc *m, const uint8_t *data, size_t n);
void roc_prompt(roc *m);
void roc_exec(roc *m, char *line);
void roc_reader_begin_script(roc *m, const char *path);
void roc_show_bytes(roc *m, bool less, const char *name, const uint8_t *data, size_t n);
/* Whether a command of the shell's table is one here: the shell's own
   (articles) are not in an app's shell. */
bool roc_command_here(const roc *m, const char *name);
void roc_cmd_cd(roc *m, const char *arg);
void roc_cmd_cat(roc *m, const char *arg);
void roc_cmd_less(roc *m, const char *arg);
/* Entry i of the session's history, oldest first; i < m->hist_n. */
const uint8_t *roc_history_at(const roc *m, size_t i, size_t *len);
size_t roc_history_text(const roc *m, uint8_t *buf, size_t cap);
/* A line remembered as if typed (a host bringing back an earlier session's). */
void roc_history_add(roc *m, const uint8_t *line, size_t n);
/* /home/<user>/.history: the session's lines, readable as a file. */
bool roc_home_path(const roc *m, const char *file, char *out, size_t cap);
size_t roc_command_count(void);
const char *roc_command_name(size_t i);
/* "rocchetto: cmd: arg: Message", the shape every refusal has; arg may be "".
   The words are the Unix ones — No such file or directory, Disc quota
   exceeded — because they are true here too, and true is the rule: the
   board may sound like a machine from 1989, it may not lie. */
size_t roc_err_text(char *buf, size_t cap, const char *cmd, const char *arg, const char *msg);
void roc_err(roc *m, const char *cmd, const char *arg, const char *msg);

/* Where a path's facts came from: the user's own (the home: in memory or
   the host's storage), the shell's built-in tree, or the site's index (a
   file not fetched). */
typedef enum { ROC_FROM_HOME, ROC_FROM_BOARD, ROC_FROM_SITE } roc_origin;

/* What path (resolved, absolute) is now: the host's storage first, then the
   files in memory, the built-in tree and the index. False when nothing is
   there. */
bool roc_stat_path(const roc *m, const char *path, bool follow, roc_stat *st, roc_origin *from);

/* vfs_lookup, with the entry first brought to what the host's storage
   says now (the index is made when a session opens; the disk changes under
   it). Hosts without file_stat: vfs_lookup itself. */
const vfs_node *roc_lookup(roc *m, const char *path);

/* A file the shell makes up as it is read (~/.history): in the index, on no
   storage. */
bool roc_made_up(const roc *m, const char *path);

/* The index's entries right under dir brought to the host's listing, for ls,
   globs and Tab. At most ROC_SYNC_SEEN entries are compared for removals:
   no more than the index can hold. */
enum { ROC_SYNC_SEEN = ROC_CFG_VFS_NODES_MAX < 1024 ? ROC_CFG_VFS_NODES_MAX : 1024 };
void roc_sync_dir(roc *m, const char *dir);
/* A program of our VM by its first bytes, 7F 'F' 'B' 'C' and a unit's or a
   bundle's kind (docs/bytecode.md): what exec takes, as an a.out. */
bool roc_is_program(const uint8_t *data, size_t len);
/* What the file at path (absolute) runs as, by its first bytes: 'p' a
   program of ours, 's' a shell script (#!/bin/sh), 0 neither. Read here,
   or from the host's disk; a file iCloud keeps away is brought only when
   bring (to run it, not to list it). The site's are never fetched. */
int roc_exec_kind(roc *m, const char *path, bool bring);
/* name in each directory of PATH, in order: the first file that runs (a
   program or a shell script), its absolute path in out, and its kind; 0
   when none. A directory of the site is passed over (nothing is fetched
   to look). */
int roc_path_find(roc *m, const char *name, char *out, size_t cap);

/* Each entry right under dir (resolved), once: the host's, live, then what
   the index and the memory know that the host did not say. False when the
   host could not read it. */
bool roc_dir_list(const roc *m, const char *dir, roc_dir_each each, void *user);
/* A program's stdout as data: the bytes as they are into a > or a | (no
   escape or \r dropped, unlike what the shell draws), and on the terminal
   a bare \n becomes \r\n. A capture over ROC_CAP_MAX is marked cut. */
void roc_out(roc *m, const uint8_t *data, size_t n);
/* A > is full (a command's, c the capture, or a block's one level): buf
   into its file, open from now to the end; false when it cannot go there
   (a pipe, nested levels, the store busy), and the capture is then cut. */
bool roc_capture_spill(roc *m, roc_capture *c);
/* The spooled | input, let go: the command that read it is done. */
void roc_spool_drop(roc *m);
/* For a command that needs its input whole in memory: true, said, when it
   is spooled (larger than the shell holds). */
bool roc_input_spooled(roc *m, const char *cmd);
/* What roc_call gives back: the command's stdout (valid until the next
   $(...) or call), its stderr, its status. */
typedef struct {
    const uint8_t *out;
    size_t out_len;
    uint8_t err[ROC_ERRCAP_MAX];
    size_t err_len;
    bool err_cut;
    int status;
} roc_called;

/* Runs line as a $(...) would, from inside a command that is running (a
   script's run): in reading it, out and err kept apart for the caller, and
   everything the running command had (what it wrote so far, its 2>, the
   input it has not read, $?, the directory) as it was afterwards. keep
   holds what the running command wrote and the input it has yet to read
   meanwhile: roc_call_keep says how much. False, why said, when it could
   not run (too deep, or waiting on the site or a screen). */
size_t roc_call_keep(const roc *m);
bool roc_call(roc *m, const char *line, size_t len, const uint8_t *in, size_t in_len, uint8_t *keep,
              size_t keep_cap, roc_called *res, char *why, size_t why_cap);

/* stderr as data: where the refusals go now (the command's 2>, 2>&1, its
   block's, or the terminal past any capture). Sets no status. */
void roc_errout(roc *m, const uint8_t *data, size_t n);
/* A path as typed, against cwd, "~" the home; says why when it cannot. */
bool roc_resolve_arg(roc *m, const char *arg, char *dst, size_t cap);
/* A file of the user's own: the shell's tree, then the home. */
bool roc_find_file(const roc *m, const char *path, const uint8_t **data, size_t *len);
/* The unit of a program file: a unit itself (a .fbc, or what filo build
   writes), or of a bundle (a .fbb) the member named name, or the only
   member of one renamed on its way here. False with the error in ctx. */
bool roc_program_unit(filo_ctx *ctx, const uint8_t *data, size_t len, const char *name,
                      const uint8_t **unit, size_t *unit_len);
/* Prompt and line drawn again where the cursor is, after something else
   was printed under them. */
void roc_line_repaint(roc *m);

/* Who is at the keyboard from now on — a login, a logout: the prompt and
   USER follow, in this session and in every program it starts. The files
   stay where they are. */
void roc_set_user(roc *m, const char *name);

/* A line the host has for the person, unasked (a message from someone
   else, a connection that dropped): over the app on top, or above the
   line being typed, which comes back whole under it. */
void roc_notice(roc *m, const char *text);
/* A one-line note on the app on top; the shell just prints it. */
void roc_flash(roc *m, const char *note);
void roc_cmd_upload(roc *m, const char *arg);
bool roc_cmd_rm(roc *m, const char *arg);
bool roc_cmd_mkdir(roc *m, const char *arg);
/* pbcopy: what a | hands over (or arg, the text itself, when given) goes to
   the host's clipboard; pbpaste: the clipboard is printed. */
bool roc_cmd_pbcopy(roc *m, const uint8_t *data, size_t len);
bool roc_cmd_pbpaste(roc *m);
bool roc_cmd_rmdir(roc *m, const char *arg);
bool roc_cmd_cp(roc *m, const char *src, const char *dst);
bool roc_cmd_mv(roc *m, const char *src, const char *dst);
bool roc_cmd_download(roc *m, const char *arg);
/* The home as a blob out to the person, and back in from a file of theirs. */
bool roc_home_export(roc *m);
bool roc_home_import(roc *m, const char *arg);
/* What the blob would hold now, in bytes; -1 when this host keeps nothing. */
long roc_home_kept(roc *m);
/* Another tab kept its home after this one read it: this one's changes
   wait for home keep (this one wins) or home reload (that one comes). */
bool roc_home_conflict(const roc *m);
bool roc_home_keep(roc *m);
/* The kept home asked for, to replace this one when it comes. */
bool roc_home_reload(roc *m);
/* A file of the user's, written whole; the one way bytes get into the
   home besides upload. Refusals are printed as cmd's. */
bool roc_write_file(roc *m, const char *cmd, const char *path, const uint8_t *data, size_t len);
/* The same, saying nothing: NULL when written, else the words for why. */
/* Why path cannot be written (its directory missing, outside the home and
   /bin, a directory itself), or NULL when it can. */
const char *roc_write_allowed(roc *m, const char *path);
const char *roc_write_check(roc *m, const char *path, const uint8_t *data, size_t len);
/* Everything that changes the home ends here; persistence hangs on it. */
void roc_home_changed(roc *m);
/* The store's file at path written now, by the host's clock (none: no time). */
void roc_ufs_stamp(roc *m, const char *path);
/* cmd > file: starts (the file is made or emptied first, as sh does) and
   ends (writes) the capture of a command's output. */
bool roc_capture_begin(roc *m, const char *arg, bool append);
/* The capture for a | : what the command writes becomes the input of the
   next, in m->in. */
void roc_pipe_begin(roc *m);
/* Whether what the terminal gets now goes to a file or a pipe instead: a
   command's > or |, or its block's (a screen is never captured). */
bool roc_output_captured(const roc *m);
/* What roc_out writes goes straight to the terminal: no |, > or $(...). */
bool roc_out_terminal(const roc *m);
/* The line a read at the prompt waited for (eof: Ctrl-D, none): into its
   names, $? 0, or 1 at the end. */
void roc_read_answer(roc *m, const char *line, bool eof);
/* The command host.run took has ended, with its exit status. */
void roc_run_done(roc *m, int status);
/* Whether name is one of host.commands. */
bool roc_host_command(const roc *m, const char *name, size_t len);
/* The end of a command's 2>: its file written. */
void roc_err_end(roc *m);

/* The scratch belongs to the tool running: it starts over with
   roc_scratch_reset, then takes pieces in order (16-byte aligned, NULL when
   the host has no more); a piece used for a moment inside goes back with
   roc_scratch_release to the mark taken before it. */
void roc_scratch_reset(roc *m);
void *roc_scratch_take(roc *m, size_t n);
size_t roc_scratch_mark(const roc *m);
void roc_scratch_release(roc *m, size_t mark);
void roc_capture_end(roc *m);

/* A file coming in from the host: name as the person's system calls it,
   dest a directory of theirs or "" for where they are. Bytes follow in
   any number of calls, then end (or abort). A refusal is printed, and the
   host sends nothing more. */
bool roc_upload_begin(roc *m, const char *name, const char *dest, size_t size);
void roc_upload_data(roc *m, const uint8_t *data, size_t n);
void roc_upload_end(roc *m);
void roc_upload_abort(roc *m);

#endif
