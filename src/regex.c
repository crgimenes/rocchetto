#include "regex.h"

#include <stdio.h>
#include <string.h>

enum {
    OP_CHAR,
    OP_ANY,
    OP_CLASS,
    OP_BOL,
    OP_EOL,
    OP_SPLIT, /* x first, y on backtracking */
    OP_JMP,
    OP_SAVE,     /* a group's start or end: vals[arg] = sp */
    OP_MARK,     /* a star's round begins: vals[RE_GROUPS * 2 + arg] = sp */
    OP_PROGRESS, /* ...and ends having read something, or not at all */
    OP_BACKREF,
    OP_MATCH,
};

enum { REP_INF = -1, REP_MAX = 255 };

typedef struct {
    sh_regex *re;
    const char *p;
    bool ere;
    const char *err;
} compiler;

static char fold(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static int emit(compiler *c, uint8_t op, uint8_t arg, int x, int y) {
    sh_regex *re = c->re;
    if (re->n >= RE_INST_MAX) {
        c->err = "regex too long";
        return -1;
    }
    re->prog[re->n].op = op;
    re->prog[re->n].arg = arg;
    re->prog[re->n].x = (int16_t)x;
    re->prog[re->n].y = (int16_t)y;
    return re->n++;
}

/* Room for one instruction at at: the code after it moves on by one (its
   jumps are relative, so they stay right). */
static bool insert_at(compiler *c, int at) {
    sh_regex *re = c->re;
    if (re->n >= RE_INST_MAX) {
        c->err = "regex too long";
        return false;
    }
    memmove(&re->prog[at + 1], &re->prog[at], (size_t)(re->n - at) * sizeof(re->prog[0]));
    re->n++;
    return true;
}

static void set_bit(uint8_t *set, unsigned char b) {
    set[b >> 3U] = (uint8_t)(set[b >> 3U] | (1U << (b & 7U)));
}

static bool has_bit(const uint8_t *set, unsigned char b) {
    return (set[b >> 3U] & (1U << (b & 7U))) != 0;
}

/* [...] at c->p (past the [): a class of its own. */
static bool bracket(compiler *c) {
    sh_regex *re = c->re;
    if (re->nclasses == RE_CLASS_MAX) {
        c->err = "too many [...] in a regex";
        return false;
    }
    uint8_t *set = re->classes[re->nclasses];
    memset(set, 0, 32);
    const char *p = c->p;
    bool negate = false;
    if (*p == '^') {
        negate = true;
        p++;
    }
    bool first = true;
    while (*p != '\0' && (first || *p != ']')) {
        first = false;
        if (p[0] == '[' && p[1] == ':') {
            static const struct {
                const char *name;
                const char *bytes;
            } classes[] = {
                {"alpha", "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"},
                {"digit", "0123456789"},
                {"alnum", "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"},
                {"upper", "ABCDEFGHIJKLMNOPQRSTUVWXYZ"},
                {"lower", "abcdefghijklmnopqrstuvwxyz"},
                {"space", " \t\n\r\v\f"},
                {"blank", " \t"},
                {"punct", "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"},
                {"xdigit", "0123456789ABCDEFabcdef"},
            };
            const char *end = strstr(p + 2, ":]");
            bool known = false;
            for (size_t i = 0; end != NULL && i < sizeof(classes) / sizeof(classes[0]); i++) {
                size_t n = strlen(classes[i].name);
                if ((size_t)(end - (p + 2)) == n && strncmp(p + 2, classes[i].name, n) == 0) {
                    for (const char *b = classes[i].bytes; *b != '\0'; b++) {
                        set_bit(set, (unsigned char)*b);
                    }
                    known = true;
                }
            }
            if (!known) {
                c->err = "unknown [:class:] in a regex";
                return false;
            }
            p = end + 2;
            continue;
        }
        unsigned char lo = (unsigned char)*p++;
        if ((lo == '[') && (*p == '.' || *p == '=')) { /* [.x.] [=x=]: x */
            char close = *p;
            lo = (unsigned char)p[1];
            if (p[1] == '\0' || p[2] != close || p[3] != ']') {
                c->err = "unfinished [. or [= in a regex";
                return false;
            }
            p += 4;
        }
        unsigned char hi = lo;
        if (p[0] == '-' && p[1] != ']' && p[1] != '\0') {
            hi = (unsigned char)p[1];
            p += 2;
        }
        for (unsigned v = lo; v <= hi; v++) {
            set_bit(set, (unsigned char)v);
            if (re->icase) {
                set_bit(set, (unsigned char)fold((char)v));
                if (v >= 'a' && v <= 'z') {
                    set_bit(set, (unsigned char)(v - 'a' + 'A'));
                }
            }
        }
    }
    if (*p != ']') {
        c->err = "unmatched [ in a regex";
        return false;
    }
    c->p = p + 1;
    if (negate) {
        for (int i = 0; i < 32; i++) {
            set[i] = (uint8_t)~set[i];
        }
    }
    return emit(c, OP_CLASS, (uint8_t)re->nclasses++, 0, 0) >= 0;
}

static bool alternation(compiler *c, int depth);

/* Whether c->p ends what is being read: the pattern, a group, or an
   alternative. */
static bool at_end(const compiler *c) {
    const char *p = c->p;
    if (*p == '\0') {
        return true;
    }
    if (c->ere) {
        if (*p == ')' || *p == '|') {
            return true;
        }
        return false;
    }
    if (p[0] == '\\' && (p[1] == ')' || p[1] == '|')) {
        return true;
    }
    return false;
}

static bool atom(compiler *c, int depth, bool first) {
    sh_regex *re = c->re;
    const char *p = c->p;
    char ch = *p;
    if (c->ere ? ch == '(' : (ch == '\\' && p[1] == '(')) {
        c->p += c->ere ? 1 : 2;
        int g = ++re->ngroups;
        if (g < RE_GROUPS && emit(c, OP_SAVE, (uint8_t)(2 * g), 0, 0) < 0) {
            return false;
        }
        if (!alternation(c, depth + 1)) {
            return false;
        }
        if (c->ere ? *c->p != ')' : !(c->p[0] == '\\' && c->p[1] == ')')) {
            c->err = "unmatched ( in a regex";
            return false;
        }
        c->p += c->ere ? 1 : 2;
        if (g >= RE_GROUPS) {
            return true; /* a group past \9: matched, not kept */
        }
        return emit(c, OP_SAVE, (uint8_t)((2 * g) + 1), 0, 0) >= 0;
    }
    c->p++;
    if (ch == '[') {
        return bracket(c);
    }
    if (ch == '.') {
        return emit(c, OP_ANY, 0, 0, 0) >= 0;
    }
    if (ch == '^' && (c->ere || first)) {
        return emit(c, OP_BOL, 0, 0, 0) >= 0;
    }
    if (ch == '$' && (c->ere || at_end(c))) {
        return emit(c, OP_EOL, 0, 0, 0) >= 0;
    }
    if (ch == '\\') {
        char e = *c->p;
        if (e == '\0') {
            c->err = "a regex ends in \\";
            return false;
        }
        c->p++;
        if (e >= '1' && e <= '9') {
            if (e - '0' > re->ngroups) {
                c->err = "a \\N in a regex before its group";
                return false;
            }
            return emit(c, OP_BACKREF, (uint8_t)(e - '0'), 0, 0) >= 0;
        }
        if (e == 'n') {
            e = '\n';
        } else if (e == 't') {
            e = '\t';
        }
        return emit(c, OP_CHAR, (uint8_t)e, 0, 0) >= 0;
    }
    return emit(c, OP_CHAR, (uint8_t)ch, 0, 0) >= 0;
}

/* A quantifier at c->p: *, +, ?, {m,n} (ERE) or *, \+, \?, \{m,n\} (BRE);
   false (nothing read) for none. */
static bool quantifier(compiler *c, int *min, int *max) {
    const char *p = c->p;
    char q = *p;
    bool escaped = false;
    if (!c->ere && q == '\\' && (p[1] == '+' || p[1] == '?' || p[1] == '{')) {
        q = p[1];
        escaped = true;
    } else if (!c->ere && (q == '+' || q == '?' || q == '{')) {
        return false; /* plain in a basic regex */
    }
    if (q == '*') {
        *min = 0;
        *max = REP_INF;
        c->p = p + 1;
        return true;
    }
    if (q == '+' || q == '?') {
        *min = q == '+' ? 1 : 0;
        *max = q == '+' ? REP_INF : 1;
        c->p = p + (escaped ? 2 : 1);
        return true;
    }
    if (q != '{') {
        return false;
    }
    const char *s = p + (escaped ? 2 : 1);
    if (*s < '0' || *s > '9') {
        return false; /* a { of its own: itself */
    }
    int a = 0;
    while (*s >= '0' && *s <= '9' && a <= REP_MAX) {
        a = (a * 10) + (*s++ - '0');
    }
    int b = a;
    if (*s == ',') {
        s++;
        b = REP_INF;
        if (*s >= '0' && *s <= '9') {
            b = 0;
            while (*s >= '0' && *s <= '9' && b <= REP_MAX) {
                b = (b * 10) + (*s++ - '0');
            }
        }
    }
    if (escaped ? !(s[0] == '\\' && s[1] == '}') : *s != '}') {
        c->err = "unfinished { } in a regex";
        return true;
    }
    if (a > REP_MAX || (b != REP_INF && (b > REP_MAX || b < a))) {
        c->err = "bad { } in a regex";
        return true;
    }
    *min = a;
    *max = b;
    c->p = s + (escaped ? 2 : 1);
    return true;
}

static bool append(compiler *c, const re_inst *code, int len) {
    sh_regex *re = c->re;
    if (re->n + len > RE_INST_MAX) {
        c->err = "regex too long";
        return false;
    }
    memcpy(&re->prog[re->n], code, (size_t)len * sizeof(code[0]));
    re->n += len;
    return true;
}

/* The atom's code (from start on) repeated min to max times. */
static bool repeat(compiler *c, int start, int min, int max) {
    sh_regex *re = c->re;
    static re_inst atom_code[RE_INST_MAX];
    int len = re->n - start;
    memcpy(atom_code, &re->prog[start], (size_t)len * sizeof(atom_code[0]));
    re->n = start;
    for (int i = 0; i < min; i++) {
        if (!append(c, atom_code, len)) {
            return false;
        }
    }
    if (max == REP_INF) { /* SPLIT MARK atom PROGRESS JMP */
        if (re->nloops == RE_LOOPS) {
            c->err = "too many * in a regex";
            return false;
        }
        int loop = re->nloops++;
        int s = emit(c, OP_SPLIT, 0, 1, len + 4);
        if (s < 0 || emit(c, OP_MARK, (uint8_t)loop, 0, 0) < 0 || !append(c, atom_code, len) ||
            emit(c, OP_PROGRESS, (uint8_t)loop, 0, 0) < 0 ||
            emit(c, OP_JMP, 0, -(len + 3), 0) < 0) {
            return false;
        }
        return true;
    }
    for (int i = min; i < max; i++) {
        if (emit(c, OP_SPLIT, 0, 1, len + 1) < 0 || !append(c, atom_code, len)) {
            return false;
        }
    }
    return true;
}

static bool concatenation(compiler *c, int depth) {
    bool first = true;
    while (!at_end(c)) {
        if (!first && c->err != NULL) {
            return false;
        }
        int start = c->re->n;
        const char *before = c->p;
        /* a * that has nothing before it is itself */
        if (*c->p == '*' && first) {
            c->p++;
            if (emit(c, OP_CHAR, '*', 0, 0) < 0) {
                return false;
            }
        } else if (!atom(c, depth, first)) {
            return false;
        }
        bool was_anchor = false; /* ^ or $ read as such: nothing repeats them */
        if (c->p - before == 1 && (*before == '^' || *before == '$') &&
            c->re->prog[start].op != OP_CHAR) {
            was_anchor = true;
        }
        int min = 0;
        int max = 0;
        while (!was_anchor && quantifier(c, &min, &max)) {
            if (c->err != NULL || !repeat(c, start, min, max)) {
                return false;
            }
        }
        if (c->err != NULL) {
            return false;
        }
        first = false;
    }
    return true;
}

static bool alternation(compiler *c, int depth) {
    if (depth > 16) {
        c->err = "regex nested too deep";
        return false;
    }
    int start = c->re->n;
    if (!concatenation(c, depth)) {
        return false;
    }
    bool bar = false;
    if (c->ere) {
        bar = *c->p == '|';
    } else if (c->p[0] == '\\' && c->p[1] == '|') {
        bar = true;
    }
    if (!bar) {
        return true;
    }
    c->p += c->ere ? 1 : 2;
    if (!insert_at(c, start)) { /* SPLIT first, rest */
        return false;
    }
    int jmp = emit(c, OP_JMP, 0, 0, 0);
    if (jmp < 0) {
        return false;
    }
    c->re->prog[start].op = OP_SPLIT;
    c->re->prog[start].x = 1;
    c->re->prog[start].y = (int16_t)(c->re->n - start);
    if (!alternation(c, depth)) {
        return false;
    }
    c->re->prog[jmp].x = (int16_t)(c->re->n - jmp);
    return true;
}

bool sh_regex_compile(sh_regex *re, const char *pattern, bool extended, bool icase, char *why,
                      size_t cap) {
    memset(re, 0, sizeof(*re));
    re->icase = icase;
    compiler c = {re, pattern, extended, NULL};
    re->anchored = pattern[0] == '^';
    if (emit(&c, OP_SAVE, 0, 0, 0) < 0 || !alternation(&c, 0)) {
        (void)snprintf(why, cap, "%s", c.err != NULL ? c.err : "bad regex");
        return false;
    }
    if (*c.p != '\0') {
        (void)snprintf(why, cap, "%s",
                       extended ? "unmatched ) in a regex" : "unmatched \\) in a regex");
        return false;
    }
    if (emit(&c, OP_SAVE, 1, 0, 0) < 0 || emit(&c, OP_MATCH, 0, 0, 0) < 0) {
        (void)snprintf(why, cap, "%s", c.err);
        return false;
    }
    for (int i = 0; i < re->n && re->anchored; i++) {
        if (re->prog[i].op == OP_SPLIT) {
            re->anchored = false; /* ^a|b: not only at the start */
        }
    }
    return true;
}

/* ---- matching: backtracking over the program, every path, the longest ---- */

static bool same(const sh_regex *re, char a, char b) {
    if (re->icase) {
        return fold(a) == fold(b);
    }
    return a == b;
}

enum { RE_STEPS = 1000000, RE_STACK = ROC_CFG_RE_STACK, RE_VALS = (RE_GROUPS * 2) + RE_LOOPS };

typedef struct {
    int pc; /* a thread's, or -1: an undo of vals[slot] */
    int slot;
    long at; /* the thread's sp, or the old value */
} re_entry;

/* One start: 1 with best set for the longest match from it, 0 for none,
   -1 for too many steps. */
static int run(const sh_regex *re, const char *s, size_t len, size_t start, bool not_bol,
               long *best_vals, long *steps) {
    static re_entry stack[RE_STACK];
    long vals[RE_VALS];
    for (int i = 0; i < RE_VALS; i++) {
        vals[i] = -1;
    }
    int top = 0;
    long best = -1;
    stack[top++] = (re_entry){0, 0, (long)start};
    while (top > 0) {
        re_entry e = stack[--top];
        if (e.pc < 0) {
            vals[e.slot] = e.at;
            continue;
        }
        int pc = e.pc;
        long sp = e.at;
        for (;;) {
            if (++*steps > RE_STEPS) {
                return -1;
            }
            const re_inst *in = &re->prog[pc];
            bool ok = true;
            switch (in->op) {
            case OP_CHAR:
                ok = (size_t)sp < len;
                if (ok) {
                    ok = same(re, s[sp], (char)in->arg);
                }
                sp++;
                pc++;
                break;
            case OP_ANY:
                ok = (size_t)sp < len;
                if (ok) {
                    ok = s[sp] != '\n';
                }
                sp++;
                pc++;
                break;
            case OP_CLASS:
                ok = (size_t)sp < len;
                if (ok) {
                    ok = has_bit(re->classes[in->arg], (unsigned char)s[sp]);
                }
                sp++;
                pc++;
                break;
            case OP_BOL:
                ok = sp == 0;
                if (not_bol) {
                    ok = false;
                }
                pc++;
                break;
            case OP_EOL:
                ok = (size_t)sp == len;
                pc++;
                break;
            case OP_SPLIT:
                if (top >= RE_STACK) {
                    return -1;
                }
                stack[top++] = (re_entry){pc + in->y, 0, sp};
                pc += in->x;
                break;
            case OP_JMP:
                pc += in->x;
                break;
            case OP_SAVE:
            case OP_MARK: {
                int slot = in->op == OP_SAVE ? in->arg : (RE_GROUPS * 2) + in->arg;
                if (top >= RE_STACK) {
                    return -1;
                }
                stack[top++] = (re_entry){-1, slot, vals[slot]};
                vals[slot] = sp;
                pc++;
                break;
            }
            case OP_PROGRESS:
                ok = vals[(RE_GROUPS * 2) + in->arg] != sp; /* a round that read nothing ends it */
                pc++;
                break;
            case OP_BACKREF: {
                long a = vals[(size_t)2 * in->arg];
                long b = vals[((size_t)2 * in->arg) + 1];
                long n = a >= 0 && b >= a ? b - a : 0;
                ok = (size_t)(sp + n) <= len;
                for (long i = 0; ok && i < n; i++) {
                    char x = s[a + i];
                    char y = s[sp + i];
                    ok = same(re, x, y);
                }
                sp += n;
                pc++;
                break;
            }
            default: /* OP_MATCH */
                if (sp > best) {
                    best = sp;
                    memcpy(best_vals, vals, sizeof(long) * RE_GROUPS * 2);
                }
                ok = false;
                if ((size_t)best == len) {
                    return 1; /* none can be longer */
                }
                break;
            }
            if (!ok) {
                break;
            }
        }
    }
    return best >= 0 ? 1 : 0;
}

int sh_regex_exec(const sh_regex *re, const char *s, size_t len, size_t from, bool not_bol,
                  long *so, long *eo) {
    long steps = 0;
    long vals[RE_GROUPS * 2];
    for (size_t start = from; start <= len; start++) {
        if (re->anchored && (start != 0 || not_bol)) {
            return 0;
        }
        if (re->prog[1].op == OP_CHAR && !re->icase && start < len &&
            s[start] != (char)re->prog[1].arg) {
            continue; /* it starts with a byte this one is not */
        }
        int r = run(re, s, len, start, not_bol, vals, &steps);
        if (r < 0) {
            return -1;
        }
        if (r == 1) {
            for (int g = 0; g < RE_GROUPS; g++) {
                so[g] = vals[(size_t)2 * (size_t)g];
                eo[g] = vals[((size_t)2 * (size_t)g) + 1];
            }
            return 1;
        }
    }
    return 0;
}
