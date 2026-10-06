#include "diff.h"

#include <stdio.h>
#include <string.h>

#include "roc.h"
#include "sh.h"

enum {
    DIFF_LINES_MAX = 16384,
    DIFF_CELLS_MAX = 1U << 20U, /* the table of the part in between: 2 MB */
    DIFF_CONTEXT = 3,
};

/* A line: its bytes, whether a newline ends it (only the last may lack
   one), and a hash to compare it by first. */
typedef struct {
    const uint8_t *p;
    uint32_t len;
    uint32_t hash;
    bool nl;
} dline;

typedef struct {
    const char *name;
    dline *lines;
    uint32_t n;
} dfile;

/* One step of the edit: a line both have, one only a has, one only b has;
   a and b are where the step is in each (for the side it lacks, how many
   lines of it came before). */
enum { DOP_SAME, DOP_DEL, DOP_ADD };
typedef struct {
    uint8_t op;
    uint32_t a;
    uint32_t b;
} dop;

/* In the scratch, taken when diff starts. */
static dline *lines_a;
static dline *lines_b;
static uint16_t *table;
static dop *ops;

static void usage(roc *m) {
    term_puts(&m->t, "usage: diff [-u] <file> <file>   (- is what a | hands over)\r\n");
    m->status = 2;
}

static void trouble(roc *m, const char *arg, const char *why) {
    roc_err(m, "diff", arg, why);
    m->status = 2; /* POSIX: 2 is trouble, 1 is a difference */
}

static uint32_t hash_of(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261U;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619U;
    }
    return h;
}

/* The bytes of the file named arg (here, in the home, or - for the input),
   cut into lines. False, said, when they cannot be had. */
static bool load(roc *m, const char *arg, dfile *f, dline *lines) {
    const uint8_t *data = NULL;
    size_t len = 0;
    f->name = arg;
    f->lines = lines;
    f->n = 0;
    if (strcmp(arg, "-") == 0) {
        if (!m->piped || m->in == NULL) {
            trouble(m, arg, "No input: nothing was piped");
            return false;
        }
        if (m->in_spool != NULL) {
            trouble(m, arg, "Input too large to hold whole here");
            return false;
        }
        data = m->in;
        len = m->in_len;
    } else {
        char path[VFS_PATH_MAX];
        if (!roc_resolve_arg(m, arg, path, sizeof(path))) {
            m->status = 2;
            return false;
        }
        const vfs_node *node = vfs_lookup(&m->fs, path);
        if (node != NULL && node->dir) {
            trouble(m, arg, "Is a directory");
            return false;
        }
        if (!roc_find_file(m, path, &data, &len)) {
            trouble(m, arg,
                    node != NULL ? "On the site, not here: cp it home first"
                                 : "No such file or directory");
            return false;
        }
    }
    size_t at = 0;
    while (at < len) {
        if (f->n == DIFF_LINES_MAX) {
            trouble(m, arg, "Too many lines to compare here");
            return false;
        }
        size_t end = at;
        while (end < len && data[end] != '\n') {
            end++;
        }
        dline *l = &lines[f->n];
        l->p = data + at;
        l->len = (uint32_t)(end - at);
        l->nl = end < len;
        l->hash = hash_of(l->p, l->len);
        f->n++;
        at = end + 1;
    }
    return true;
}

static bool same(const dline *x, const dline *y) {
    if (x->hash != y->hash || x->len != y->len || x->nl != y->nl) {
        return false;
    }
    return memcmp(x->p, y->p, x->len) == 0;
}

/* Cell (i, j) of the table, w columns a row: every index is inside it, since
   edit refuses a middle part that would not fit; the check says so to the
   reader, and to the analyzer. */
static uint16_t cell(size_t w, uint32_t i, uint32_t j) {
    size_t at = ((size_t)i * w) + j;
    return at < DIFF_CELLS_MAX ? table[at] : 0;
}

static void set_cell(size_t w, uint32_t i, uint32_t j, uint16_t v) {
    size_t at = ((size_t)i * w) + j;
    if (at < DIFF_CELLS_MAX) {
        table[at] = v;
    }
}

static uint32_t add_op(uint32_t k, uint8_t op, uint32_t a, uint32_t b) {
    ops[k].op = op;
    ops[k].a = a;
    ops[k].b = b;
    return k + 1;
}

/* The edit from a to b, in ops: the lines both begin and end with, and
   between them a longest common subsequence. False when the part in
   between is too large for the table. */
static bool edit(const dfile *a, const dfile *b, uint32_t *nops) {
    uint32_t pre = 0;
    while (pre < a->n && pre < b->n && same(&a->lines[pre], &b->lines[pre])) {
        pre++;
    }
    uint32_t suf = 0;
    while (suf < a->n - pre && suf < b->n - pre &&
           same(&a->lines[a->n - 1 - suf], &b->lines[b->n - 1 - suf])) {
        suf++;
    }
    uint32_t n = a->n - pre - suf;
    uint32_t k = b->n - pre - suf;
    if ((uint64_t)(n + 1) * (k + 1) > DIFF_CELLS_MAX) {
        return false;
    }
    const dline *x = a->lines + pre;
    const dline *y = b->lines + pre;
    size_t w = (size_t)k + 1;
    for (uint32_t i = n + 1; i-- > 0;) {
        for (uint32_t j = k + 1; j-- > 0;) {
            uint16_t v = 0;
            if (i < n && j < k) {
                if (same(&x[i], &y[j])) {
                    v = (uint16_t)(cell(w, i + 1, j + 1) + 1);
                } else {
                    uint16_t down = cell(w, i + 1, j);
                    uint16_t right = cell(w, i, j + 1);
                    v = down > right ? down : right;
                }
            }
            set_cell(w, i, j, v);
        }
    }
    uint32_t c = 0;
    for (uint32_t i = 0; i < pre; i++) {
        c = add_op(c, DOP_SAME, i, i);
    }
    uint32_t i = 0;
    uint32_t j = 0;
    while (i < n || j < k) {
        if (i < n && j < k && same(&x[i], &y[j])) {
            c = add_op(c, DOP_SAME, pre + i, pre + j);
            i++;
            j++;
        } else if (j == k || (i < n && cell(w, i + 1, j) >= cell(w, i, j + 1))) {
            c = add_op(c, DOP_DEL, pre + i, pre + j);
            i++;
        } else {
            c = add_op(c, DOP_ADD, pre + i, pre + j);
            j++;
        }
    }
    for (uint32_t s = 0; s < suf; s++) {
        c = add_op(c, DOP_SAME, pre + n + s, pre + k + s);
    }
    *nops = c;
    return true;
}

static void put_line(roc *m, const char *mark, const dline *l) {
    term_puts(&m->t, mark);
    term_write(&m->t, l->p, l->len);
    term_puts(&m->t, "\r\n");
    if (!l->nl) {
        term_puts(&m->t, "\\ No newline at end of file\r\n");
    }
}

/* first,last as diff writes a range of lines (from 1), or one number. */
static void put_range(roc *m, uint32_t first, uint32_t last) {
    term_put_u32(&m->t, first);
    if (last != first) {
        term_puts(&m->t, ",");
        term_put_u32(&m->t, last);
    }
}

/* The normal format: each run of changes as 2,3c2 (or a, d), the lines of a
   with <, ---, the lines of b with >. */
static void normal(roc *m, const dfile *a, const dfile *b, uint32_t nops) {
    uint32_t s = 0;
    while (s < nops) {
        if (ops[s].op == DOP_SAME) {
            s++;
            continue;
        }
        uint32_t e = s;
        uint32_t dels = 0;
        uint32_t adds = 0;
        while (e < nops && ops[e].op != DOP_SAME) {
            dels += ops[e].op == DOP_DEL ? 1U : 0U;
            adds += ops[e].op == DOP_ADD ? 1U : 0U;
            e++;
        }
        uint32_t a0 = ops[s].a; /* lines of a before the run */
        uint32_t b0 = ops[s].b;
        if (dels > 0) {
            put_range(m, a0 + 1, a0 + dels);
        } else {
            term_put_u32(&m->t, a0);
        }
        const char *what = "a"; /* lines added */
        if (dels > 0) {
            what = adds > 0 ? "c" : "d"; /* changed, or deleted */
        }
        term_puts(&m->t, what);
        if (adds > 0) {
            put_range(m, b0 + 1, b0 + adds);
        } else {
            term_put_u32(&m->t, b0);
        }
        term_puts(&m->t, "\r\n");
        for (uint32_t i = s; i < e; i++) {
            if (ops[i].op == DOP_DEL) {
                put_line(m, "< ", &a->lines[ops[i].a]);
            }
        }
        if (dels > 0 && adds > 0) {
            term_puts(&m->t, "---\r\n");
        }
        for (uint32_t i = s; i < e; i++) {
            if (ops[i].op == DOP_ADD) {
                put_line(m, "> ", &b->lines[ops[i].b]);
            }
        }
        s = e;
    }
}

/* -u: hunks of the changes with three lines around them, joined when
   their context meets, under @@ -start,len +start,len @@. */
static void unified(roc *m, const dfile *a, const dfile *b, uint32_t nops) {
    term_puts(&m->t, "--- ");
    term_puts(&m->t, a->name);
    term_puts(&m->t, "\r\n+++ ");
    term_puts(&m->t, b->name);
    term_puts(&m->t, "\r\n");
    uint32_t s = 0;
    while (s < nops) {
        if (ops[s].op == DOP_SAME) {
            s++;
            continue;
        }
        uint32_t first = s > DIFF_CONTEXT ? s - DIFF_CONTEXT : 0;
        uint32_t last = s; /* the last change of the hunk */
        uint32_t i = s;
        uint32_t gap = 0;
        while (i < nops && gap <= 2 * DIFF_CONTEXT) {
            if (ops[i].op == DOP_SAME) {
                gap++;
            } else {
                last = i;
                gap = 0;
            }
            i++;
        }
        uint32_t end = last + 1 + DIFF_CONTEXT < nops ? last + 1 + DIFF_CONTEXT : nops;
        uint32_t alen = 0;
        uint32_t blen = 0;
        for (uint32_t k = first; k < end; k++) {
            alen += ops[k].op != DOP_ADD ? 1U : 0U;
            blen += ops[k].op != DOP_DEL ? 1U : 0U;
        }
        /* an empty side starts at the line before it, as diff -u writes it */
        uint32_t astart = ops[first].a + (alen > 0 ? 1U : 0U);
        uint32_t bstart = ops[first].b + (blen > 0 ? 1U : 0U);
        term_puts(&m->t, "@@ -");
        term_put_u32(&m->t, astart);
        if (alen != 1) {
            term_puts(&m->t, ",");
            term_put_u32(&m->t, alen);
        }
        term_puts(&m->t, " +");
        term_put_u32(&m->t, bstart);
        if (blen != 1) {
            term_puts(&m->t, ",");
            term_put_u32(&m->t, blen);
        }
        term_puts(&m->t, " @@\r\n");
        for (uint32_t k = first; k < end; k++) {
            if (ops[k].op == DOP_SAME) {
                put_line(m, " ", &a->lines[ops[k].a]);
            } else if (ops[k].op == DOP_DEL) {
                put_line(m, "-", &a->lines[ops[k].a]);
            } else {
                put_line(m, "+", &b->lines[ops[k].b]);
            }
        }
        s = end;
    }
}

void roc_cmd_diff(roc *m, const char *rest) {
    static sh_line sl;
    char why[64];
    if (!sh_split(rest, &sl, why, sizeof(why))) {
        trouble(m, "", why);
        return;
    }
    int at = 0;
    bool u = false;
    if (sl.argc > 0 && strcmp(sl.argv[0], "-u") == 0) {
        u = true;
        at = 1;
    }
    if (sl.argc - at != 2) {
        usage(m);
        return;
    }
    roc_scratch_reset(m);
    lines_a = roc_scratch_take(m, sizeof(dline) * DIFF_LINES_MAX);
    lines_b = roc_scratch_take(m, sizeof(dline) * DIFF_LINES_MAX);
    table = roc_scratch_take(m, sizeof(uint16_t) * DIFF_CELLS_MAX);
    ops = roc_scratch_take(m, sizeof(dop) * 2 * DIFF_LINES_MAX);
    if (lines_a == NULL || lines_b == NULL || table == NULL || ops == NULL) {
        trouble(m, "", "Not enough memory here to compare");
        return;
    }
    dfile a;
    dfile b;
    if (!load(m, sl.argv[at], &a, lines_a) || !load(m, sl.argv[at + 1], &b, lines_b)) {
        return;
    }
    uint32_t nops = 0;
    if (!edit(&a, &b, &nops)) {
        trouble(m, sl.argv[at], "Too different to compare here");
        return;
    }
    bool differ = false;
    for (uint32_t i = 0; i < nops; i++) {
        if (ops[i].op != DOP_SAME) {
            differ = true;
            break;
        }
    }
    if (!differ) {
        return; /* the same: nothing said, $? 0 */
    }
    if (u) {
        unified(m, &a, &b, nops);
    } else {
        normal(m, &a, &b, nops);
    }
    m->status = 1;
}
