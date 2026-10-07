#ifndef ROC_CONFIG_H
#define ROC_CONFIG_H

/* Every limit that decides how much memory a build takes, in one file, so
   that reading it tells you what a target costs. Each one is the default for
   the web and the desktop; a build overrides what it needs with -D and
   changes nothing else:

     make small      the shell-sized configuration, and its footprint

   Constants that decide behaviour rather than footprint stay in the header
   they belong to. The line between them is whether the number appears in an
   array bound.

   Sizes are what one of them costs, measured at the defaults. */

/* ---- what the build contains ----

   An app that is not chosen is not compiled and not linked: its command
   disappears from the shell, its target disappears from the screens, and
   its state leaves the session. A screen that still names a dropped target
   fails the way it would for a name that never existed, so a board that
   drops the doors changes its games screen too.

   ROC_APP_SCREENS is the machinery: Filo programs that paint, the apps of
   /bin (edt, Core War's picker) among them.

   A layer over the shell (a BBS) is not a switch here: it is a file, the
   one with roc_layer_spec (src/roc.h) — its front screen, its doors, its
   commands, its effects — named in the Makefile's EXTEND_SRC and
   LAYER_SRC. The shell alone links host/extend_none.c, which has none.

   The Makefile keeps these and the source list in step: set them there
   (APPS_COREWAR=0 and so on), not with -D alone, or the files would still
   be linked. */

#ifndef ROC_APP_SCREENS
#define ROC_APP_SCREENS 1 /* Filo programs that paint: the screens and the apps of /bin */
#endif
#ifndef ROC_APP_EDIT
#define ROC_APP_EDIT 1 /* the full-screen editor (edit, edt): 27 KB with a small text buffer */
#endif
#ifndef ROC_APP_COREWAR
#define ROC_APP_COREWAR 1 /* the arena, the assembler, the mars command: 255 KB of statics */
#endif
#ifndef ROC_APP_TOOLS
#define ROC_APP_TOOLS 1 /* filo build, dump, decompile...: 9 MB of statics */
#endif

/* What needs what, said here rather than left to the linker. */
#if ROC_APP_EDIT && !ROC_APP_SCREENS
#error "ROC_APP_EDIT needs ROC_APP_SCREENS: the editor's face is a screen"
#endif
#if ROC_APP_COREWAR && !ROC_APP_SCREENS
#error "ROC_APP_COREWAR needs ROC_APP_SCREENS: the arena picks its warriors on a screen"
#endif

/* ---- how big the build is ---- */

/* The terminal, the pager and the text buffer are filo-term's: their sizes
   are in its term_config.h, overridden the same way. */
#include "term_config.h"

/* The user's own files, held in memory because the browser has nowhere else
   to put them. A board with a card reader sets this to nothing and mounts
   the card instead. 2 MB. */
#ifndef ROC_CFG_UFS_DATA_CAP
#define ROC_CFG_UFS_DATA_CAP (2 * 1024 * 1024)
#endif
#ifndef ROC_CFG_UFS_FILE_MAX
#define ROC_CFG_UFS_FILE_MAX (512 * 1024)
#endif
#ifndef ROC_CFG_UFS_FILES_MAX
#define ROC_CFG_UFS_FILES_MAX 64
#endif

/* The index of the site: one arena for the names and one entry per file.
   A board serving its own tree needs a fraction of it. 1.1 MB. */
#ifndef ROC_CFG_VFS_ARENA_CAP
#define ROC_CFG_VFS_ARENA_CAP (1024 * 1024)
#endif
#ifndef ROC_CFG_VFS_NODES_MAX
#define ROC_CFG_VFS_NODES_MAX 4096
#endif

/* Filo for the user: the filo command, the REPL and scripts under bin. A build
   without user scripting sets these low; they cannot go to zero while the
   command exists. 8.5 MB. */
/* A program's IR lives here: ~30x its source (sed takes most of it). */
#ifndef ROC_CFG_SC_MEM_PERSISTENT
#define ROC_CFG_SC_MEM_PERSISTENT (1024 * 1024)
#endif
/* A run's memory: sort holds its whole input, up to 20000 lines and 1 MB,
   with a key a line and the merge's two halves (the worst, 20000 lines of
   52 bytes sorted on two keys, fits 10 MB measured; 8 did not do). */
#ifndef ROC_CFG_SC_MEM_RUN
#define ROC_CFG_SC_MEM_RUN (10 * 1024 * 1024)
#endif
#ifndef ROC_CFG_SC_SRC_MAX
#define ROC_CFG_SC_SRC_MAX (256 * 1024)
#endif

/* Filo for the shell's own screens. These go away with the last screen
   written in Filo, and not before. 820 KB. */
#ifndef ROC_CFG_SCR_MEM_PERSISTENT
#define ROC_CFG_SCR_MEM_PERSISTENT (512 * 1024)
#endif
#ifndef ROC_CFG_SCR_MEM_RUN
#define ROC_CFG_SCR_MEM_RUN (256 * 1024)
#endif

/* Regular expressions a script may hold compiled at once (re-compile):
   about 4 KB each, in the script's state. At most 32. */
#ifndef ROC_CFG_SC_REGEX
#define ROC_CFG_SC_REGEX 32
#endif

/* Files a script may hold open at once (file-open), each with this much
   read ahead for file-line. */
#ifndef ROC_CFG_SC_FILES
#define ROC_CFG_SC_FILES 8
#endif
#ifndef ROC_CFG_SC_FILE_BUF
#define ROC_CFG_SC_FILE_BUF 4096
#endif

/* A line of the REPL, until its parens close, twice: as typed and as
   shown. */
#ifndef ROC_CFG_SC_REPL
#define ROC_CFG_SC_REPL 4096
#endif

/* What one redirected command may write before it is cut. 256 KB. */
#ifndef ROC_CFG_CAP_MAX
#define ROC_CFG_CAP_MAX (256 * 1024)
#endif

/* A | larger than CAP_MAX waits in the host's spool, read back in pieces
   this big: the size is speed only, and a host with no spool reads none. */
#ifndef ROC_CFG_SPOOL_PIECE
#define ROC_CFG_SPOOL_PIECE 4096
#endif

/* What one 2> keeps of the refusals; a block's own 2> keeps four times
   that, and a call from a script one more. */
#ifndef ROC_CFG_ERRCAP
#define ROC_CFG_ERRCAP 2048
#endif

/* The kept home, on its way in or out in one piece. 64 KB. */
#ifndef ROC_CFG_HOME_BLOB_CAP
#define ROC_CFG_HOME_BLOB_CAP (64 * 1024)
#endif

/* Commands a line runs before it lets the tick come back to it: a while
   that never ends leaves the terminal alive and ^C heard. A $(...) cannot
   wait for a tick, so it stops, an error, at SUB_STEPS. */
#ifndef ROC_CFG_LIST_STEPS
#define ROC_CFG_LIST_STEPS 256
#endif
#ifndef ROC_CFG_SUB_STEPS
#define ROC_CFG_SUB_STEPS 100000
#endif

/* Functions the shell keeps (NAME() { ... }), and the bytes of their
   bodies, all of them. */
#ifndef ROC_CFG_FUNCS
#define ROC_CFG_FUNCS 32
#endif
#ifndef ROC_CFG_FUNC_TEXT
#define ROC_CFG_FUNC_TEXT 16384
#endif

/* The shell scripts running at once (sh f inside sh g...): their texts and
   what each subshell saved. */
#ifndef ROC_CFG_SH_TEXT
#define ROC_CFG_SH_TEXT (32 * 1024)
#endif

/* The shell's lines. SH_LINE is the text one command is read from, and
   eight times it what the command expands to; every line being read at
   once has both (a $(...) inside another, the word of ${x:-...}, a trap),
   so it multiplies: 10 KB a line at the default. SH_SCRIPT is a line and
   the lines that continue it (an if typed whole, a here-document); the
   line running, the one being typed, and each $(...) waiting keep one.
   SH_WORDS are what one command may have, a pointer each. */
#ifndef ROC_CFG_SH_LINE
#define ROC_CFG_SH_LINE 1024
#endif
#ifndef ROC_CFG_SH_SCRIPT
#define ROC_CFG_SH_SCRIPT 8192
#endif
#ifndef ROC_CFG_SH_WORDS
#define ROC_CFG_SH_WORDS 256
#endif

/* if, while, for and functions open one inside another: each level keeps
   where its output, input and refusals go, a path each. 10 KB. */
#ifndef ROC_CFG_SH_NEST
#define ROC_CFG_SH_NEST 16
#endif

/* A compiled regular expression: its instructions, 6 bytes each, and its
   bracket expressions, 32 bytes each. 4 KB at the defaults. A match
   backtracks on one stack, 12 bytes a level on 32 bits (96 KB): a long
   line under a .* needs about a level a byte. */
#ifndef ROC_CFG_RE_INST
#define ROC_CFG_RE_INST 512
#endif
#ifndef ROC_CFG_RE_CLASSES
#define ROC_CFG_RE_CLASSES 32
#endif
#ifndef ROC_CFG_RE_STACK
#define ROC_CFG_RE_STACK 8192
#endif

/* $(...) inside $(...), this deep: a copy of the line (~18 KB) each. */
#ifndef ROC_CFG_SUB_DEPTH
#define ROC_CFG_SUB_DEPTH 4
#endif

/* Variables made local (local x) in the functions running, all of them. */
#ifndef ROC_CFG_LOCALS
#define ROC_CFG_LOCALS 32
#endif

/* Lines of history, each one ROC_LINE_MAX bytes. 32 KB. */
#ifndef ROC_CFG_HIST_MAX
#define ROC_CFG_HIST_MAX 64
#endif

#endif
