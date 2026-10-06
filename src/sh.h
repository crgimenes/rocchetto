/* A command line read as sh reads it (POSIX, "Shell Command Language",
   2.2 to 2.6): words split at blanks, 'single quotes' taking everything
   literally, "double quotes" everything but $ and \ before $ ` " \ and a
   newline, a backslash outside quotes taking the next character as it is,
   # at the start of a word beginning a comment, and the redirections, the
   word after them a file, wherever they are on the line: > and >> for the
   output, < for the input, 2> and 2>> for the errors, 2>&1 the errors
   where the output goes (1> is >, as the digit right before the operator
   names what goes: only 1 and 2 here). sh_read
   also expands: $NAME, ${NAME}, $? and, with sh_expand, $(command) (split
   into fields at blanks when not quoted) and the * ? [...] of a word into
   the paths they match, ~ at the start of a word, and takes NAME=value before the
   command for an assignment, and stops at the ;, &&, || or | that ends the
   command, for a list or a pipeline. What the shell does not have yet (&,
   `...`) is read as text. */
#ifndef ROC_SH_H
#define ROC_SH_H

#include <stdbool.h>
#include <stddef.h>

#include "roc_config.h"

enum {
    SH_WORDS_MAX = ROC_CFG_SH_WORDS, /* a pattern may make many */
    SH_LINE_MAX = ROC_CFG_SH_LINE,   /* the text a line is read from, quotes and all */
    SH_VARS_MAX = 16,
    SH_NAME_MAX = 32,
    SH_VALUE_MAX = 256,
    SH_SCRIPT_MAX = ROC_CFG_SH_SCRIPT, /* a line and the lines that continue it */
    SH_NEST_MAX = ROC_CFG_SH_NEST,     /* if, while, for, one inside another */
};

/* The reserved words, where a command would start: the compound commands
   of POSIX 2.9.4. SH_RW_DSEMI is no word: the ;; of case, for sh_skip to
   look for; SH_RW_CALL neither: a function being run, for the runner.
   ( and ) are operators, not words, but open and close a subshell the
   same way. */
enum {
    SH_RW_NONE,
    SH_RW_IF,
    SH_RW_THEN,
    SH_RW_ELIF,
    SH_RW_ELSE,
    SH_RW_FI,
    SH_RW_FOR,
    SH_RW_DO,
    SH_RW_DONE,
    SH_RW_WHILE,
    SH_RW_UNTIL,
    SH_RW_CASE,
    SH_RW_ESAC,
    SH_RW_LBRACE,
    SH_RW_RBRACE,
    SH_RW_DSEMI,
    SH_RW_CALL,
    SH_RW_LPAREN,
    SH_RW_RPAREN,
};

/* What ends a command: the line, or the operator before the next one. */
enum {
    SH_OP_END,
    SH_OP_SEMI,  /* ; the next runs anyway */
    SH_OP_AND,   /* && the next runs when this one ended with 0 */
    SH_OP_OR,    /* || the next runs when this one did not */
    SH_OP_PIPE,  /* | the next reads what this one writes */
    SH_OP_DSEMI, /* ;; the end of a branch of case */
    SH_OP_BG,    /* & the next runs anyway: nothing runs in the background here */
};

typedef struct {
    int argc;
    char *argv[SH_WORDS_MAX + 1]; /* NULL after the last, as main's */
    char *assign[SH_WORDS_MAX];   /* NAME=value words before the command */
    int nassign;
    const char *out;  /* where > or >> sends the output; NULL for none */
    bool append;      /* >> */
    const char *in;   /* where < reads the input from; NULL for none */
    const char *here; /* a here-document's text, expanded: the input; NULL for none */
    size_t here_len;
    const char *err; /* where 2> or 2>> sends the errors; NULL for none */
    bool err_append; /* 2>> */
    bool err_to_out; /* 2>&1 */
    bool out_to_err; /* >&2 */
    int op;          /* SH_OP_*: what ended the command */
    size_t next;     /* where the next command starts in the line (sh_read) */
    char buf[8 * SH_LINE_MAX];
} sh_line;

/* The value of the variable name (len bytes: a name, or "?"), NULL when it
   is not set. */
typedef const char *(*sh_lookup)(void *user, const char *name, size_t len);

/* Reads line into words, nothing expanded: how a command reads back the
   words sh_join wrote. False, with why ("Unterminated quoted string"), for
   a line sh would refuse, or one past SH_WORDS_MAX words or SH_LINE_MAX
   bytes. */
bool sh_split(const char *line, sh_line *l, char *why, size_t cap);

/* What $(command) writes, the command's text (len bytes) run: NULL, with
   why, when it cannot be. The lexer drops the newlines it ends with. */
typedef const char *(*sh_subst)(void *user, const char *cmd, size_t len, size_t *out_len, char *why,
                                size_t cap);

/* The paths pattern matches (* ? [...], a \ before a byte taking it as it
   is), sorted, a NUL after each, and how many in count; NULL when none, and
   then count is -1 when there were more than it could hold. */
typedef const char *(*sh_glob)(void *user, const char *pattern, int *count);

/* ${name:=value}: the variable set; false with why when it cannot be. */
typedef bool (*sh_set)(void *user, const char *name, size_t len, const char *value, char *why,
                       size_t cap);

typedef struct {
    sh_lookup look;
    sh_subst subst; /* NULL: $( is text */
    sh_glob glob;   /* NULL: * ? [ are text */
    void *user;
    sh_set set; /* NULL: ${x:=v} gives v and sets nothing */
} sh_env;

/* sh_split with the expansions and the assignments: the line as typed, up
   to the end of its first command, which l->op and l->next say. */
bool sh_expand(const char *line, sh_line *l, const sh_env *env, char *why, size_t cap);
/* set -u: a parameter not set is an error when expanded ($x, ${x}, ${#x};
   not ${x-...} and the like, not $@ or $*). The shell's, for the lexer. */
extern bool sh_nounset;

/* The ! that negates a pipeline, at p where a command would start: its
   length, blanks after it included; 0 for none. */
size_t sh_bang(const char *p);

/* Whether the command at line is only assignments, more than one: sh
   sets them one by one, a=1 b=$a seeing the new a. Read, not run. */
bool sh_assigns_only(const char *line);
/* sh_expand that sets each assignment (with env->set) as it is read. */
bool sh_expand_assigning(const char *line, sh_line *l, const sh_env *env, char *why, size_t cap);

/* sh_expand with no $(...). */
bool sh_read(const char *line, sh_line *l, sh_lookup look, void *user, char *why, size_t cap);

/* The here-documents of text (cmd <<WORD, <<-WORD, the lines after the
   command's up to one that is WORD) moved into their commands, as a token
   only the lexer reads, so the text reads by position like any other: out
   is text with the bodies out of the way. False with why for a body not
   ended ("end of file unexpected": sh_needs_more wants the rest) or too
   long. */
bool sh_heredocs(const char *text, char *out, size_t cap, char *why, size_t whycap);

/* The value of the alias name (len bytes), NULL when there is none. */
typedef const char *(*sh_alias)(void *user, const char *name, size_t len);

/* text with the aliases replaced where a command starts, as sh does when
   it reads a line (again for the first word of a value, never an alias
   inside itself; the word after a value that ends in a blank too). Not in
   $(...) yet. False with why when out is too small. */
bool sh_aliases(const char *text, char *out, size_t cap, sh_alias find, void *user, char *why,
                size_t whycap);

/* Whether the whole line reads, every command of its list and every if,
   while and for closed, before any of them runs, as sh reads a line: false
   with why ("Syntax error: \";\" unexpected") for one that does not. */
bool sh_check(const char *line, char *why, size_t cap);

/* Whether why, from sh_check, says the line only stopped too soon (a quote
   or an if left open, a && at the end): sh reads another line for it. */
bool sh_needs_more(const char *why);

/* The blanks, newlines and comments at p: how many bytes. */
size_t sh_blanks(const char *p);

/* The reserved word at p, where a command would start, and its length;
   SH_RW_NONE for a command. Only as typed: a quoted if is a word. */
int sh_reserved(const char *p, size_t *len);

/* Whether rw opens a block: if, for, while, until, case, {. */
bool sh_opens(int rw);

/* A function's definition at p, NAME(): its length through the ), the
   name's in *name_len; 0 for anything else. */
size_t sh_fdef(const char *p, size_t *name_len);

/* for NAME do (no in, no ;) at p, past the for: where the do is; 0 for
   any other head. */
size_t sh_for_do(const char *p);

/* The closer of the block rw opens: fi, done, esac or }. */
int sh_closer(int rw);

/* case's head at p (past case): the word, expanded but not split into
   fields nor into paths, in l->argv[0], and in after it; *len past the in.
   False with why. */
bool sh_case_head(const char *p, sh_line *l, const sh_env *env, size_t *len, char *why, size_t cap);

/* The patterns of a branch of case at p: [(] pattern [| pattern]... ),
   each expanded and made a pattern for sh_match (what was quoted in it
   escaped), in l->argv; *len past the ). False with why. */
bool sh_case_patterns(const char *p, sh_line *l, const sh_env *env, size_t *len, char *why,
                      size_t cap);

/* Whether name matches pat: * any run, ? one byte, [abc] [a-z] [!a] one
   of (or none of) a set, \x the byte x. */
bool sh_match(const char *pat, const char *name);

/* From pos on, the first reserved word of targets (a bit for each
   SH_RW_*) at depth 0, the ifs and loops opened on the way passed over
   whole, and depth fi, done or esac closed first (-1: pos is at the
   opener whose closer is wanted): where it is, and which
   (for SH_RW_DSEMI, the ;; of the case it is in: where after it). Read,
   not run. False when there is none. */
bool sh_skip(const char *text, size_t pos, unsigned targets, int depth, size_t *at, int *found);

/* What follows a fi or a done at p, read as a command with no words: the
   block's redirections (> >> < 2> 2>> 2>&1) and the operator after them
   (l->op, l->next). False with why for a word. */
bool sh_after_close(const char *p, sh_line *l, const sh_env *env, char *why, size_t cap);

/* argc words as one line sh_split reads back into the same words: a word as
   it is where it needs no quotes, else in single quotes ('\'' for one
   inside). False when dst is too small. */
bool sh_join(char *const *argv, int argc, char *dst, size_t cap);

/* The shell's own variables: set with NAME=value, read with $NAME. */
typedef struct {
    char name[SH_NAME_MAX];
    char value[SH_VALUE_MAX];
    bool held;     /* readonly: no assignment, no unset */
    bool exported; /* export: what a utility it runs sees (env, env-get) */
} sh_var;

typedef struct {
    sh_var v[SH_VARS_MAX];
    int n;
} sh_vars;

/* Whether the len bytes at s make a name: a letter or _, then letters,
   digits and _. */
bool sh_is_name(const char *s, size_t len);
/* name exported (on) or not: what a utility sees. An unset name is set to
   "" first. False with why when it cannot be. */
bool sh_var_export(sh_vars *vs, const char *name, bool on, char *why, size_t cap);
bool sh_var_exported(const sh_vars *vs, const char *name);
/* The value of name, NULL when it is not set. */
const char *sh_var_get(const sh_vars *vs, const char *name, size_t len);
/* An assignment, "NAME=value": false with why when the table is full or
   the value too long. */
bool sh_var_assign(sh_vars *vs, const char *word, char *why, size_t cap);
void sh_var_unset(sh_vars *vs, const char *name);
/* readonly name: set from now on as it is (made, empty, when it is not);
   false with why when the table is full. */
bool sh_var_hold(sh_vars *vs, const char *name, char *why, size_t cap);
bool sh_var_held(const sh_vars *vs, const char *name);

#endif
