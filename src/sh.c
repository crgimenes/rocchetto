#include "sh.h"

#include <stdio.h>
#include <string.h>

static bool blank(char c) {
    if (c == ' ') {
        return true;
    }
    return c == '\t';
}

bool sh_nounset;

size_t sh_bang(const char *p) {
    if (p[0] != '!' || (p[1] != ' ' && p[1] != '\t')) {
        return 0;
    }
    size_t n = 1;
    while (p[n] == ' ' || p[n] == '\t') {
        n++;
    }
    return n;
}

static bool fail(char *why, size_t cap, const char *text) {
    (void)snprintf(why, cap, "%s", text);
    return false;
}

static bool name_start(char c) {
    if (c == '_') {
        return true;
    }
    if (c >= 'a' && c <= 'z') {
        return true;
    }
    if (c < 'A') {
        return false;
    }
    return c <= 'Z';
}

static bool name_char(char c) {
    if (name_start(c)) {
        return true;
    }
    if (c < '0') {
        return false;
    }
    return c <= '9';
}

bool sh_is_name(const char *s, size_t len) {
    if (len == 0 || !name_start(s[0])) {
        return false;
    }
    for (size_t i = 1; i < len; i++) {
        if (!name_char(s[i])) {
            return false;
        }
    }
    return true;
}

/* What the word being read is the file of: none, >, <, 2>. */
enum { R_NONE, R_OUT, R_IN, R_ERR };

/* An assignment word as it is read: not yet known, known not to be one, or
   past its = (where an expansion is not split into fields). */
enum { AS_NAME, AS_NOT, AS_VALUE };

/* What read_line reads: a command, or one word as case wants it (no
   fields, no paths, ended by ) and | too): its word, or a pattern. */
enum { M_LINE, M_WORD, M_PATTERN, M_TEXT, M_TEXT_PATTERN, M_HERE };

/* The reading of one line: where the next byte goes, and the word being
   made, which exists once any part of it was read (quotes too: '' is an
   empty word, $unset alone is none). */
typedef struct {
    sh_line *l;
    sh_lookup look; /* NULL: nothing expands, nothing assigns */
    sh_subst subst; /* NULL: $( is text */
    sh_glob glob;   /* NULL: * ? [ are text */
    void *user;
    sh_set set;
    char *o;
    char *end;
    char *start;
    bool in_word;
    bool plain;      /* the word is literal bytes only: a digit before > names what goes */
    int redirect;    /* R_*: the word being made is a redirection's file */
    int as;          /* AS_*: whether the word is an assignment */
    int mode;        /* M_*: a line, or one word of case */
    bool assign_now; /* a=1 b=$a: each assignment set as it is read */
    const char *err;
    char why[96]; /* what a $(...) said went wrong */
    /* the word as a pattern: its quoted * ? [ ] \ escaped, and whether an
       unquoted * ? [ makes it one at all */
    char pat[2 * SH_LINE_MAX];
    size_t patlen;
    bool globby;
    bool pat_full;
} lexer;

static void begin(lexer *x) {
    if (!x->in_word) {
        x->in_word = true;
        x->plain = true;
        x->start = x->o;
        x->patlen = 0;
        x->globby = false;
        x->pat_full = false;
        x->as = AS_NOT;
        if (x->look != NULL && x->l->argc == 0 && x->redirect == R_NONE && x->mode == M_LINE) {
            x->as = AS_NAME;
        }
    }
}

static void pat_put(lexer *x, char c) {
    if (x->patlen + 1 >= sizeof(x->pat)) {
        x->pat_full = true; /* too long to be a pattern: the word as it is */
        return;
    }
    x->pat[x->patlen] = c;
    x->patlen++;
}

/* A byte of the word; quoted, a * ? [ in it is only itself. */
static void put_byte(lexer *x, char c, bool quoted_byte) {
    begin(x);
    if (x->o + 1 >= x->end) {
        x->err = "Line too long";
        return;
    }
    *x->o++ = c;
    bool special = false; /* a byte a pattern reads as more than itself */
    if (c != '\0' && strchr("*?[]\\", c) != NULL) {
        special = true;
    }
    if (quoted_byte && special) {
        pat_put(x, '\\');
    }
    pat_put(x, c);
    if (!quoted_byte && (c == '*' || c == '?' || c == '[')) {
        x->globby = true;
    }
}

static void emit(lexer *x, char c) {
    put_byte(x, c, false);
}

static void emitq(lexer *x, char c) {
    put_byte(x, c, true);
}

/* A literal byte of the word, outside quotes: the one place an assignment's
   name and = are read. */
static void literal(lexer *x, char c) {
    begin(x);
    if (x->as == AS_NAME) {
        if (c == '=' && x->o > x->start) {
            x->as = AS_VALUE;
        } else if (!name_char(c) || (x->o == x->start && !name_start(c))) {
            x->as = AS_NOT;
        }
    }
    emit(x, c);
}

/* A part of the word that is not a literal byte outside quotes. */
static void quoted(lexer *x) {
    begin(x);
    x->plain = false;
    if (x->as == AS_NAME) {
        x->as = AS_NOT;
    }
}

/* A word with an unquoted * ? [: the names it matches, sorted, each a word
   in its place; false, the word left as it is, when it matches none (or
   is no pattern), as sh leaves it. */
static bool glob_word(lexer *x) {
    if (!x->globby || x->pat_full || x->glob == NULL) {
        return false;
    }
    x->pat[x->patlen] = '\0';
    int n = 0;
    const char *names = x->glob(x->user, x->pat, &n);
    if (n < 0) {
        x->err = "Too many words: a pattern matched more than a line holds";
        return true;
    }
    if (names == NULL || n == 0) {
        return false;
    }
    sh_line *l = x->l;
    x->o = x->start; /* the matches take the word's place */
    for (int i = 0; i < n; i++) {
        size_t len = strlen(names);
        if (l->argc == SH_WORDS_MAX) {
            x->err = "Too many words";
            return true;
        }
        if (len + 1 > (size_t)(x->end - x->o)) {
            x->err = "Line too long";
            return true;
        }
        memcpy(x->o, names, len + 1);
        l->argv[l->argc] = x->o;
        l->argc++;
        x->o += len + 1;
        names += len + 1;
    }
    return true;
}

static void finish(lexer *x) {
    if (!x->in_word || x->err != NULL) {
        return;
    }
    x->in_word = false;
    if (x->o + 1 > x->end) {
        x->err = "Line too long";
        return;
    }
    *x->o++ = '\0';
    sh_line *l = x->l;
    if (x->redirect != R_NONE) { /* the last one of a kind wins, as in sh */
        if (x->redirect == R_OUT) {
            l->out = x->start;
        } else if (x->redirect == R_IN) {
            l->in = x->start;
            l->here = NULL; /* the last of them wins */
        } else {
            l->err = x->start;
        }
        x->redirect = R_NONE;
    } else if (x->as == AS_VALUE) {
        l->assign[l->nassign] = x->start;
        l->nassign++;
        const char *eq = strchr(x->start, '=');
        if (x->assign_now && x->set != NULL && eq != NULL &&
            !x->set(x->user, x->start, (size_t)(eq - x->start), eq + 1, x->why, sizeof(x->why))) {
            x->err = x->why;
        }
    } else if (l->argc == SH_WORDS_MAX || l->nassign == SH_WORDS_MAX) {
        x->err = "Too many words";
    } else if (!glob_word(x)) {
        l->argv[l->argc] = x->start;
        l->argc++;
    }
}

/* The field separators: $IFS, space tab newline when it is not set. */
static const char *ifs_of(const lexer *x) {
    const char *v = NULL;
    if (x->look != NULL) {
        v = x->look(x->user, "IFS", 3);
    }
    if (v == NULL) {
        return " \t\n";
    }
    return v;
}

/* An expansion's value in the word: split into fields at the bytes of IFS
   unless quoted, or the value of an assignment, or a redirection's file.
   A run of IFS's blanks is one break, and none at the ends; any other byte
   of it ends exactly one field, so a::b is three and :b two. */
static void put_value(lexer *x, const char *v, size_t n, bool in_quotes) {
    bool split = false;
    if (!in_quotes && x->as != AS_VALUE && x->redirect == R_NONE && x->mode == M_LINE) {
        split = true;
    }
    if (x->in_word) {
        x->plain = false;
    }
    if (in_quotes) {
        quoted(x);
    } else if (x->in_word && x->as == AS_NAME) {
        x->as = AS_NOT;
    }
    const char *ifs = split ? ifs_of(x) : "";
    bool ended = false; /* a field ended by a byte of IFS that is no blank */
    for (size_t i = 0; i < n; i++) {
        char c = v[i];
        if (split && c != '\0' && strchr(ifs, c) != NULL) {
            bool white = false;
            if (blank(c) || c == '\n') {
                white = true;
            }
            if (!white && !x->in_word && (ended || i == 0)) {
                quoted(x); /* nothing before it: an empty field */
            }
            if (!white) {
                ended = true;
            } else if (x->in_word) {
                ended = false;
            }
            finish(x);
            continue;
        }
        ended = false;
        if (split && !x->in_word) {
            begin(x);
            x->as = AS_NOT;
        }
        x->plain = false;
        if (in_quotes) {
            emitq(x, c);
        } else {
            emit(x, c);
        }
    }
}

/* Whether the word w starts at p, a whole word. */
static bool word_at(const char *p, const char *w) {
    size_t n = strlen(w);
    if (strncmp(p, w, n) != 0) {
        return false;
    }
    if (p[n] == '\0' || blank(p[n])) {
        return true;
    }
    return strchr("\n;&|()<>", p[n]) != NULL;
}

/* The ) that closes the $( before p, quotes and inner parens skipped, and
   the ) of a case's patterns (case x in a) ...;; esac) too; NULL when none
   does. */
static const char *subst_end(const char *p) {
    int depth = 1;
    int cases = 0;        /* case ... esac open inside */
    bool head = false;    /* after case: its word, until in */
    bool pattern = false; /* a branch's patterns: their ) closes nothing */
    bool start = true;    /* where a word starts */
    while (*p != '\0') {
        char c = *p;
        if (c == '\\' && p[1] != '\0') {
            p += 2;
            start = false;
            continue;
        }
        if (c == '\'') {
            const char *q = strchr(p + 1, '\'');
            if (q == NULL) {
                return NULL;
            }
            p = q + 1;
            start = false;
            continue;
        }
        if (c == '"') {
            p++;
            while (*p != '\0' && *p != '"') {
                p += p[0] == '\\' && p[1] != '\0' ? 2 : 1;
            }
            if (*p == '\0') {
                return NULL;
            }
            p++;
            start = false;
            continue;
        }
        if (start && !pattern && word_at(p, "case")) {
            cases++;
            head = true;
            p += 4;
            start = false;
            continue;
        }
        if (start && head && word_at(p, "in")) {
            head = false;
            pattern = true;
            p += 2;
            continue;
        }
        if (start && cases > 0 && word_at(p, "esac")) {
            cases--;
            pattern = false;
            p += 4;
            start = false;
            continue;
        }
        if (c == ';' && p[1] == ';' && cases > 0) {
            pattern = true;
            p += 2;
            start = true;
            continue;
        }
        if (c == '(' && pattern && start) {
            p++; /* (a) as a pattern: its opening one */
            continue;
        }
        if (c == ')' && pattern) {
            pattern = false;
        } else if (c == '(') {
            depth++;
        } else if (c == ')') {
            depth--;
            if (depth == 0) {
                return p;
            }
        }
        start = false;
        if (blank(c) || strchr("\n;&|()", c) != NULL) {
            start = true;
        }
        p++;
    }
    return NULL;
}

/* What the command (len bytes at cmd) writes, its last newlines dropped,
   in the word. */
static void put_subst(lexer *x, const char *cmd, size_t len, bool in_quotes) {
    size_t n = 0;
    const char *v = x->subst(x->user, cmd, len, &n, x->why, sizeof(x->why));
    if (v == NULL) {
        x->err = x->why;
        return;
    }
    while (n > 0 && v[n - 1] == '\n') {
        n--;
    }
    put_value(x, v, n, in_quotes);
}

/* $(command) at p (past the $). */
static const char *substitute(lexer *x, const char *p, bool in_quotes) {
    const char *end = subst_end(p + 1);
    if (end == NULL) {
        x->err = "Syntax error: end of file unexpected (expecting \")\")";
        return p;
    }
    put_subst(x, p + 1, (size_t)(end - p - 1), in_quotes);
    return end + 1;
}

/* `command` at p (past the `): as $(command), a \ before $ ` \ (and " in
   double quotes) taken off first, as POSIX says. */
static const char *backquoted(lexer *x, const char *p, bool in_quotes) {
    char cmd[SH_LINE_MAX + 1];
    size_t n = 0;
    while (*p != '`') {
        if (*p == '\0') {
            x->err = "Syntax error: end of file unexpected (expecting \"`\")";
            return p;
        }
        if (*p == '\\' && p[1] != '\0' &&
            (strchr("$`\\", p[1]) != NULL || (in_quotes && p[1] == '"'))) {
            p++;
        }
        if (n + 1 >= sizeof(cmd)) {
            x->err = "Line too long";
            return p;
        }
        cmd[n++] = *p++;
    }
    cmd[n] = '\0';
    put_subst(x, cmd, n, in_quotes);
    return p + 1;
}

/* $NAME, ${NAME}, $? or $(command) at *p (past the $); the text of a lone $
   when neither follows. */
/* ---- $((...)): arithmetic, as C reads it, in 64 bits ---- */

static const char *anything(void *user, const char *name, size_t len);

typedef struct {
    const char *p;
    const char *end;
    sh_lookup look;
    sh_subst subst; /* NULL: no $(...) in it */
    void *user;
    bool lenient; /* only being checked: a name is 1, whatever it holds */
    const char *err;
    sh_set set; /* NULL: name = value is refused */
} arith;

static long long ar_expr(arith *a);

/* The )) that closes the $(( before p; NULL when a ) comes alone first
   (then it was a $( with a ( inside) or nothing closes. */
static const char *arith_end(const char *p) {
    int depth = 0;
    for (; *p != '\0'; p++) {
        if (*p == '(') {
            depth++;
        } else if (*p == ')' && depth > 0) {
            depth--;
        } else if (*p == ')') {
            if (p[1] == ')') {
                return p;
            }
            return NULL;
        }
    }
    return NULL;
}

static void ar_fail(arith *a, const char *err) {
    if (a->err == NULL) {
        a->err = err;
    }
    a->p = a->end;
}

/* The number that is all of s[0..n): decimal, 0x hex or 0 octal, a sign
   before it. */
static bool ar_number(const char *s, size_t n, long long *v) {
    size_t i = 0;
    bool neg = false;
    if (i < n && s[i] == '-') {
        neg = true;
        i++;
    } else if (i < n && s[i] == '+') {
        i++;
    }
    unsigned base = 10;
    if (i + 1 < n && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        base = 16;
        i += 2;
    } else if (i + 1 < n && s[i] == '0') {
        base = 8;
    }
    if (i == n) {
        return false;
    }
    unsigned long long u = 0;
    for (; i < n; i++) {
        char c = s[i];
        unsigned d = 99;
        if (c >= '0' && c <= '9') {
            d = (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (unsigned)(c - 'a') + 10U;
        } else if (c >= 'A' && c <= 'F') {
            d = (unsigned)(c - 'A') + 10U;
        }
        if (d >= base) {
            return false;
        }
        u = (u * base) + d;
    }
    if (neg) {
        u = 0 - u;
    }
    *v = (long long)u;
    return true;
}

/* A name in an expression, with or without its $: its value as a number,
   0 when it is not set or empty. */
static long long ar_name(arith *a, const char *name, size_t len) {
    const char *v = a->look(a->user, name, len);
    if (a->lenient) {
        return 1;
    }
    long long n = 0;
    if (v == NULL || v[0] == '\0') {
        return 0;
    }
    if (!ar_number(v, strlen(v), &n)) {
        ar_fail(a, "Illegal number");
    }
    return n;
}

static void ar_blanks(arith *a) {
    while (a->p < a->end && (blank(*a->p) || *a->p == '\n')) {
        a->p++;
    }
}

/* $(command) in an expression: what it writes, as a number. */
static long long ar_subst(arith *a, const char *cmd) {
    const char *close = subst_end(cmd);
    if (a->subst == NULL || close == NULL || close >= a->end) {
        ar_fail(a, "arithmetic expression: expecting primary");
        return 0;
    }
    size_t n = 0;
    char why[96];
    const char *out = a->subst(a->user, cmd, (size_t)(close - cmd), &n, why, sizeof(why));
    a->p = close + 1;
    if (out == NULL) {
        static char said[96]; /* what the $(...) said, as the error */
        (void)snprintf(said, sizeof(said), "%s", why);
        ar_fail(a, said);
        return 0;
    }
    while (n > 0 && (out[n - 1] == '\n' || blank(out[n - 1]))) {
        n--;
    }
    while (n > 0 && blank(*out)) {
        out++;
        n--;
    }
    long long v = 0;
    if (a->lenient) {
        return 1;
    }
    if (!ar_number(out, n, &v)) {
        ar_fail(a, "Illegal number");
    }
    return v;
}

static long long ar_primary(arith *a) {
    ar_blanks(a);
    const char *p = a->p;
    if (p >= a->end) {
        ar_fail(a, "arithmetic expression: expecting primary");
        return 0;
    }
    if (*p == '(') {
        a->p++;
        long long v = ar_expr(a);
        ar_blanks(a);
        if (a->p >= a->end || *a->p != ')') {
            ar_fail(a, "arithmetic expression: expecting ')'");
            return 0;
        }
        a->p++;
        return v;
    }
    if (*p >= '0' && *p <= '9') {
        size_t n = 0;
        while (p + n < a->end && name_char(p[n])) {
            n++;
        }
        long long v = 0;
        if (!ar_number(p, n, &v)) {
            ar_fail(a, "arithmetic expression: bad number");
        }
        a->p += n;
        return v;
    }
    if (*p == '$' && p + 2 < a->end && p[1] == '(' && p[2] == '(') {
        const char *close = arith_end(p + 3);
        if (close == NULL || close + 2 > a->end) {
            ar_fail(a, "arithmetic expression: expecting '))'");
            return 0;
        }
        arith in = {p + 3, close, a->look, a->subst, a->user, a->lenient, NULL, a->set};
        long long v = ar_expr(&in);
        ar_blanks(&in);
        if (in.err != NULL || in.p != in.end) {
            ar_fail(a, in.err != NULL ? in.err : "arithmetic expression: expecting EOF");
        }
        a->p = close + 2;
        return v;
    }
    if (*p == '$' && p + 1 < a->end && p[1] == '(') {
        return ar_subst(a, p + 2);
    }
    if (*p == '$') {
        p++;
    }
    bool braced = false;
    if (p < a->end && *p == '{') {
        braced = true;
        p++;
    }
    size_t n = 0;
    if (p < a->end && (*p == '?' || *p == '#' || (*p >= '0' && *p <= '9'))) {
        n = 1;
    } else {
        while (p + n < a->end && name_char(p[n]) && (n > 0 || name_start(p[n]))) {
            n++;
        }
    }
    if (n == 0 || (braced && (p + n >= a->end || p[n] != '}'))) {
        ar_fail(a, "arithmetic expression: expecting primary");
        return 0;
    }
    a->p = p + n + (braced ? 1U : 0U);
    return ar_name(a, p, n);
}

/* The operator at a->p, longest first; NULL for none. */
static const char *ar_op(arith *a, size_t *len) {
    static const char *const ops[] = {
        "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+", "-", "*", "/",
        "%",  "<",  ">",  "&",  "|",  "^",  "!",  "~",  "?", ":", "(", ")",
    };
    ar_blanks(a);
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        size_t n = strlen(ops[i]);
        if ((size_t)(a->end - a->p) >= n && strncmp(a->p, ops[i], n) == 0) {
            *len = n;
            return ops[i];
        }
    }
    return NULL;
}

static long long ar_unary(arith *a) {
    size_t n = 0;
    const char *op = ar_op(a, &n);
    if (op == NULL || n != 1 || strchr("+-!~", op[0]) == NULL) {
        return ar_primary(a);
    }
    a->p += n;
    long long v = ar_unary(a);
    switch (op[0]) {
    case '-':
        return (long long)(0 - (unsigned long long)v);
    case '!':
        return v == 0 ? 1 : 0;
    case '~':
        return (long long)~(unsigned long long)v;
    default:
        return v;
    }
}

/* How tightly op binds, C's order; 0 for what is not a binary operator. */
static int ar_prec(const char *op) {
    static const struct {
        const char *op;
        int prec;
    } table[] = {
        {"||", 1}, {"&&", 2}, {"|", 3},  {"^", 4},  {"&", 5},  {"==", 6},
        {"!=", 6}, {"<", 7},  {"<=", 7}, {">", 7},  {">=", 7}, {"<<", 8},
        {">>", 8}, {"+", 9},  {"-", 9},  {"*", 10}, {"/", 10}, {"%", 10},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(op, table[i].op) == 0) {
            return table[i].prec;
        }
    }
    return 0;
}

/* Two's complement throughout, as the shells do: + - * wrap, never trap. */
static long long ar_apply(arith *a, const char *op, long long l, long long r) {
    unsigned long long ul = (unsigned long long)l;
    unsigned long long ur = (unsigned long long)r;
    switch (op[0]) {
    case '+':
        return (long long)(ul + ur);
    case '-':
        return (long long)(ul - ur);
    case '*':
        return (long long)(ul * ur);
    case '/':
    case '%':
        if (r == 0) {
            if (!a->lenient) { /* checking: the names are not their values yet */
                ar_fail(a, "arithmetic expression: division by zero");
            }
            return 0;
        }
        if (r == -1) {
            return op[0] == '/' ? (long long)(0 - ul) : 0; /* no trap at the smallest */
        }
        return op[0] == '/' ? l / r : l % r;
    case '^':
        return (long long)(ul ^ ur);
    default:
        break;
    }
    if (strcmp(op, "<<") == 0) {
        return (long long)(ul << (ur & 63U));
    }
    if (strcmp(op, ">>") == 0) { /* the sign kept, as the shells do */
        if (l < 0) {
            return (long long)~(~ul >> (ur & 63U));
        }
        return (long long)(ul >> (ur & 63U));
    }
    if (strcmp(op, "|") == 0) {
        return (long long)(ul | ur);
    }
    if (strcmp(op, "&") == 0) {
        return (long long)(ul & ur);
    }
    bool t = false;
    if (strcmp(op, "||") == 0) {
        if (l != 0 || r != 0) {
            t = true;
        }
    } else if (strcmp(op, "&&") == 0) {
        if (l != 0 && r != 0) {
            t = true;
        }
    } else if (strcmp(op, "==") == 0) {
        t = l == r;
    } else if (strcmp(op, "!=") == 0) {
        t = l != r;
    } else if (strcmp(op, "<") == 0) {
        t = l < r;
    } else if (strcmp(op, "<=") == 0) {
        t = l <= r;
    } else if (strcmp(op, ">") == 0) {
        t = l > r;
    } else {
        t = l >= r;
    }
    return t ? 1 : 0;
}

static long long ar_binary(arith *a, int min) {
    long long v = ar_unary(a);
    for (;;) {
        size_t n = 0;
        const char *op = ar_op(a, &n);
        int prec = op != NULL ? ar_prec(op) : 0;
        if (prec == 0 || prec < min || a->err != NULL) {
            return v;
        }
        a->p += n;
        long long r = ar_binary(a, prec + 1);
        v = ar_apply(a, op, v, r);
    }
}

/* a ? b : c, loosest of all, right to left. */
static size_t format_ll(long long v, char *buf);

/* name = expr, name += expr and the like, right to left: the value set,
   as sh's $(( )) does. -1 in *did when what is at a->p is no assignment. */
static long long ar_assign(arith *a, bool *did) {
    static const char *const ops[] = {
        "<<=", ">>=", "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|=", "=",
    };
    *did = false;
    ar_blanks(a);
    const char *name = a->p;
    size_t len = 0;
    while (name + len < a->end && name_char(name[len]) && (len > 0 || name_start(*name))) {
        len++;
    }
    if (len == 0) {
        return 0;
    }
    const char *q = name + len;
    while (q < a->end && blank(*q)) {
        q++;
    }
    const char *op = NULL;
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]) && op == NULL; i++) {
        size_t n = strlen(ops[i]);
        if ((size_t)(a->end - q) >= n && strncmp(q, ops[i], n) == 0 && q[n] != '=') {
            op = ops[i];
        }
    }
    if (op == NULL) {
        return 0;
    }
    *did = true;
    a->p = q + strlen(op);
    long long r = ar_expr(a);
    long long v = r;
    if (op[0] != '=') {
        const char bin[3] = {op[0], op[1] == '=' ? '\0' : op[1], '\0'};
        v = ar_apply(a, bin, ar_name(a, name, len), r);
    }
    if (a->err != NULL || a->lenient) {
        return v;
    }
    char num[24];
    size_t n = format_ll(v, num);
    num[n] = '\0';
    static char why[96];
    if (a->set == NULL || !a->set(a->user, name, len, num, why, sizeof(why))) {
        ar_fail(a, a->set == NULL ? "arithmetic expression: nothing can be set here" : why);
    }
    return v;
}

static long long ar_expr(arith *a) {
    const char *start = a->p;
    bool did = false;
    long long set = ar_assign(a, &did);
    if (did) {
        return set;
    }
    a->p = start;
    long long c = ar_binary(a, 1);
    size_t n = 0;
    const char *op = ar_op(a, &n);
    if (op == NULL || op[0] != '?') {
        return c;
    }
    a->p += n;
    long long yes = ar_expr(a);
    op = ar_op(a, &n);
    if (op == NULL || op[0] != ':') {
        ar_fail(a, "arithmetic expression: expecting ':'");
        return 0;
    }
    a->p += n;
    long long no = ar_expr(a);
    return c != 0 ? yes : no;
}

static size_t format_ll(long long v, char *buf) {
    char digits[24];
    size_t d = sizeof(digits);
    unsigned long long u = (unsigned long long)v;
    if (v < 0) {
        u = 0 - u;
    }
    do {
        digits[--d] = (char)('0' + (u % 10U));
        u /= 10U;
    } while (u > 0);
    size_t n = 0;
    if (v < 0) {
        buf[n++] = '-';
    }
    memcpy(buf + n, digits + d, sizeof(digits) - d);
    return n + sizeof(digits) - d;
}

/* $((expr)) at p (past the $, at the first '('): the value, in place. */
static const char *arithmetic(lexer *x, const char *p, const char *close, bool in_quotes) {
    arith a = {p + 2, close, x->look, x->subst, x->user, x->look == anything, NULL, x->set};
    long long v = ar_expr(&a);
    ar_blanks(&a);
    if (a.err == NULL && a.p != a.end) {
        a.err = "arithmetic expression: expecting EOF";
    }
    if (a.err != NULL) {
        x->err = a.err;
        return close + 2;
    }
    char num[24];
    size_t n = format_ll(v, num);
    put_value(x, num, n, in_quotes);
    return close + 2;
}

/* $x with x not set: fine, unless set -u says it is an error (said). */
static bool not_set_ok(lexer *x, const char *name, size_t len) {
    if (!sh_nounset || (len == 1 && *name == '*')) {
        return true;
    }
    static char said[SH_NAME_MAX + 32];
    (void)snprintf(said, sizeof(said), "%.*s: parameter not set", (int)len, name);
    x->err = said;
    return false;
}

/* Whether there are no positional parameters: "$@" is then no word. */
static bool no_params(const lexer *x) {
    const char *n = x->look(x->user, "#", 1);
    if (n == NULL) {
        return false;
    }
    return strcmp(n, "0") == 0;
}

/* $@: the parameters, a \x1f between them, each its own field, quoted or
   not ("x$@y" joins x to the first and y to the last, as sh). */
static void put_params(lexer *x, const char *v, bool in_quotes) {
    if (v == NULL || no_params(x)) {
        return;
    }
    for (;;) {
        const char *cut = strchr(v, '\x1f');
        size_t n = cut != NULL ? (size_t)(cut - v) : strlen(v);
        put_value(x, v, n, in_quotes);
        if (cut == NULL) {
            return;
        }
        finish(x);
        if (in_quotes) {
            quoted(x); /* the next one a word even when empty */
        }
        v = cut + 1;
    }
}

static bool read_line(const char *line, sh_line *l, const sh_env *env, int mode, char *why,
                      size_t cap);

/* The } that closes the ${ before p: quotes, $( ) and inner ${ } passed
   over. NULL when none does. */
static const char *brace_end(const char *p) {
    while (*p != '\0') {
        if (*p == '\\' && p[1] != '\0') {
            p += 2;
        } else if (*p == '\'') {
            const char *q = strchr(p + 1, '\'');
            if (q == NULL) {
                return NULL;
            }
            p = q + 1;
        } else if (*p == '"') {
            p++;
            while (*p != '\0' && *p != '"') {
                p += p[0] == '\\' && p[1] != '\0' ? 2 : 1;
            }
            if (*p == '\0') {
                return NULL;
            }
            p++;
        } else if (p[0] == '$' && p[1] == '(') {
            const char *q = subst_end(p + 2);
            if (q == NULL) {
                return NULL;
            }
            p = q + 1;
        } else if (p[0] == '$' && p[1] == '{') {
            const char *q = brace_end(p + 2);
            if (q == NULL) {
                return NULL;
            }
            p = q + 1;
        } else if (*p == '}') {
            return p;
        } else {
            p++;
        }
    }
    return NULL;
}

/* The word of ${x:-word}, expanded whole (no fields, no paths), or as a
   pattern for % and #; NULL, x->err said, when it cannot be. */
static const char *expand_text(lexer *x, const char *w, size_t n, bool pattern) {
    enum { TEXT_DEPTH = 3 };
    static sh_line lines[TEXT_DEPTH];
    static char texts[TEXT_DEPTH][SH_LINE_MAX + 1];
    static int depth;
    if (depth >= TEXT_DEPTH || n > SH_LINE_MAX) {
        x->err = "Bad substitution: too deep or too long";
        return NULL;
    }
    int d = depth++;
    memcpy(texts[d], w, n);
    texts[d][n] = '\0';
    sh_line *l = &lines[d];
    l->argc = 0;
    l->nassign = 0;
    const sh_env env = {x->look, x->subst, NULL, x->user, x->set};
    bool ok =
        read_line(texts[d], l, &env, pattern ? M_TEXT_PATTERN : M_TEXT, x->why, sizeof(x->why));
    depth--;
    if (!ok) {
        x->err = x->why;
        return NULL;
    }
    return l->argv[0];
}

/* ${x%pat} and the like: what is left of v when the shortest (or, twice,
   the longest) end (%) or start (#) that pat matches goes. */
static void trim(lexer *x, const char *v, const char *pat, char op, bool longest, bool in_quotes) {
    static char part[SH_LINE_MAX + 1];
    size_t n = strlen(v);
    if (n > SH_LINE_MAX) {
        n = SH_LINE_MAX;
    }
    size_t keep_from = 0;
    size_t keep_to = n;
    for (size_t k = 0; k <= n; k++) {
        size_t cut = longest ? k : n - k; /* how much of v stays out of the match */
        bool hit = false;
        if (op == '%') {
            hit = sh_match(pat, v + (longest ? k : n - k));
        } else {
            size_t len = longest ? n - k : k;
            memcpy(part, v, len);
            part[len] = '\0';
            hit = sh_match(pat, part);
        }
        if (hit && op == '%') {
            keep_to = cut;
            break;
        }
        if (hit) {
            keep_from = longest ? n - k : k;
            break;
        }
    }
    put_value(x, v + keep_from, keep_to - keep_from, in_quotes);
}

/* ${...} at p (past the {): ${x}, ${#x}, and ${x op word} for op in :- - :=
   = :+ + :? ? % %% # ##, as POSIX 2.6.2 says. */
static const char *braced(lexer *x, const char *p, bool in_quotes) {
    const char *close = brace_end(p);
    if (close == NULL) {
        x->err = "Syntax error: Missing '}'";
        return p;
    }
    const char *after = close + 1;
    bool length = false;
    if (p[0] == '#' && p + 1 < close) { /* ${#x}; ${#} alone is $# */
        length = true;
        p++;
    }
    const char *name = p;
    size_t len = 0;
    if (strchr("?#@*-$!", *p) != NULL && p < close) {
        len = 1;
    } else if (*p >= '0' && *p <= '9') {
        while (name + len < close && name[len] >= '0' && name[len] <= '9') {
            len++;
        }
    } else {
        while (name + len < close && name_char(name[len]) && (len > 0 || name_start(*name))) {
            len++;
        }
    }
    const char *op = name + len;
    if (len == 0 || (length && op != close)) {
        x->err = "Bad substitution";
        return after;
    }
    const char *v = x->look(x->user, name, len);
    if (length && v == NULL && !not_set_ok(x, name, len)) {
        return after;
    }
    if (length) {
        size_t chars = 0;
        for (const char *c = v != NULL ? v : ""; *c != '\0'; c++) {
            if (((unsigned char)*c & 0xC0U) != 0x80U) {
                chars++; /* a UTF-8 character: its lead byte */
            }
        }
        char num[24];
        size_t nn = format_ll((long long)chars, num);
        put_value(x, num, nn, in_quotes);
        return after;
    }
    if (op == close) {
        if (len == 1 && *name == '@') {
            put_params(x, v, in_quotes);
        } else if (v == NULL && !not_set_ok(x, name, len)) {
            return after;
        } else {
            put_value(x, v, v != NULL ? strlen(v) : 0, in_quotes);
        }
        return after;
    }
    bool colon = *op == ':';
    if (colon) {
        op++;
    }
    char kind = *op;
    bool twice = false;
    if ((kind == '%' || kind == '#') && !colon && op[1] == kind) {
        twice = true;
        op++;
    }
    if (op >= close || strchr("-=+?%#", kind) == NULL || (colon && (kind == '%' || kind == '#'))) {
        x->err = "Bad substitution";
        return after;
    }
    const char *word = op + 1;
    size_t wlen = (size_t)(close - word);
    bool missing = false; /* :- and the like: unset or empty; - and the like: unset */
    if (v == NULL || (colon && v[0] == '\0')) {
        missing = true;
    }
    if (kind == '%' || kind == '#') {
        const char *pat = expand_text(x, word, wlen, true);
        if (pat != NULL) {
            trim(x, v != NULL ? v : "", pat, kind, twice, in_quotes);
        }
        return after;
    }
    if (kind == '+') {
        if (!missing) {
            const char *w = expand_text(x, word, wlen, false);
            if (w != NULL) {
                put_value(x, w, strlen(w), in_quotes);
            }
        }
        return after;
    }
    if (!missing) {
        put_value(x, v, strlen(v), in_quotes);
        return after;
    }
    const char *w = expand_text(x, word, wlen, false);
    if (w == NULL) {
        return after;
    }
    if (kind == '?') {
        static char said[160];
        (void)snprintf(said, sizeof(said), "%.*s: %s", (int)len, name,
                       w[0] != '\0' ? w : "parameter not set");
        x->err = said;
        return after;
    }
    if (kind == '=') {
        if (!sh_is_name(name, len)) {
            x->err = "Bad substitution: only a name can be set";
            return after;
        }
        if (x->set != NULL && !x->set(x->user, name, len, w, x->why, sizeof(x->why))) {
            x->err = x->why;
            return after;
        }
    }
    put_value(x, w, strlen(w), in_quotes);
    return after;
}

static const char *expand(lexer *x, const char *p, bool in_quotes) {
    if (*p == '(' && p[1] == '(') {
        const char *close = arith_end(p + 2);
        if (close != NULL) {
            return arithmetic(x, p, close, in_quotes);
        }
    }
    if (*p == '(' && x->subst != NULL) {
        return substitute(x, p, in_quotes);
    }
    const char *name = p;
    size_t len = 0;
    if (*p == '{') {
        return braced(x, p + 1, in_quotes);
    }
    if (*p != '\0' && strchr("?#@*-$!0123456789", *p) != NULL) {
        len = 1; /* $10 is $1 and a 0, as sh reads it: ${10} for the tenth */
        p++;
    } else if (name_start(*p)) {
        while (name_char(p[len])) {
            len++;
        }
        p += len;
    } else {
        if (in_quotes) {
            emitq(x, '$');
        } else {
            literal(x, '$');
        }
        return p;
    }
    const char *v = x->look(x->user, name, len);
    if (len == 1 && *name == '@') {
        put_params(x, v, in_quotes);
        return p;
    }
    if (v == NULL && !not_set_ok(x, name, len)) {
        return p;
    }
    put_value(x, v, v != NULL ? strlen(v) : 0, in_quotes);
    return p;
}

/* A double-quoted part at *p (past the "); where it ends, or NULL when it
   does not close. */
static const char *double_quoted(lexer *x, const char *p) {
    if (x->look != NULL && strncmp(p, "$@\"", 3) == 0 && no_params(x)) {
        return p + 3; /* "$@" with no parameters: nothing, not even an empty word */
    }
    quoted(x);
    while (*p != '"') {
        if (*p == '\0') {
            return NULL;
        }
        if (*p == '$' && x->look != NULL) {
            p = expand(x, p + 1, true);
            continue;
        }
        if (*p == '`' && x->subst != NULL) {
            quoted(x);
            p = backquoted(x, p + 1, true);
            continue;
        }
        if (*p == '\\' && p[1] == '\n') {
            p += 2; /* the line goes on */
            continue;
        }
        /* inside double quotes a backslash escapes only these */
        if (*p == '\\' && p[1] != '\0' && strchr("$`\"\\", p[1]) != NULL) {
            p++;
        }
        emitq(x, *p);
        p++;
    }
    return p + 1;
}

/* ~ alone or before a / at the start of a word, outside quotes: the home;
   in an assignment's value too, after the = and after each : (PATH=~/bin:~/x),
   as POSIX expands it. False, nothing done, anywhere else. */
static bool tilde(lexer *x, const char *p) {
    char next = p[1];
    if (x->look == NULL) {
        return false;
    }
    if (x->in_word &&
        (x->as != AS_VALUE || x->o <= x->start || (x->o[-1] != '=' && x->o[-1] != ':'))) {
        return false;
    }
    if (x->in_word && next == ':') {
        next = '\0'; /* ~ alone in the list */
    }
    if (next != '\0' && next != '/' && next != '>' && !blank(next)) {
        return false;
    }
    const char *home = x->look(x->user, "HOME", 4);
    if (home == NULL) {
        return false;
    }
    quoted(x);
    for (; *home != '\0'; home++) {
        emitq(x, *home);
    }
    return true;
}

/* The list operator at p, SH_OP_END when there is none. */
static size_t op_len(int op) {
    if (op == SH_OP_AND || op == SH_OP_OR || op == SH_OP_DSEMI) {
        return 2;
    }
    return 1;
}

static int list_op(const char *p) {
    if (p[0] == ';' && p[1] == ';') {
        return SH_OP_DSEMI;
    }
    if (*p == ';' || *p == '\n') {
        return SH_OP_SEMI;
    }
    if (p[0] == '&' && p[1] == '&') {
        return SH_OP_AND;
    }
    if (p[0] == '|' && p[1] == '|') {
        return SH_OP_OR;
    }
    if (*p == '|') {
        return SH_OP_PIPE;
    }
    if (*p == '&') {
        return SH_OP_BG;
    }
    return SH_OP_END;
}

/* The redirection at *p: < > >> >& and the digit that may come right before
   > (1 the output, 2 the errors), as the word being read. */
static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* <<\x01 flag hex \x01, as sh_heredocs left it: the body decoded and, for
   flag e, expanded as a here-document is; the command's input. */
static bool heredoc(lexer *x, const char **pp, char *why, size_t cap) {
    /* a $(...) in a body with a body of its own, no deeper than roc runs them */
    enum { HERE_DEPTH = ROC_CFG_SUB_DEPTH < 2 ? ROC_CFG_SUB_DEPTH + 1 : 3 };
    static char bodies[HERE_DEPTH][SH_SCRIPT_MAX + 1];
    static sh_line lines[HERE_DEPTH];
    static int depth;
    finish(x);
    if (depth >= HERE_DEPTH) {
        return fail(why, cap, "Too deeply nested");
    }
    int d = depth;
    const char *p = *pp + 3;
    char flag = *p++;
    size_t n = 0;
    while (*p != '\x01') {
        int hi = hex_value(p[0]);
        int lo = hi < 0 ? -1 : hex_value(p[1]);
        if (lo < 0 || n >= SH_SCRIPT_MAX) {
            return fail(why, cap, "Syntax error: a broken here-document");
        }
        bodies[d][n++] = (char)((hi * 16) + lo);
        p += 2;
    }
    bodies[d][n] = '\0';
    *pp = p + 1;
    const char *text = bodies[d];
    if (flag == 'e' && x->look != NULL) {
        sh_line *hl = &lines[d];
        hl->argc = 0;
        hl->nassign = 0;
        const sh_env env = {x->look, x->subst, NULL, x->user, x->set};
        depth++;
        bool ok = read_line(bodies[d], hl, &env, M_HERE, why, cap);
        depth--;
        if (!ok) {
            return false;
        }
        text = hl->argv[0];
        n = strlen(text);
    }
    if (n + 1 > (size_t)(x->end - x->o)) {
        return fail(why, cap, "Line too long");
    }
    memcpy(x->o, text, n);
    x->o[n] = '\0';
    x->l->here = x->o;
    x->l->here_len = n;
    x->l->in = NULL; /* the last of them wins */
    x->o += n + 1;
    return true;
}

static bool redirection(lexer *x, const char **pp, char *why, size_t cap) {
    const char *p = *pp;
    if (p[0] == '<' && p[1] == '<' && p[2] == '\x01') {
        return heredoc(x, pp, why, cap);
    }
    int fd = *p == '<' ? 0 : 1;
    if (*p == '>' && x->in_word && x->plain && x->o == x->start + 1 &&
        (*x->start == '1' || *x->start == '2')) {
        fd = *x->start - '0'; /* not a word: what the > sends */
        x->in_word = false;
        x->o = x->start;
    } else {
        finish(x);
    }
    if (x->redirect != R_NONE) {
        (void)snprintf(why, cap, "Syntax error: \"%c\" unexpected", *p);
        return false;
    }
    if (*p == '<') {
        x->redirect = R_IN;
        *pp = p + 1;
        return true;
    }
    if (p[1] == '&') { /* 2>&1, >&2; 1>&1 and 2>&2 change nothing */
        if ((p[2] != '1' && p[2] != '2') || (p[3] >= '0' && p[3] <= '9')) {
            return fail(why, cap, "Syntax error: Bad fd number");
        }
        if (fd == 2 && p[2] == '1') {
            x->l->err_to_out = true;
        } else if (fd == 1 && p[2] == '2') {
            x->l->out_to_err = true;
        }
        *pp = p + 3;
        return true;
    }
    bool twice = p[1] == '>';
    if (fd == 2) {
        x->l->err_append = twice;
        x->redirect = R_ERR;
    } else {
        x->l->append = twice;
        x->redirect = R_OUT;
    }
    *pp = p + (twice ? 2 : 1);
    return true;
}

/* Where the words of l end in its buffer: the next one goes there. */
static char *words_end(sh_line *l) {
    if (l->argc == 0) {
        return l->buf;
    }
    const char *w = l->argv[l->argc - 1];
    return l->buf + (w - l->buf) + strlen(w) + 1;
}

/* M_WORD and M_PATTERN add one word to l's words, "" for none; a pattern
   is the word as a pattern: its quoted * ? [ ] \ escaped. */
static bool one_word(lexer *x, int argc_before) {
    sh_line *l = x->l;
    if (l->argc == argc_before) {
        char *o = words_end(l);
        if (l->argc == SH_WORDS_MAX || o + 1 > x->end) {
            x->err = "Line too long";
            return false;
        }
        *o = '\0';
        l->argv[l->argc++] = o;
        x->patlen = 0;
    }
    if (x->mode != M_PATTERN && x->mode != M_TEXT_PATTERN) {
        return true;
    }
    char *o = words_end(l);
    if (l->argc == 0 || x->pat_full || o + x->patlen + 1 > x->end) {
        x->err = "Line too long";
        return false;
    }
    memcpy(o, x->pat, x->patlen);
    o[x->patlen] = '\0';
    l->argv[l->argc - 1] = o;
    return true;
}

static bool assign_as_read; /* sh_expand_assigning's, for the next read_line */

static bool read_line(const char *line, sh_line *l, const sh_env *env, int mode, char *why,
                      size_t cap) {
    sh_lookup look = env->look;
    if (mode == M_LINE) {
        l->argc = 0;
        l->nassign = 0;
    }
    int argc_before = l->argc;
    l->argv[l->argc] = NULL;
    l->out = NULL;
    l->append = false;
    l->in = NULL;
    l->here = NULL;
    l->here_len = 0;
    l->err = NULL;
    l->err_append = false;
    l->err_to_out = false;
    l->out_to_err = false;
    l->op = SH_OP_END;
    l->next = strlen(line);
    if (strlen(line) > SH_SCRIPT_MAX) {
        return fail(why, cap, "Line too long");
    }
    lexer x = {
        .l = l,
        .look = env->look,
        .subst = env->subst,
        .glob = mode == M_LINE ? env->glob : NULL,
        .user = env->user,
        .set = env->set,
        .o = words_end(l),
        .end = l->buf + sizeof(l->buf),
        .start = l->buf,
        .redirect = R_NONE,
        .as = AS_NOT,
        .mode = mode,
        .assign_now = assign_as_read,
    };
    assign_as_read = false; /* this line's only, not a $(...)'s inside it */
    const char *p = line;
    while (*p != '\0' && x.err == NULL) {
        char c = *p;
        if (mode == M_HERE) { /* a here-document: only $ ` and \ before them */
            if (c == '$') {
                p = expand(&x, p + 1, true);
            } else if (c == '`' && x.subst != NULL) {
                quoted(&x);
                p = backquoted(&x, p + 1, true);
            } else if (c == '\\' && p[1] != '\0' && strchr("$`\\\n", p[1]) != NULL) {
                if (p[1] != '\n') {
                    emitq(&x, p[1]);
                }
                p += 2;
            } else {
                emitq(&x, c);
                p++;
            }
            continue;
        }
        bool text = false;
        if (mode == M_TEXT || mode == M_TEXT_PATTERN) {
            text = true;
        }
        if (text && (blank(c) || strchr("\n#;&|()<>", c) != NULL)) {
            literal(&x, c); /* the word of ${x:-a b}: all one text */
            p++;
            continue;
        }
        if (mode != M_LINE && strchr(" \t\n;&|()<>", c) != NULL) {
            l->next = (size_t)(p - line); /* the word of case ends here */
            break;
        }
        if (blank(c)) {
            finish(&x);
            p++;
        } else if (c == '#' && !x.in_word) {
            while (*p != '\0' && *p != '\n') {
                p++; /* a comment ends with its line */
            }
            if (*p == '\n' && look != NULL) {
                l->op = SH_OP_SEMI;
                l->next = (size_t)(p - line) + 1U;
            }
            break;
        } else if (c == ')' && look != NULL) {
            l->op = SH_OP_SEMI; /* the command ends; the ) closes a subshell */
            l->next = (size_t)(p - line);
            break;
        } else if (c == '(' && look != NULL) {
            return fail(why, cap, "Syntax error: \"(\" unexpected");
        } else if (look != NULL && list_op(p) != SH_OP_END) {
            l->op = list_op(p);
            l->next = (size_t)(p - line) + op_len(l->op);
            break;
        } else if (c == '>' || c == '<') {
            if (!redirection(&x, &p, why, cap)) {
                return false;
            }
        } else if (c == '\\' && p[1] == '\n') {
            p += 2; /* the line goes on in the next */
        } else if (c == '\\' && p[1] == '\0' && look != NULL) {
            return fail(why, cap, "Syntax error: end of file unexpected");
        } else if (c == '\\') {
            quoted(&x);
            if (p[1] == '\0') {
                emitq(&x, '\\');
                p++;
            } else {
                emitq(&x, p[1]);
                p += 2;
            }
        } else if (c == '\'') {
            quoted(&x);
            const char *close = strchr(p + 1, '\'');
            if (close == NULL) {
                return fail(why, cap, "Syntax error: Unterminated quoted string");
            }
            for (p++; p < close; p++) {
                emitq(&x, *p);
            }
            p++;
        } else if (c == '"') {
            p = double_quoted(&x, p + 1);
            if (p == NULL) {
                return fail(why, cap, "Syntax error: Unterminated quoted string");
            }
        } else if (c == '$' && look != NULL) {
            p = expand(&x, p + 1, false);
        } else if (c == '`' && x.subst != NULL) {
            p = backquoted(&x, p + 1, false);
        } else if (c == '~' && tilde(&x, p)) {
            p++;
        } else {
            literal(&x, c);
            p++;
        }
    }
    finish(&x);
    if (mode != M_LINE && x.err == NULL) {
        (void)one_word(&x, argc_before);
    }
    if (x.err != NULL) {
        return fail(why, cap, x.err);
    }
    if (x.redirect != R_NONE) {
        return fail(why, cap, "Syntax error: newline unexpected");
    }
    l->argv[l->argc] = NULL;
    return true;
}

/* A value for every name, so a word made of an expansion is still a word
   when a line is only being checked. */
static const char *anything(void *user, const char *name, size_t len) {
    (void)user;
    if (len == 3 && memcmp(name, "IFS", 3) == 0) {
        return NULL;
    }
    return "x";
}

enum { CHECK_DEPTH = ROC_CFG_SUB_DEPTH + 1 }; /* the line and the $( inside $( roc runs */

static bool check_at(const char *line, int depth, char *why, size_t cap);

/* $(command) while a line is only being checked: the command read, not
   run. */
/* cppcheck-suppress constParameterCallback ; sh_subst's shape */
static const char *check_subst(void *user, const char *cmd, size_t len, size_t *out_len, char *why,
                               size_t cap) {
    static char inner[CHECK_DEPTH][SH_LINE_MAX + 1];
    int depth = *(const int *)user + 1;
    if (depth >= CHECK_DEPTH || len > SH_LINE_MAX) {
        (void)fail(why, cap, "Too deeply nested");
        return NULL;
    }
    memcpy(inner[depth], cmd, len);
    inner[depth][len] = '\0';
    if (!check_at(inner[depth], depth, why, cap)) {
        return NULL;
    }
    *out_len = 1;
    return "x";
}

size_t sh_blanks(const char *p) {
    size_t n = 0;
    for (;;) {
        if (p[n] == ' ' || p[n] == '\t' || p[n] == '\n') {
            n++;
        } else if (p[n] == '#') {
            while (p[n] != '\0' && p[n] != '\n') {
                n++;
            }
        } else {
            return n;
        }
    }
}

int sh_reserved(const char *p, size_t *len) {
    if (*p == '(' || *p == ')') {
        *len = 1;
        return *p == '(' ? SH_RW_LPAREN : SH_RW_RPAREN;
    }
    static const char *const words[] = {
        "",     "if",    "then",  "elif", "else", "fi", "for", "do",
        "done", "while", "until", "case", "esac", "{",  "}",
    };
    for (int i = 1; i < (int)(sizeof(words) / sizeof(words[0])); i++) {
        size_t n = strlen(words[i]);
        if (strncmp(p, words[i], n) == 0 &&
            (p[n] == '\0' || strchr(" \t\n;&|<>()", p[n]) != NULL)) {
            *len = n;
            return i;
        }
    }
    *len = 0;
    return SH_RW_NONE;
}

static size_t spaces(const char *p);

bool sh_opens(int rw) {
    switch (rw) {
    case SH_RW_IF:
    case SH_RW_FOR:
    case SH_RW_WHILE:
    case SH_RW_UNTIL:
    case SH_RW_CASE:
    case SH_RW_LBRACE:
    case SH_RW_LPAREN:
        return true;
    default:
        return false;
    }
}

int sh_closer(int rw) {
    switch (rw) {
    case SH_RW_IF:
        return SH_RW_FI;
    case SH_RW_CASE:
        return SH_RW_ESAC;
    case SH_RW_LBRACE:
        return SH_RW_RBRACE;
    case SH_RW_LPAREN:
        return SH_RW_RPAREN;
    default:
        return SH_RW_DONE;
    }
}

size_t sh_fdef(const char *p, size_t *name_len) {
    size_t n = 0;
    while (name_char(p[n]) && (n > 0 || name_start(p[n]))) {
        n++;
    }
    if (n == 0) {
        return 0;
    }
    size_t k = n + spaces(p + n);
    if (p[k] != '(') {
        return 0;
    }
    k++;
    k += spaces(p + k);
    if (p[k] != ')') {
        return 0;
    }
    *name_len = n;
    return k + 1;
}

bool sh_after_close(const char *p, sh_line *l, const sh_env *env, char *why, size_t cap) {
    if (!sh_expand(p, l, env, why, cap)) {
        return false;
    }
    if (l->argc > 0 || l->nassign > 0) {
        return fail(why, cap, "Syntax error: word unexpected");
    }
    return true;
}

bool sh_match(const char *pat, const char *name) {
    while (*pat != '\0') {
        char c = *pat;
        if (c == '*') {
            while (*pat == '*') {
                pat++;
            }
            if (*pat == '\0') {
                return true;
            }
            for (const char *n = name; *n != '\0'; n++) {
                if (sh_match(pat, n)) {
                    return true;
                }
            }
            return false;
        }
        if (*name == '\0') {
            return false;
        }
        if (c == '?') {
            pat++;
            name++;
            continue;
        }
        if (c == '[') {
            const char *p = pat + 1;
            bool negate = false;
            if (*p == '!' || *p == '^') {
                negate = true;
                p++;
            }
            bool hit = false;
            bool first = true;
            while (*p != '\0' && (first || *p != ']')) {
                first = false;
                char lo = *p == '\\' && p[1] != '\0' ? *++p : *p;
                char hi = lo;
                if (p[1] == '-' && p[2] != ']' && p[2] != '\0') {
                    p += 2;
                    hi = *p == '\\' && p[1] != '\0' ? *++p : *p;
                }
                if ((unsigned char)*name >= (unsigned char)lo &&
                    (unsigned char)*name <= (unsigned char)hi) {
                    hit = true;
                }
                p++;
            }
            if (*p != ']') { /* no ]: the [ is itself */
                if (*name != '[') {
                    return false;
                }
                pat++;
                name++;
                continue;
            }
            if (hit == negate) {
                return false;
            }
            pat = p + 1;
            name++;
            continue;
        }
        if (c == '\\' && pat[1] != '\0') {
            pat++;
            c = *pat;
        }
        if (c != *name) {
            return false;
        }
        pat++;
        name++;
    }
    return *name == '\0';
}

static size_t spaces(const char *p) {
    size_t n = 0;
    while (blank(p[n])) {
        n++;
    }
    return n;
}

bool sh_case_head(const char *p, sh_line *l, const sh_env *env, size_t *len, char *why,
                  size_t cap) {
    l->argc = 0;
    l->nassign = 0;
    size_t n = spaces(p);
    if (strchr("\n;&|()<>", p[n]) != NULL) { /* also the end of the text */
        return fail(why, cap, "Syntax error: word unexpected (expecting a word after case)");
    }
    if (!read_line(p + n, l, env, M_WORD, why, cap)) {
        return false;
    }
    n += l->next;
    n += sh_blanks(p + n); /* in may come on a line of its own */
    if (strncmp(p + n, "in", 2) != 0 ||
        (p[n + 2] != '\0' && strchr(" \t\n;&|<>()", p[n + 2]) == NULL)) {
        return fail(why, cap, "Syntax error: word unexpected (expecting \"in\")");
    }
    *len = n + 2;
    return true;
}

bool sh_case_patterns(const char *p, sh_line *l, const sh_env *env, size_t *len, char *why,
                      size_t cap) {
    l->argc = 0;
    l->nassign = 0;
    size_t n = spaces(p);
    if (p[n] == '(') {
        n++;
    }
    for (;;) {
        n += spaces(p + n);
        if (strchr("\n;&|()<>", p[n]) != NULL) {
            return fail(why, cap, "Syntax error: word unexpected (expecting a pattern)");
        }
        if (!read_line(p + n, l, env, M_PATTERN, why, cap)) {
            return false;
        }
        n += l->next;
        n += spaces(p + n);
        if (p[n] == ')') {
            *len = n + 1;
            return true;
        }
        if (p[n] != '|' || p[n + 1] == '|') {
            return fail(why, cap, "Syntax error: word unexpected (expecting \")\")");
        }
        n++;
    }
}

size_t sh_for_do(const char *p) {
    size_t n = spaces(p);
    size_t name = n;
    while (name_char(p[n]) && (n > name || name_start(p[n]))) {
        n++;
    }
    if (n == name) {
        return 0;
    }
    n += sh_blanks(p + n);
    if (!word_at(p + n, "do")) {
        return 0;
    }
    return n;
}

static int skip_depth; /* check_subst's depth, for reading only */

bool sh_skip(const char *text, size_t pos, unsigned targets, int depth, size_t *at, int *found) {
    static sh_line l;
    const sh_env env = {anything, check_subst, NULL, &skip_depth, NULL};
    char why[96];
    bool patterns = false; /* after case's in, or a ;;: a branch's patterns first */
    for (;;) {
        pos += sh_blanks(text + pos);
        pos += sh_bang(text + pos);
        if (text[pos] == '\0') {
            return false;
        }
        size_t len = 0;
        int rw = sh_reserved(text + pos, &len);
        if (patterns && rw != SH_RW_ESAC) {
            patterns = false;
            if (!sh_case_patterns(text + pos, &l, &env, &len, why, sizeof(why))) {
                return false;
            }
            pos += len;
            continue;
        }
        patterns = false;
        int op = list_op(text + pos);
        if (op == SH_OP_DSEMI && depth == 0 && (targets & (1U << (unsigned)SH_RW_DSEMI)) != 0) {
            *at = pos + 2;
            *found = SH_RW_DSEMI;
            return true;
        }
        if (op != SH_OP_END) { /* what a closer left behind */
            pos += op_len(op);
            patterns = op == SH_OP_DSEMI;
            continue;
        }
        size_t name_len = 0;
        size_t fdef = sh_fdef(text + pos, &name_len);
        if (fdef > 0) {
            pos += fdef; /* NAME(): its body is a block like any */
            continue;
        }
        if (rw != SH_RW_NONE) {
            if (depth == 0 && (targets & (1U << (unsigned)rw)) != 0) {
                *at = pos;
                *found = rw;
                return true;
            }
            pos += len;
            if (sh_opens(rw)) {
                depth++;
            } else if (rw == SH_RW_FI || rw == SH_RW_DONE || rw == SH_RW_ESAC ||
                       rw == SH_RW_RBRACE || rw == SH_RW_RPAREN) {
                if (depth == 0) {
                    return false;
                }
                depth--;
            }
            if (rw == SH_RW_CASE) {
                if (!sh_case_head(text + pos, &l, &env, &len, why, sizeof(why))) {
                    return false;
                }
                pos += len;
                patterns = true;
            }
            if (rw == SH_RW_FOR) {
                pos += sh_for_do(text + pos); /* for name do: on at the do */
            }
            continue;
        }
        if (!sh_expand(text + pos, &l, &env, why, sizeof(why))) {
            return false;
        }
        pos += l.next;
        if (l.op == SH_OP_DSEMI) {
            if (depth == 0 && (targets & (1U << (unsigned)SH_RW_DSEMI)) != 0) {
                *at = pos;
                *found = SH_RW_DSEMI;
                return true;
            }
            patterns = true;
        }
    }
}

/* A block of the checker: which word opened it, and where in it the reading
   is: the condition, the body, else's body, for's name and words, or the
   do they wait for. */
enum { B_COND, B_BODY, B_ELSE, B_HEAD, B_DO, B_PATS };
typedef struct {
    int rw;
    int state;
    int cmds; /* commands so far in the part being read */
} block;

static const char *const rw_names[] = {
    "",      "if",   "then", "elif", "else", "fi", "for", "do", "done", "while",
    "until", "case", "esac", "{",    "}",    ";;", "",    "(",  ")",
};

/* A block closed: in the one around it, it counts as a command. */
static bool close_block(block *st, int *n) {
    (*n)--;
    if (*n > 0) {
        st[*n - 1].cmds++;
    }
    return true;
}

/* A reserved word against the blocks open: the next state, or why not. */
static bool check_word(block *st, int *n, int rw, char *why, size_t cap) {
    block *top = *n > 0 ? &st[*n - 1] : NULL;
    bool ok = false;
    if (sh_opens(rw)) {
        if (*n == SH_NEST_MAX) {
            return fail(why, cap, "Too deeply nested");
        }
        st[*n].rw = rw;
        st[*n].state = B_COND;
        if (rw == SH_RW_FOR) {
            st[*n].state = B_HEAD;
        } else if (rw == SH_RW_CASE) {
            st[*n].state = B_PATS;
        } else if (rw == SH_RW_LBRACE || rw == SH_RW_LPAREN) {
            st[*n].state = B_BODY;
        }
        st[*n].cmds = 0;
        (*n)++;
        return true;
    }
    if (rw == SH_RW_ESAC && top != NULL && top->rw == SH_RW_CASE) {
        return close_block(st, n); /* a branch may be empty, and so may case */
    }
    if (top != NULL && top->cmds > 0 && top->rw != SH_RW_CASE) {
        bool is_if = top->rw == SH_RW_IF;
        bool closes = false;
        if (rw == SH_RW_FI && is_if && (top->state == B_BODY || top->state == B_ELSE)) {
            closes = true;
        }
        if (rw == SH_RW_DONE && sh_closer(top->rw) == SH_RW_DONE && top->state == B_BODY) {
            closes = true;
        }
        if (rw == SH_RW_RBRACE && top->rw == SH_RW_LBRACE) {
            closes = true;
        }
        if (rw == SH_RW_RPAREN && top->rw == SH_RW_LPAREN) {
            closes = true;
        }
        if (closes) {
            return close_block(st, n);
        }
        /* then after if's condition, do after while's or until's */
        if (top->state == B_COND && ((rw == SH_RW_THEN && is_if) || (rw == SH_RW_DO && !is_if))) {
            top->state = B_BODY;
            ok = true;
        } else if ((rw == SH_RW_ELIF || rw == SH_RW_ELSE) && is_if && top->state == B_BODY) {
            top->state = rw == SH_RW_ELIF ? B_COND : B_ELSE;
            ok = true;
        }
    }
    if (rw == SH_RW_DO && top != NULL && top->rw == SH_RW_FOR && top->state == B_DO) {
        top->state = B_BODY;
        ok = true;
    }
    if (!ok) {
        (void)snprintf(why, cap, "Syntax error: \"%s\" unexpected", rw_names[rw]);
        return false;
    }
    top->cmds = 0;
    return true;
}

/* for's head, read as a command: a name, and in and the words or nothing. */
static bool check_head(const sh_line *l, char *why, size_t cap) {
    if (l->argc == 0 || !sh_is_name(l->argv[0], strlen(l->argv[0])) ||
        (l->argc > 1 && strcmp(l->argv[1], "in") != 0)) {
        return fail(why, cap, "Syntax error: Bad for loop variable");
    }
    if (l->op != SH_OP_SEMI) {
        return fail(why, cap, "Syntax error: for wants ; or a newline before do");
    }
    return true;
}

/* A ;; : the end of a branch of the case around it, patterns next. */
static bool branch_end(block *st, int n, char *why, size_t cap) {
    if (n == 0 || st[n - 1].rw != SH_RW_CASE || st[n - 1].state != B_BODY) {
        return fail(why, cap, "Syntax error: \";;\" unexpected");
    }
    st[n - 1].state = B_PATS;
    return true;
}

static bool check_at(const char *line, int depth, char *why, size_t cap) {
    static sh_line lines[CHECK_DEPTH];
    static block blocks[CHECK_DEPTH][SH_NEST_MAX];
    static const char *const shown[] = {"", ";", "&&", "||", "|", ";;", "&"};
    sh_line *l = &lines[depth];
    block *st = blocks[depth];
    int n = 0;
    const sh_env env = {anything, check_subst, NULL, &depth, NULL};
    size_t pos = 0;
    int before = SH_OP_SEMI; /* the operator before what is at pos */
    bool body_due = false;   /* NAME() read: its block next */
    for (;;) {
        pos += sh_blanks(line + pos);
        pos += sh_bang(line + pos);
        if (line[pos] == '\0') {
            break;
        }
        size_t len = 0;
        int rw = sh_reserved(line + pos, &len);
        if (n > 0 && st[n - 1].state == B_PATS && rw != SH_RW_ESAC) {
            if (!sh_case_patterns(line + pos, l, &env, &len, why, cap)) {
                return false;
            }
            pos += len;
            st[n - 1].state = B_BODY;
            before = SH_OP_SEMI;
            continue;
        }
        if (body_due && !sh_opens(rw)) {
            return fail(why, cap,
                        "Syntax error: a function's body is { }, ( ), if, while, for or case");
        }
        body_due = false;
        size_t name_len = 0;
        size_t fdef = sh_fdef(line + pos, &name_len);
        if (fdef > 0 && rw == SH_RW_NONE && !(n > 0 && st[n - 1].state == B_HEAD)) {
            if (before == SH_OP_PIPE) {
                return fail(why, cap, "Syntax error: a pipe into a function's definition");
            }
            pos += fdef;
            body_due = true;
            continue;
        }
        if (rw != SH_RW_NONE && !(n > 0 && st[n - 1].state == B_HEAD)) {
            if (!check_word(st, &n, rw, why, cap)) {
                return false;
            }
            pos += len;
            before = SH_OP_SEMI;
            if (rw == SH_RW_CASE) {
                if (!sh_case_head(line + pos, l, &env, &len, why, cap)) {
                    return false;
                }
                pos += len;
            }
            size_t for_do = rw == SH_RW_FOR ? sh_for_do(line + pos) : 0;
            if (for_do > 0) { /* for name do: the parameters, and do next */
                pos += for_do;
                st[n - 1].state = B_DO;
            }
            if (rw == SH_RW_FI || rw == SH_RW_DONE || rw == SH_RW_ESAC || rw == SH_RW_RBRACE ||
                rw == SH_RW_RPAREN) {
                if (!sh_after_close(line + pos, l, &env, why, cap)) {
                    return false;
                }
                pos += l->next;
                before = l->op == SH_OP_END ? SH_OP_SEMI : l->op;
                if (before == SH_OP_DSEMI && !branch_end(st, n, why, cap)) {
                    return false;
                }
            }
            continue;
        }
        if (!sh_expand(line + pos, l, &env, why, cap)) {
            return false;
        }
        bool empty = false; /* no command: only an operator where one was due */
        if (l->argc == 0 && l->nassign == 0 && l->out == NULL && l->in == NULL && l->err == NULL &&
            !l->err_to_out && !l->out_to_err) {
            empty = true;
        }
        bool owed = false; /* a && || | before it wants a command */
        if (before == SH_OP_AND || before == SH_OP_OR || before == SH_OP_PIPE) {
            owed = true;
        }
        if (empty && l->op != SH_OP_END && (l->op != SH_OP_DSEMI || owed)) {
            (void)snprintf(why, cap, "Syntax error: \"%s\" unexpected", shown[l->op]);
            return false;
        }
        if (l->op == SH_OP_DSEMI && !branch_end(st, n, why, cap)) {
            return false;
        }
        if (n > 0 && st[n - 1].state == B_HEAD) {
            if (!check_head(l, why, cap)) {
                return false;
            }
            st[n - 1].state = B_DO;
        } else if (n > 0 && !empty) {
            st[n - 1].cmds++;
        }
        if (l->op == SH_OP_END) {
            if (!empty) {
                before = SH_OP_SEMI; /* the last command: nothing left owed */
            }
            break;
        }
        pos += l->next;
        before = l->op == SH_OP_DSEMI ? SH_OP_SEMI : l->op;
    }
    if (before == SH_OP_AND || before == SH_OP_OR || before == SH_OP_PIPE || body_due) {
        return fail(why, cap, "Syntax error: end of file unexpected");
    }
    if (n > 0) {
        const block *top = &st[n - 1];
        const char *want = "done";
        if (top->rw == SH_RW_IF) {
            want = top->state == B_COND ? "then" : "fi";
        } else if (top->rw == SH_RW_CASE) {
            want = "esac";
        } else if (top->rw == SH_RW_LBRACE) {
            want = "}";
        } else if (top->rw == SH_RW_LPAREN) {
            want = ")";
        } else if (top->state != B_BODY) {
            want = "do";
        }
        (void)snprintf(why, cap, "Syntax error: end of file unexpected (expecting \"%s\")", want);
        return false;
    }
    return true;
}

/* The word after << at p: the delimiter, its quotes taken off (*was_quoted when
   there were any), and where it ends. */
static const char *here_word(const char *p, char *word, size_t cap, bool *was_quoted) {
    size_t n = 0;
    *was_quoted = false;
    while (*p != '\0' && !blank(*p) && strchr("\n;&|<>()", *p) == NULL) {
        char c = *p++;
        if (c == '\'' || c == '"') {
            *was_quoted = true;
            while (*p != '\0' && *p != c) {
                if (n + 1 < cap) {
                    word[n++] = *p;
                }
                p++;
            }
            if (*p == c) {
                p++;
            }
            continue;
        }
        if (c == '\\' && *p != '\0') {
            *was_quoted = true;
            c = *p++;
        }
        if (n + 1 < cap) {
            word[n++] = c;
        }
    }
    word[n] = '\0';
    return p;
}

bool sh_heredocs(const char *text, char *out, size_t cap, char *why, size_t whycap) {
    enum { PENDING_MAX = 8 };
    struct {
        char word[64];
        bool strip; /* <<-: the tabs a line starts with go */
        bool quoted;
        size_t at; /* where in out its token goes */
    } pending[PENDING_MAX];
    int np = 0;
    size_t o = 0;
    const char *p = text;
    bool word_start = true;
    static const char hexd[] = "0123456789abcdef";
    while (*p != '\0') {
        const char *from = p;
        char c = *p;
        if (c == '\\' && p[1] != '\0') {
            p += 2;
        } else if (c == '\'' || c == '"') {
            p++;
            while (*p != '\0' && *p != c) {
                p += c == '"' && *p == '\\' && p[1] != '\0' ? 2 : 1;
            }
            if (*p == c) {
                p++;
            }
        } else if (c == '#' && word_start) {
            while (*p != '\0' && *p != '\n') {
                p++;
            }
        } else if (c == '$' && p[1] == '(' && p[2] == '(' && arith_end(p + 3) != NULL) {
            p = arith_end(p + 3) + 2; /* 1 << 2 is no here-document */
        } else if (c == '<' && p[1] == '<') {
            if (np == PENDING_MAX) {
                return fail(why, whycap, "Too many here-documents on a line");
            }
            p += 2;
            pending[np].strip = *p == '-';
            if (pending[np].strip) {
                p++;
            }
            while (blank(*p)) {
                p++;
            }
            p = here_word(p, pending[np].word, sizeof(pending[np].word), &pending[np].quoted);
            if (pending[np].word[0] == '\0' && !pending[np].quoted) {
                return fail(why, whycap, "Syntax error: newline unexpected");
            }
            if (o + 2 > cap) {
                return fail(why, whycap, "Line too long");
            }
            out[o++] = '<';
            out[o++] = '<';
            pending[np].at = o;
            np++;
            word_start = false;
            continue;
        } else if (c == '\n' && np > 0) {
            p++;
            if (o + 1 > cap) {
                return fail(why, whycap, "Line too long");
            }
            out[o++] = '\n';
            size_t shift = 0; /* the tokens put before, pushing the rest on */
            for (int i = 0; i < np; i++) {
                static char body[SH_SCRIPT_MAX];
                size_t bn = 0;
                bool ended = false;
                while (*p != '\0') {
                    const char *line = p;
                    const char *end = strchr(p, '\n');
                    if (end == NULL) {
                        end = p + strlen(p);
                    }
                    p = *end == '\n' ? end + 1 : end;
                    if (pending[i].strip) {
                        while (line < end && *line == '\t') {
                            line++;
                        }
                    }
                    size_t ln = (size_t)(end - line);
                    if (ln == strlen(pending[i].word) && strncmp(line, pending[i].word, ln) == 0) {
                        ended = true;
                        break;
                    }
                    if (bn + ln + 1 > sizeof(body)) {
                        return fail(why, whycap, "Here-document too long");
                    }
                    memcpy(body + bn, line, ln);
                    bn += ln;
                    body[bn++] = '\n';
                }
                if (!ended) {
                    (void)snprintf(why, whycap,
                                   "Syntax error: end of file unexpected (expecting \"%s\")",
                                   pending[i].word);
                    return false;
                }
                size_t tn = 2 + (2 * bn) + 1; /* \x01 flag hex \x01 */
                if (o + tn > cap) {
                    return fail(why, whycap, "Line too long");
                }
                size_t at = pending[i].at + shift;
                memmove(out + at + tn, out + at, o - at);
                out[at] = '\x01';
                out[at + 1] = pending[i].quoted ? 'q' : 'e';
                for (size_t k = 0; k < bn; k++) {
                    out[at + 2 + (2 * k)] = hexd[(unsigned char)body[k] >> 4U];
                    out[at + 3 + (2 * k)] = hexd[(unsigned char)body[k] & 15U];
                }
                out[at + 2 + (2 * bn)] = '\x01';
                o += tn;
                shift += tn;
            }
            np = 0;
            word_start = true;
            continue;
        } else {
            p++;
        }
        size_t n = (size_t)(p - from);
        if (o + n > cap) {
            return fail(why, whycap, "Line too long");
        }
        memcpy(out + o, from, n);
        o += n;
        word_start = false;
        if (blank(c) || strchr("\n;&|()<>", c) != NULL) {
            word_start = true;
        }
    }
    if (np > 0) {
        (void)snprintf(why, whycap, "Syntax error: end of file unexpected (expecting \"%s\")",
                       pending[0].word);
        return false;
    }
    if (o + 1 > cap) {
        return fail(why, whycap, "Line too long");
    }
    out[o] = '\0';
    return true;
}

bool sh_check(const char *line, char *why, size_t cap) {
    return check_at(line, 0, why, cap);
}

bool sh_needs_more(const char *why) {
    if (strncmp(why, "Syntax error: end of file unexpected", 36) == 0) {
        return true;
    }
    return strcmp(why, "Syntax error: Unterminated quoted string") == 0;
}

bool sh_assigns_only(const char *line) {
    static sh_line l;
    static int depth;
    size_t n = 0;
    while (name_char(line[n]) && (n > 0 || name_start(line[n]))) {
        n++;
    }
    if (n == 0 || line[n] != '=') {
        return false; /* the usual case, at no cost */
    }
    const sh_env env = {anything, check_subst, NULL, &depth, NULL};
    char why[96];
    if (!read_line(line, &l, &env, M_LINE, why, sizeof(why))) {
        return false;
    }
    if (l.argc > 0) {
        return false;
    }
    return l.nassign > 1;
}

bool sh_expand_assigning(const char *line, sh_line *l, const sh_env *env, char *why, size_t cap) {
    assign_as_read = true;
    return read_line(line, l, env, M_LINE, why, cap);
}

bool sh_split(const char *line, sh_line *l, char *why, size_t cap) {
    const sh_env env = {NULL, NULL, NULL, NULL, NULL};
    return read_line(line, l, &env, M_LINE, why, cap);
}

bool sh_read(const char *line, sh_line *l, sh_lookup look, void *user, char *why, size_t cap) {
    const sh_env env = {look, NULL, NULL, user, NULL};
    return read_line(line, l, &env, M_LINE, why, cap);
}

bool sh_expand(const char *line, sh_line *l, const sh_env *env, char *why, size_t cap) {
    return read_line(line, l, env, M_LINE, why, cap);
}

/* A word sh reads as it is: nothing it would split at, quote, expand or
   take for an operator or a comment. */
static bool plain(const char *w) {
    if (*w == '\0') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)w; *p != '\0'; p++) {
        unsigned char c = *p;
        if (c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            continue;
        }
        if (strchr("_-./~:=+@%,^", c) == NULL) {
            return false;
        }
    }
    return true;
}

static bool put(char *dst, size_t cap, size_t *at, char c) {
    if (*at + 1 >= cap) {
        return false;
    }
    dst[*at] = c;
    (*at)++;
    dst[*at] = '\0';
    return true;
}

bool sh_join(char *const *argv, int argc, char *dst, size_t cap) {
    size_t at = 0;
    if (cap == 0) {
        return false;
    }
    dst[0] = '\0';
    for (int i = 0; i < argc; i++) {
        if (i > 0 && !put(dst, cap, &at, ' ')) {
            return false;
        }
        const char *w = argv[i];
        if (plain(w)) {
            for (; *w != '\0'; w++) {
                if (!put(dst, cap, &at, *w)) {
                    return false;
                }
            }
            continue;
        }
        if (!put(dst, cap, &at, '\'')) {
            return false;
        }
        for (; *w != '\0'; w++) {
            if (*w != '\'') {
                if (!put(dst, cap, &at, *w)) {
                    return false;
                }
                continue;
            }
            for (const char *q = "'\\''"; *q != '\0'; q++) {
                if (!put(dst, cap, &at, *q)) {
                    return false;
                }
            }
        }
        if (!put(dst, cap, &at, '\'')) {
            return false;
        }
    }
    return true;
}

const char *sh_var_get(const sh_vars *vs, const char *name, size_t len) {
    for (int i = 0; i < vs->n; i++) {
        if (strlen(vs->v[i].name) == len && memcmp(vs->v[i].name, name, len) == 0) {
            return vs->v[i].value;
        }
    }
    return NULL;
}

bool sh_var_assign(sh_vars *vs, const char *word, char *why, size_t cap) {
    const char *eq = strchr(word, '=');
    if (eq == NULL || !sh_is_name(word, (size_t)(eq - word))) {
        return fail(why, cap, "not an assignment");
    }
    size_t len = (size_t)(eq - word);
    if (len >= SH_NAME_MAX) {
        return fail(why, cap, "name too long");
    }
    if (strlen(eq + 1) >= SH_VALUE_MAX) {
        return fail(why, cap, "value too long");
    }
    int i = 0;
    while (i < vs->n && !(strlen(vs->v[i].name) == len && memcmp(vs->v[i].name, word, len) == 0)) {
        i++;
    }
    if (i >= SH_VARS_MAX) {
        return fail(why, cap, "too many variables");
    }
    if (i < vs->n && vs->v[i].held) {
        return fail(why, cap, "readonly variable");
    }
    if (i == vs->n) {
        memcpy(vs->v[i].name, word, len);
        vs->v[i].name[len] = '\0';
        vs->v[i].held = false;
        vs->v[i].exported = false;
        vs->n++;
    }
    memcpy(vs->v[i].value, eq + 1, strlen(eq + 1) + 1);
    return true;
}

bool sh_var_held(const sh_vars *vs, const char *name) {
    for (int i = 0; i < vs->n; i++) {
        if (strcmp(vs->v[i].name, name) == 0) {
            return vs->v[i].held;
        }
    }
    return false;
}

bool sh_var_hold(sh_vars *vs, const char *name, char *why, size_t cap) {
    if (sh_var_get(vs, name, strlen(name)) == NULL) {
        char word[SH_NAME_MAX + 1];
        (void)snprintf(word, sizeof(word), "%s=", name);
        if (!sh_var_assign(vs, word, why, cap)) {
            return false;
        }
    }
    for (int i = 0; i < vs->n; i++) {
        if (strcmp(vs->v[i].name, name) == 0) {
            vs->v[i].held = true;
        }
    }
    return true;
}

bool sh_var_export(sh_vars *vs, const char *name, bool on, char *why, size_t cap) {
    if (sh_var_get(vs, name, strlen(name)) == NULL) {
        char word[SH_NAME_MAX + 1];
        (void)snprintf(word, sizeof(word), "%s=", name);
        if (!sh_var_assign(vs, word, why, cap)) {
            return false;
        }
    }
    for (int i = 0; i < vs->n; i++) {
        if (strcmp(vs->v[i].name, name) == 0) {
            vs->v[i].exported = on;
        }
    }
    return true;
}

bool sh_var_exported(const sh_vars *vs, const char *name) {
    for (int i = 0; i < vs->n; i++) {
        if (strcmp(vs->v[i].name, name) == 0) {
            return vs->v[i].exported;
        }
    }
    return false;
}

void sh_var_unset(sh_vars *vs, const char *name) {
    for (int i = 0; i < vs->n; i++) {
        if (strcmp(vs->v[i].name, name) == 0 && !vs->v[i].held) {
            vs->n--;
            vs->v[i] = vs->v[vs->n];
            return;
        }
    }
}

/* ---- aliases: replaced where a command starts, as the text is read ---- */

enum { ALIAS_DEPTH = 8 };

typedef struct {
    char *out;
    size_t cap;
    size_t o;
    sh_alias find;
    void *user;
    const char *used[ALIAS_DEPTH]; /* the aliases being replaced: none again inside itself */
    int nused;
    bool full;
} aliaser;

static void a_put(aliaser *a, const char *p, size_t n) {
    if (a->full || a->o + n + 1 > a->cap) {
        a->full = true;
        return;
    }
    memcpy(a->out + a->o, p, n);
    a->o += n;
    a->out[a->o] = '\0';
}

static bool word_end_char(char c) {
    if (c == '\0' || blank(c)) {
        return true;
    }
    return strchr("\n;&|()", c) != NULL;
}

static bool alias_text(aliaser *a, const char *p, const char *end);

/* One word at *pp, copied as it is: its quotes, ${ }, ` ` and here-document
   tokens whole; a $( )'s commands through the aliases too. */
static void a_word(aliaser *a, const char **pp) {
    const char *p = *pp;
    const char *from = p;
    if (word_end_char(*p) && *p != '\0') {
        a_put(a, p, 1); /* no word here (a ; among patterns): on, the checker says why */
        *pp = p + 1;
        return;
    }
    while (!word_end_char(*p)) {
        const char *end = NULL;
        if (*p == '\\' && p[1] != '\0') {
            end = p + 2;
        } else if (*p == '\'' || *p == '`' || *p == '\x01') {
            end = strchr(p + 1, *p);
            end = end != NULL ? end + 1 : p + strlen(p);
        } else if (*p == '"') { /* a "$(...)" inside: its commands too */
            end = p + 1;
            while (*end != '\0' && *end != '"') {
                if (end[0] == '$' && end[1] == '(' && end[2] != '(' && subst_end(end + 2) != NULL) {
                    const char *close = subst_end(end + 2);
                    a_put(a, from, (size_t)(end - from) + 2);
                    (void)alias_text(a, end + 2, close);
                    from = close;
                    end = close + 1;
                    continue;
                }
                end += end[0] == '\\' && end[1] != '\0' ? 2 : 1;
            }
            if (*end == '"') {
                end++;
            }
        } else if (p[0] == '$' && p[1] == '(' && p[2] == '(' && arith_end(p + 3) != NULL) {
            end = arith_end(p + 3) + 2;
        } else if (p[0] == '$' && p[1] == '(' && subst_end(p + 2) != NULL) {
            const char *close = subst_end(p + 2);
            a_put(a, from, (size_t)(p - from) + 2);
            (void)alias_text(a, p + 2, close);
            from = close;
            end = close + 1;
        } else if (p[0] == '$' && p[1] == '(') {
            end = p + strlen(p);
        } else if (p[0] == '$' && p[1] == '{') {
            end = brace_end(p + 2);
            end = end != NULL ? end + 1 : p + strlen(p);
        } else {
            end = p + 1;
        }
        p = end;
    }
    a_put(a, from, (size_t)(p - from));
    *pp = p;
}

/* The plain word at p (no quote, no $): its length; 0 for any other. */
static size_t plain_len(const char *p) {
    size_t n = 0;
    while (!word_end_char(p[n]) && strchr("'\"`\\$<>\x01", p[n]) == NULL) {
        n++;
    }
    return word_end_char(p[n]) ? n : 0;
}

static bool alias_used(const aliaser *a, const char *name, size_t n) {
    for (int i = 0; i < a->nused; i++) {
        if (strlen(a->used[i]) == n && memcmp(a->used[i], name, n) == 0) {
            return true;
        }
    }
    return false;
}

/* The text from p to end through the aliases, into a->out. Returns whether
   a word after it is still where a command starts (an alias whose value
   ends in a blank). end is a NUL or the ) of a $(: both end any word. */
static bool alias_text(aliaser *a, const char *p, const char *end) {
    bool start = true;
    int mode = 0; /* 1: a case's word, until in; 2: a branch's patterns, until ) */
    while (p < end && !a->full) {
        char c = *p;
        if (blank(c)) {
            a_put(a, p++, 1);
            continue;
        }
        size_t rl = 0;
        int rw = sh_reserved(p, &rl);
        if (mode == 2 && rw == SH_RW_ESAC) {
            a_put(a, p, rl);
            p += rl;
            mode = 0;
            start = false;
            continue;
        }
        if (mode == 2) { /* the patterns, up to the ) that ends them */
            if (c == ')') {
                mode = 0;
                start = true;
                a_put(a, p++, 1);
            } else if (c == '\n' || c == '|' || c == '(') {
                a_put(a, p++, 1);
            } else {
                a_word(a, &p);
            }
            continue;
        }
        if (c == ';' && p[1] == ';') {
            a_put(a, p, 2);
            p += 2;
            mode = 2;
            continue;
        }
        if (c == ')') {
            a_put(a, p++, 1);
            start = false;
            continue;
        }
        if (strchr("\n;&|(", c) != NULL) {
            size_t n = (c == '&' || c == '|') && p[1] == c ? 2 : 1;
            a_put(a, p, n);
            p += n;
            start = true;
            continue;
        }
        if (c == '#') {
            size_t n = 0;
            while (p + n < end && p[n] != '\n') {
                n++;
            }
            a_put(a, p, n);
            p += n;
            continue;
        }
        if (start && rw != SH_RW_NONE) {
            a_put(a, p, rl);
            p += rl;
            start = true; /* if then do {...: a command next; not after these */
            if (rw == SH_RW_FOR || rw == SH_RW_CASE || rw == SH_RW_ESAC || rw == SH_RW_FI ||
                rw == SH_RW_DONE || rw == SH_RW_RBRACE) {
                start = false;
            }
            size_t for_do = rw == SH_RW_FOR ? sh_for_do(p) : 0;
            if (for_do > 0) {
                a_put(a, p, for_do);
                p += for_do;
                start = true; /* do next, and a command after it */
            }
            if (rw == SH_RW_CASE) {
                mode = 1;
            }
            continue;
        }
        size_t bang = start ? sh_bang(p) : 0;
        if (bang > 0) {
            a_put(a, p, bang);
            p += bang;
            continue;
        }
        size_t n = plain_len(p);
        size_t name_len = 0;
        size_t eq = 0;
        while (eq < n && p[eq] != '=') {
            eq++;
        }
        if (start && eq < n && sh_is_name(p, eq)) {
            a_word(a, &p); /* NAME=value: the command comes after it */
            continue;
        }
        const char *v = NULL;
        if (start && n > 0 && sh_fdef(p, &name_len) == 0 && !alias_used(a, p, n) &&
            a->nused < ALIAS_DEPTH) {
            v = a->find(a->user, p, n);
        }
        if (v != NULL) {
            static char names[ALIAS_DEPTH][SH_NAME_MAX];
            (void)snprintf(names[a->nused], sizeof(names[0]), "%.*s", (int)n, p);
            a->used[a->nused] = names[a->nused];
            a->nused++;
            start = alias_text(a, v, v + strlen(v));
            a->nused--;
            p += n;
            continue;
        }
        const char *w = p;
        a_word(a, &p);
        if (mode == 1 && p - w == 2 && memcmp(w, "in", 2) == 0) {
            mode = 2;
        }
        start = false;
    }
    size_t k = a->o;
    if (k == 0) {
        return false;
    }
    return blank(a->out[k - 1]);
}

bool sh_aliases(const char *text, char *out, size_t cap, sh_alias find, void *user, char *why,
                size_t whycap) {
    aliaser a = {.out = out, .cap = cap, .find = find, .user = user};
    if (cap == 0) {
        return fail(why, whycap, "Line too long");
    }
    out[0] = '\0';
    (void)alias_text(&a, text, text + strlen(text));
    if (a.full) {
        return fail(why, whycap, "Line too long");
    }
    return true;
}
