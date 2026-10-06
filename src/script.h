#ifndef ROC_SCRIPT_H
#define ROC_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "roc_config.h"

#include "filo.h"
#include "regex.h"
#include "vfs.h"

/* The shell's Filo: a command the dispatcher does not know is looked up as
   bin/<name>.filo in the shell's tree and run with ARGS. A script runs to the
   end without blocking, like a screen hook; a builtin that would wait on the
   host — less, cat — schedules the action, taken once the script is done. */

enum {
    SC_MEM_PERSISTENT = ROC_CFG_SC_MEM_PERSISTENT,
    /* a command runs once, on demand, and nothing is freed while it runs:
       walking the whole site (hundreds of entries, every name and prefix a
       new string) needs room, and a runaway loop still ends */
    SC_MEM_RUN = ROC_CFG_SC_MEM_RUN,
    SC_STEPS = 2000000,
    /* and this many more for each byte of input: a utility's work grows
       with what it reads, a runaway loop's does not */
    SC_STEPS_PER_BYTE = 256,
    SC_STDIN_SPARE = 64 * 1024,      /* what STDIN leaves of the lasting memory */
    SC_STOP_EVERY = 4096,            /* steps between two looks at the host's Ctrl-C */
    SC_SRC_MAX = ROC_CFG_SC_SRC_MAX, /* a file fetched from the site: script, copy, edit */
    SC_ARGS_MAX = 256,
    SC_REPL_MAX = ROC_CFG_SC_REPL,
    SC_REGEX_MAX = ROC_CFG_SC_REGEX,
    /* a called run: regex slots, and the least memory worth starting one in
       (half of what its caller has left goes to it) */
    SC_NESTED_REGEX = 2,
    SC_NESTED_MIN = 96 * 1024,
    SC_FILES_MAX = ROC_CFG_SC_FILES,
    SC_FILE_BUF = ROC_CFG_SC_FILE_BUF,
    SC_NESTED_FILES = 4,
};

/* A file a script opened (file-open). */
typedef enum {
    SC_FILE_FREE,
    SC_FILE_MEM,       /* read from memory: the home, the shell's tree */
    SC_FILE_HOST_READ, /* read from the host's storage, a piece at a time */
    SC_FILE_HOST_WRITE,
    SC_FILE_MEM_WRITE, /* written into memory beside its name (tmp), renamed at the close */
} script_file_kind;

typedef struct {
    script_file_kind kind;
    void *host;
    const uint8_t *mem;
    uint64_t size;
    uint64_t pos;     /* the next byte a read takes, buffer included */
    uint64_t written; /* a write's bytes so far */
    char path[VFS_PATH_MAX];
    char tmp[VFS_PATH_MAX];
    size_t buf_at; /* host reads: what is read ahead, buf[buf_at..buf_len) */
    size_t buf_len;
    uint8_t buf[SC_FILE_BUF];
} script_file;

/* What to do with a site file once it is all here. */
typedef enum {
    SC_LOAD_RUN,     /* filo <file>: run it */
    SC_LOAD_PROGRAM, /* filo <file.fbb>: run the command it is */
    SC_LOAD_CP,      /* cp: write it under src_args (the destination) */
    SC_LOAD_EDIT,    /* edit: open it */
} script_load;

typedef enum {
    SC_ACT_NONE,
    SC_ACT_LESS,
    SC_ACT_CAT,
    SC_ACT_UPLOAD,
    SC_ACT_EDIT,
    SC_ACT_HOME_RELOAD, /* the kept home in place of this one, asked for after */
} script_action;

/* What one run of a script has of its own. A script may run another (run,
   a $(...) whose command is a script), each in a context of its own: the
   builtins find their run through the context, never through the session. */
typedef struct script_exec {
    struct roc *m;
    filo_ctx *ctx;
    struct script_exec *outer; /* the run that called this one; NULL at the top */
    /* (exit-status n): the command's exit status, set by the script; without
       it the command's is what it was (0, or 1 after a refusal or an error) */
    bool status_set;
    int status;
    size_t in_pos; /* what in-read and in-line took of the command's input so far */
    /* re-compile's: a handle names a slot and the run that made it */
    uint32_t run_id;
    uint32_t re_used;
    uint32_t nre; /* slots at re */
    sh_regex *re;
    uint32_t nfiles; /* slots at files */
    script_file *files;
} script_exec;

typedef struct {
    filo_ctx ctx;
    uint8_t persistent[SC_MEM_PERSISTENT];
    uint8_t run[SC_MEM_RUN];
    bool ready;
    bool broken;      /* a builtin failed to register: nothing runs in this context */
    script_exec top;  /* the session's own run, in ctx */
    script_exec *cur; /* the innermost run going on; NULL between commands */
    uint32_t runs;    /* runs so far: each one's id */
    sh_regex re[SC_REGEX_MAX];
    script_file files[SC_FILES_MAX];
    script_action action;
    char path[VFS_PATH_MAX];
    /* a script on its way from the site: bytes so far, and what to run it with */
    uint8_t src[SC_SRC_MAX];
    size_t src_len;
    bool src_overflow;
    char src_name[VFS_PATH_MAX];
    char src_args[SC_ARGS_MAX];
    script_load load;
    /* the REPL: filo with no file; a line is read until its parens close */
    bool repl;
    uint8_t repl_buf[SC_REPL_MAX];
    size_t repl_len;
} script_state;

typedef struct roc roc;

/* Runs the command name with the argument line split into ARGS. /bin holds
   programs: /bin/NAME.fbb, whose member NAME is a command (the entry
   "main", run here) or an app (the entry "draw", opened on the file in
   args). A /bin/NAME.filo of the user's own comes first, run from source,
   as does one when there is no program. False when there is neither;
   errors are printed and count as run. */
bool script_run(roc *m, const char *name, const char *args);
/* ./name, /bin/name: the file runs when it is a program of ours (by its
   first bytes), and is refused otherwise. */
void script_exec_path(roc *m, const char *path, const char *args);

/* The context a command runs in, fresh: the core, the packs and the shell's
   builtins, as a program of /bin sees them. */
filo_ctx *script_context(roc *m);

/* Compiles a script to the unit a command's program holds, with the entry
   "main", into dst (the size to *len). False with the error in the script
   context. */
bool script_build(roc *m, const uint8_t *src, size_t len, uint8_t *dst, size_t cap,
                  size_t *out_len);

/* Runs the Filo file at a VFS path (relative to cwd; ".filo" added when
   missing) with args: from memory when it is one of the shell's own, else
   fetched from the site and run when it arrives. Errors are printed. */
void script_run_path(roc *m, const char *path, const char *args);

/* Asks the host for a site file and, once it is all here, does what kind
   says: runs it with args, copies it to args, or opens it in the editor.
   False when the request could not start (said). */
bool script_load_begin(roc *m, script_load kind, const char *path, const char *args);

/* The fetched file, collected by roc_feed. */
void script_collect(roc *m, const uint8_t *data, size_t n);
void script_collected(roc *m);

/* The REPL: filo with no file. Lines go through script_repl_line until
   one says exit (or Ctrl-D does); the prompt asks what to show. */
void script_repl_begin(roc *m);
void script_repl_line(roc *m, const char *line);
const char *script_repl_prompt(const roc *m);

#endif
