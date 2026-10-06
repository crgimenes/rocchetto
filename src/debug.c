#include "debug.h"

#include <stdio.h>
#include <string.h>

#include "canvas.h"
#include "fbc_dump.h"
#include "filo.h"
#include "hl.h"
#include "keys.h"
#include "roc.h"
#include "script.h"
#if ROC_APP_SCREENS
#include "screen.h"
#endif

enum {
    DBG_UNIT_CAP = 1U << 20U,
    DBG_ROWS_MAX = 1U << 16U,
    DBG_LINES_MAX = 1U << 16U,
    DBG_FRAMES_MAX = 64,
    DBG_MARKS_MAX = 4096,
    DBG_SPANS_MAX = 256,
    DBG_MSG_MAX = 256,
    DBG_TEXT_MAX = 1024, /* a line as drawn: wider than any screen */
    DBG_VALUE_MAX = 256,
    DBG_OUT_MAX = 120,
    DBG_TAB = 8,
    DBG_STRIDE = 4096,
    DBG_LEVELS_MAX = 32,
};

/* The edt's colours, as the desktop's debugger has them: text on the
   terminal's own ground, bars dark grey with keys in bold orange and words
   in light grey, the position in green, and the place the run is at in
   orange under black. */
enum {
    COL_BAR = 236,
    COL_WORDS = 252,
    COL_KEYS = 214,
    COL_DIM = 244,
    COL_RULE = 240,
    COL_WHERE = 2,
    COL_HERE_FG = 0,
    COL_HERE_BG = 214,
    COL_BREAK = 196,
};

/* the edt's colour for each hl_class */
static const int16_t class_fg[] = {CV_COLOR_DEFAULT, 244, 108, 139, 74};

/* A row of the listing: a function's heading (of is -1, pc the function)
   or an instruction of function of, from line:col of its source. */
typedef struct {
    int32_t of;
    uint32_t pc;
    uint32_t len;
    uint32_t line;
    uint32_t col;
} code_row;

/* An entry of the debug section: from pc on, line:col (0:0 for none). */
typedef struct {
    uint32_t pc;
    uint32_t line;
    uint32_t col;
} place;

/* The source of an entry point: NAME.filo, its lines in line_at. */
typedef struct {
    char name[72];
    const uint8_t *text;
    size_t len;
    bool found;
    uint32_t first;
    uint32_t nlines;
} source;

typedef struct {
    roc *m;
    filo_ctx *ctx;
    const filo_unit *loaded;
    bool app; /* an entry draw: it runs in a screen's context */
    size_t ulen;
    char entry[64];
    char dir[VFS_PATH_MAX];
    uint32_t nrows;
    uint32_t nplaces;
    uint32_t nlines;
    int16_t file_of[FBC_FNS_MAX]; /* each function's source, by the entry it belongs to */
    filo_bc_frame cur[DBG_FRAMES_MAX];
    uint32_t ncur;
    filo_bc_frame last[DBG_FRAMES_MAX]; /* the run before its last step: where it ended */
    uint32_t nlast;
    uint32_t steps;
    uint32_t marks[DBG_MARKS_MAX]; /* where each movement began, the last one last */
    uint32_t nmarks;
    bool done;
    char ending[DBG_MSG_MAX];
    uint32_t err_line;
    uint32_t err_col;
    char msg[DBG_MSG_MAX]; /* how the run ended, or why a key did nothing */
    uint32_t cursor;       /* the source line the arrows move, from 1; it follows the run */
    bool bytes;            /* the right side shows the bytes, not the instructions */
    bool help;
    int32_t help_top;
    int32_t help_page;
    char out[DBG_OUT_MAX]; /* the last line the program wrote */
    size_t out_len;
    bool out_ended;  /* a newline came: the next byte starts another line */
    uint8_t out_esc; /* inside an escape sequence: 1 after ESC, 2 in CSI */
} debugger;

static debugger D;
static fbc_unit listing;
static source files[FBC_EXPORTS_MAX];
/* In the scratch, taken for the session: the debugger is on top while it
   lasts, so no other tool can start over it. */
static uint8_t *unit_mem;
static code_row *rows;
static place *places;
static uint32_t *line_at;
static bool *breaks;
#if ROC_APP_SCREENS
static filo_ctx app_ctx;
static uint8_t *app_persistent;
static uint8_t *app_run;
#endif

static bool take_memory(roc *m) {
    unit_mem = roc_scratch_take(m, DBG_UNIT_CAP);
    rows = roc_scratch_take(m, sizeof(code_row) * DBG_ROWS_MAX);
    places = roc_scratch_take(m, sizeof(place) * DBG_ROWS_MAX);
    line_at = roc_scratch_take(m, sizeof(uint32_t) * DBG_LINES_MAX);
    breaks = roc_scratch_take(m, sizeof(bool) * DBG_LINES_MAX);
    bool ok = true;
    if (unit_mem == NULL || rows == NULL || places == NULL || line_at == NULL || breaks == NULL) {
        ok = false;
    }
#if ROC_APP_SCREENS
    app_persistent = roc_scratch_take(m, SCR_MEM_PERSISTENT);
    app_run = roc_scratch_take(m, SCR_MEM_RUN);
    if (app_persistent == NULL || app_run == NULL) {
        ok = false;
    }
#endif
    return ok;
}

/* The h page. "# " starts a heading. */
static const char *const help_lines[] = {
    "# filo debug",
    "",
    "A program runs here one step at a time. On the left, its source; on the",
    "right, what the compiler made of it: the instructions (x switches to the",
    "bytes the file really holds); below, the calls running, innermost first.",
    "",
    "# Keys",
    "",
    "  s          step a line, into the calls it makes",
    "  n          next line, over the calls",
    "  i          one instruction",
    "  c          continue to a breakpoint, or to the end",
    "  b          back: undo the last movement",
    "  r          restart from the first instruction",
    "  space      set or clear a breakpoint on the cursor's line",
    "  arrows     move the cursor in the source; its line's instructions",
    "  PgUp/PgDn  come into view on the right, in orange",
    "  x          instructions or bytes on the right",
    "  h          this help (arrows, PgUp/PgDn and space scroll; h, q or Esc",
    "             go back)",
    "  q          quit",
    "",
    "A function a builtin calls back (map, fold, filter) runs whole, in one",
    "step: this machine stops only between the program's own instructions.",
    "",
    "# Reading the screen",
    "",
    "The line the run is at has a dark bar, the column in orange. On the right,",
    "each instruction is a row:",
    "",
    "  0015 4:12   PUSH_L   0         slot 0",
    "  |    |      |        |         what the operand names",
    "  |    |      |        operands",
    "  |    |      the instruction",
    "  |    where in the source it came from, line:column",
    "  its address in the code, counted in bytes",
    "",
    "Below, each call: its function, where it is, its slots (the parameters",
    "first, then one for each let binding) and its operands (the values it is",
    "working on, the top last). In the running call, the operands the next",
    "instruction takes are in orange, and it says which: before CALLB 2 + the",
    "top two are the numbers it adds. What the program writes (echo) shows at",
    "the right of the calls' rule, its last line.",
    "",
    "# The language, in brief",
    "",
    "Everything is a list whose first element says what to do: (+ 1 2) is 3,",
    "(* 2 (+ 3 4)) is 14. Values are numbers (42, 3.14), strings (\"hi\"),",
    "booleans (#t, #f), lists (list 1 2 3), tuples (tuple 1 2) and functions.",
    "",
    "  (def name expr)            a global",
    "  (set name expr)            change the nearest name",
    "  (fn (x y) body...)         a function",
    "  (let ((a 1) (b 2)) body)   names for the body",
    "  (letv (a b) tuple body)    the elements of a tuple, named",
    "  (if test then else)        else may be left out: then it is (list)",
    "  (cond (test body...) ... (else body...))",
    "  (do e1 e2 ...)             in order; the value of the last",
    "  (and a b) (or a b)         stop at the first #f (and) or #t (or)",
    "  (return v)                 leave the function with v",
    "  (exit v)                   end the program with v",
    "",
    "Everything else is a call: a builtin (+ - * / = < list map fold",
    "str-upper ...) or a function of the program. Tests must be booleans;",
    "comparing values of different kinds is an error.",
    "",
    "# The machine",
    "",
    "The compiler turns each function into instructions for a stack machine.",
    "An instruction takes its inputs from the top of the operand stack and",
    "leaves its result there: (+ a b) is PUSH_L a, PUSH_L b, CALLB + with 2",
    "arguments, which pops both and pushes the sum.",
    "",
    "  constants  the literals of the unit, numbered (PUSH_K n)",
    "  globals    the program's names, numbered (PUSH_G, STORE_G)",
    "  slots      a call's parameters and let bindings (PUSH_L, STORE_L)",
    "  imports    the builtins the unit calls, looked up by name when it",
    "             loads: a VM that lacks one refuses the unit",
    "  functions  numbered: fn 0 is the first; an entry point (a file's",
    "             top level) is one the unit exports by name",
    "",
    "# The instructions",
    "",
    "  PUSH_K n       push constant n",
    "  PUSH_G n       push global n; an error when it was never set",
    "  STORE_G n      set global n to the top, which stays",
    "  PUSH_L n       push slot n of this call",
    "  STORE_L n      set slot n to the top, which stays",
    "  PUSH_UP d n    push slot n of the function d levels out (a closure",
    "                 reading a name of the function it was made in)",
    "  STORE_UP d n   set it",
    "  POP n          drop n values",
    "  JMP c -> a     jump to address a: always (c 0), when the popped test",
    "                 is false (1: if, cond), when false keeping it (2: and),",
    "                 when true keeping it (3: or); 4 only checks the last",
    "                 operand of and/or is a boolean",
    "  CALL n         call the function under its n arguments",
    "  CALLB n i      call builtin i (an import) with n arguments",
    "  RET 0          return the top from the function; from an entry point,",
    "                 end the run with it",
    "  RET 1          exit: end the run with the top, from anywhere",
    "  CLOSURE n      push function n as a value, holding on to this call's",
    "                 slots so it can read them later",
    "  TUPLE n        make a tuple of the top n values",
    "  UNPACK n       replace a tuple of exactly n by its elements (letv)",
    "  TRAP n         fail with the message in constant n: a malformed form",
    "                 fails where it would, not when compiled",
    "  PUSH_B i       push builtin i as a value: (map - xs)",
    "",
    "# The bytes",
    "",
    "x shows the unit as the file holds it, region by region, each byte in",
    "hexadecimal with its text on the right. The bytes of the instruction the",
    "run is at are in orange, and so are those of the cursor's line.",
    "",
    "An instruction is one byte, the opcode in its top five bits and a small",
    "operand in the low three: PUSH_L 0 is 3 << 3 | 0 = 0x18, CLOSURE 1 is",
    "12 << 3 | 1 = 0x61. An operand of 7 or more takes a second form: the low",
    "bits are 7 and the number follows. Numbers inside the file are ULEB128:",
    "seven bits a byte, low first, the high bit set on every byte but the last.",
    "A jump carries two bytes: the distance from its own end, signed.",
    "",
    "  header     7F 46 42 43 (\"\\x7fFBC\"), kind (1 a unit), format version,",
    "             header size, a checksum (FNV-1a) of the whole file, the",
    "             widest stack and frame, then the table of the sections:",
    "             kind, offset and length of each",
    "  imports    the names of the builtins it calls",
    "  globals    the names of its globals",
    "  constants  each a tag and its value: 1 a number (8 bytes, IEEE 754),",
    "             2 a string (length, bytes), 3 true, 4 false, 5 empty list",
    "  functions  for each: where its code starts, its length, parameters,",
    "             slots and widest stack",
    "  code       every function's instructions, one after the other",
    "  exports    the entry points: a name and a function number",
    "  debug      where each instruction came from: line and column, as",
    "             differences from the one before (the desktop's filo build",
    "             --strip leaves it out)",
    "  externs    the globals it reads and never sets: the host gives them",
    "",
    "docs/bytecode.md, in the Filo repository, has all of it.",
};

enum { HELP_LINES = sizeof(help_lines) / sizeof(help_lines[0]) };

/* ---- the unit ---- */

static uint32_t insn(uint32_t pc, char *dst, size_t cap) {
    return fbc_insn(&listing, pc, dst, cap);
}

/* Whether an instruction's text is of the instruction name. */
static bool is_op(const char *text, const char *name) {
    size_t n = strlen(name);
    if (strncmp(text, name, n) != 0) {
        return false;
    }
    if (text[n] == ' ') {
        return true;
    }
    return text[n] == '\0';
}

/* The first number after an instruction's name, 0 when there is none. */
static uint32_t operand_of(const char *text) {
    const char *p = text;
    while (*p != ' ' && *p != '\0') {
        p++;
    }
    while (*p == ' ') {
        p++;
    }
    uint32_t n = 0;
    while (*p >= '0' && *p <= '9') {
        n = (n * 10U) + (uint32_t)(*p - '0');
        p++;
    }
    return n;
}

/* How many operands an instruction takes from the top of the stack, as
   docs/bytecode.md's table has it: a store reads the top and keeps it, a
   conditional jump its test, a call its function and arguments. */
static uint32_t consumes(const char *text) {
    uint32_t count = operand_of(text);
    if (is_op(text, "STORE_G") || is_op(text, "STORE_L") || is_op(text, "STORE_UP") ||
        is_op(text, "UNPACK") || is_op(text, "RET")) {
        return 1;
    }
    if (is_op(text, "POP") || is_op(text, "TUPLE") || is_op(text, "CALLB")) {
        return count;
    }
    if (is_op(text, "CALL")) {
        return count + 1;
    }
    if (is_op(text, "JMP")) {
        return strstr(text, " always") != NULL ? 0 : 1;
    }
    return 0;
}

static bool keep_place(void *user, uint32_t pc, uint32_t line, uint32_t col) {
    (void)user;
    if (D.nplaces > 0 && places[D.nplaces - 1].pc == pc) {
        D.nplaces--; /* the last entry of a pc is its place */
    }
    if (D.nplaces == DBG_ROWS_MAX) {
        return false;
    }
    places[D.nplaces].pc = pc;
    places[D.nplaces].line = line;
    places[D.nplaces].col = col;
    D.nplaces++;
    return true;
}

/* Where the instruction at pc came from, as fbc_position says, read from
   the debug section kept in places. */
static bool position(uint32_t pc, uint32_t *line, uint32_t *col) {
    uint32_t lo = 0;
    uint32_t hi = D.nplaces;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) / 2U);
        if (places[mid].pc <= pc) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    if (lo == 0 || places[lo - 1U].line == 0) {
        return false;
    }
    *line = places[lo - 1U].line;
    *col = places[lo - 1U].col;
    return true;
}

static bool add_row(int32_t of, uint32_t pc, uint32_t len) {
    if (D.nrows == DBG_ROWS_MAX) {
        return false;
    }
    code_row *r = &rows[D.nrows];
    D.nrows++;
    r->of = of;
    r->pc = pc;
    r->len = len;
    r->line = 0;
    r->col = 0;
    if (of >= 0) {
        (void)position(pc, &r->line, &r->col);
    }
    return true;
}

/* The whole unit's code, as filo dump lists it, every function under its
   heading: the functions the run has not reached yet are there to read
   too. */
static void build_rows(void) {
    D.nrows = 0;
    for (uint32_t i = 0; i < listing.nfns; i++) {
        if (!add_row(-1, i, 0)) {
            return;
        }
        const fbc_fn *f = &listing.fns[i];
        uint32_t at = f->off;
        while (at < f->off + f->len) {
            char text[160];
            uint32_t n = insn(at, text, sizeof(text));
            if (n == 0 || !add_row((int32_t)i, at, n)) {
                break;
            }
            at += n;
        }
    }
}

/* The debug section says the line and the column, not the file; but every
   entry is compiled from the file it is named after, and every other
   function is made by a CLOSURE inside one, so walking them from each entry
   says whose it is. */
static void own(uint32_t root, int16_t file) {
    static uint32_t stack[FBC_FNS_MAX];
    if (root >= listing.nfns || D.file_of[root] >= 0) {
        return;
    }
    D.file_of[root] = file;
    stack[0] = root;
    uint32_t n = 1;
    while (n > 0) {
        n--;
        const fbc_fn *f = &listing.fns[stack[n]];
        uint32_t at = f->off;
        while (at < f->off + f->len) {
            char text[160];
            uint32_t len = insn(at, text, sizeof(text));
            if (len == 0) {
                break;
            }
            uint32_t inner = operand_of(text);
            if (is_op(text, "CLOSURE") && inner < listing.nfns && D.file_of[inner] < 0) {
                D.file_of[inner] = file;
                stack[n] = inner;
                n++;
            }
            at += len;
        }
    }
}

/* The entry point function fn is, in dst; false for another function. */
static bool entry_name(uint32_t fn, char *dst, size_t cap) {
    for (uint32_t i = 0; i < listing.nexports; i++) {
        if (listing.export_fns[i] == fn) {
            const fbc_span *s = &listing.export_names[i];
            (void)snprintf(dst, cap, "%.*s", (int)s->len, (const char *)listing.data + s->off);
            return true;
        }
    }
    return false;
}

static void read_source(source *s) {
    char path[VFS_PATH_MAX + 80];
    size_t n = strlen(D.dir);
    const char *sep = n > 0 && D.dir[n - 1] == '/' ? "" : "/";
    (void)snprintf(path, sizeof(path), "%s%s%s", D.dir, sep, s->name);
    s->first = D.nlines;
    s->nlines = 0;
    s->found = roc_find_file(D.m, path, &s->text, &s->len);
    if (!s->found) {
        return;
    }
    if (s->len >= 3 && memcmp(s->text, "\xef\xbb\xbf", 3) == 0) {
        s->text += 3;
        s->len -= 3;
    }
    if (s->len > 0 && s->text[s->len - 1] == '\n') {
        s->len--;
    }
    line_at[D.nlines] = 0;
    D.nlines++;
    s->nlines++;
    for (size_t at = 0; at < s->len && D.nlines < DBG_LINES_MAX; at++) {
        if (s->text[at] == '\n') {
            line_at[D.nlines] = (uint32_t)at + 1U;
            D.nlines++;
            s->nlines++;
        }
    }
}

static void read_sources(void) {
    D.nlines = 0;
    memset(breaks, 0, sizeof(bool) * DBG_LINES_MAX);
    for (uint32_t i = 0; i < FBC_FNS_MAX; i++) {
        D.file_of[i] = -1;
    }
    for (uint32_t i = 0; i < listing.nexports; i++) {
        const fbc_span *s = &listing.export_names[i];
        (void)snprintf(files[i].name, sizeof(files[i].name), "%.*s.filo", (int)s->len,
                       (const char *)listing.data + s->off);
        read_source(&files[i]);
        own(listing.export_fns[i], (int16_t)i);
    }
}

/* Line n (from 1) of a source, without its newline. */
static void line_of(const source *s, uint32_t n, const uint8_t **p, size_t *len) {
    size_t start = line_at[s->first + n - 1U];
    size_t end = s->len;
    if (n < s->nlines) {
        end = line_at[s->first + n] - 1U;
    }
    if (end > start && s->text[end - 1] == '\r') {
        end--;
    }
    *p = s->text + start;
    *len = end - start;
}

/* ---- the run ---- */

typedef struct {
    char *dst;
    size_t cap;
    size_t at;
} text_out;

/* False once dst is full. */
static bool text_put(text_out *o, const char *s, size_t n) {
    size_t room = o->cap - 1U - o->at;
    if (n > room) {
        n = room;
    }
    memcpy(o->dst + o->at, s, n);
    o->at += n;
    o->dst[o->at] = '\0';
    return o->at + 1U < o->cap;
}

/* v as the Go engine's Value.String writes it, a function as <fn> (the
   runtime's repr refuses one), cut where o is full. */
static bool show_value(text_out *o, const filo_value *v, uint32_t level) {
    if (v->kind == FILO_FUNC) {
        return text_put(o, "<fn>", 4);
    }
    if (v->kind != FILO_LIST && v->kind != FILO_TUPLE) {
        char text[DBG_VALUE_MAX];
        size_t n = 0;
        if (filo_value_repr(D.ctx, v, text, sizeof(text), &n) != FILO_OK) {
            return text_put(o, "<?>", 3);
        }
        return text_put(o, text, n < sizeof(text) ? n : sizeof(text) - 1U);
    }
    const char *open = v->kind == FILO_LIST ? "(list" : "(tuple";
    if (!text_put(o, open, strlen(open))) {
        return false;
    }
    if (level > DBG_LEVELS_MAX && v->u.seq.len > 0) {
        return text_put(o, " \xe2\x80\xa6)", 5);
    }
    for (uint32_t i = 0; i < v->u.seq.len; i++) {
        /* no items: a range, 0 to len-1 */
        filo_value item = v->u.seq.items != NULL ? v->u.seq.items[i] : filo_num((double)i);
        if (!text_put(o, " ", 1) || !show_value(o, &item, level + 1U)) {
            return false;
        }
    }
    return text_put(o, ")", 1);
}

static void value_text(const filo_value *v, char *dst, size_t cap) {
    text_out o = {dst, cap, 0};
    dst[0] = '\0';
    (void)show_value(&o, v, 0);
}

/* The frames the screen shows: the run's, or once it ended, the ones it
   had before its last step, so the source stays and an error shows its
   place. The values are the VM's, which the end does not release. */
static const filo_bc_frame *shown(uint32_t *n) {
    if (D.ncur > 0 || D.nlast == 0) {
        *n = D.ncur;
        return D.cur;
    }
    *n = D.nlast;
    return D.last;
}

static void shown_at(uint32_t *line, uint32_t *col) {
    *line = 0;
    *col = 0;
    uint32_t n = 0;
    const filo_bc_frame *f = shown(&n);
    if (n == 0) {
        return;
    }
    (void)position(f[0].pc, line, col);
    if (D.ncur == 0 && D.err_line > 0) {
        *line = D.err_line;
        *col = D.err_col;
    }
}

/* The source of the function shown, -1 when there is none. */
static int32_t file_shown(void) {
    uint32_t n = 0;
    const filo_bc_frame *f = shown(&n);
    if (n == 0 || f[0].fn >= FBC_FNS_MAX) {
        return -1;
    }
    return D.file_of[f[0].fn];
}

static const char *file_name(int32_t f) {
    return f >= 0 ? files[f].name : "";
}

static void set_ending(int rc, const filo_value *v) {
    D.err_line = 0;
    D.err_col = 0;
    if (rc == FILO_OK && v != NULL) {
        char text[DBG_VALUE_MAX];
        value_text(v, text, sizeof(text));
        (void)snprintf(D.ending, sizeof(D.ending), "returned %s", text);
        return;
    }
    uint32_t line = 0;
    uint32_t col = 0;
    if (!filo_error_at(D.ctx, &line, &col)) {
        (void)snprintf(D.ending, sizeof(D.ending), "error: %s", filo_error(D.ctx));
        return;
    }
    D.err_line = line;
    D.err_col = col;
    (void)snprintf(D.ending, sizeof(D.ending), "error at %s %u:%u: %s", file_name(file_shown()),
                   line, col, filo_error(D.ctx));
}

/* What the program writes, while it runs: the last line kept, escape
   sequences left out. */
static bool out_sink(void *ctx, const uint8_t *data, size_t n) {
    (void)ctx;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = data[i];
        if (D.out_esc == 1) {
            D.out_esc = b == '[' ? 2 : 0;
            continue;
        }
        if (D.out_esc == 2) {
            D.out_esc = b >= 0x40 && b <= 0x7e ? 0 : 2;
            continue;
        }
        if (b == 0x1b) {
            D.out_esc = 1;
            continue;
        }
        if (b == '\n') {
            D.out_ended = true;
            continue;
        }
        if (b < 0x20) {
            continue;
        }
        if (D.out_ended) {
            D.out_len = 0;
            D.out_ended = false;
        }
        if (D.out_len + 1 < sizeof(D.out)) {
            D.out[D.out_len] = (char)b;
            D.out_len++;
        }
    }
    return true;
}

static void capture(void) {
    D.ncur = filo_bc_frames(D.ctx, D.cur, DBG_FRAMES_MAX);
}

/* Runs budget instructions of the program's own (0: to the end), its
   output to the debugger. */
static void go(uint32_t budget) {
    term *t = &D.m->t;
    bool (*sink)(void *, const uint8_t *, size_t) = t->sink;
    void *sink_ctx = t->sink_ctx;
    t->sink = out_sink;
    t->sink_ctx = NULL;
    filo_value v = {0};
    int rc = filo_bc_resume(D.ctx, budget, &v);
    t->sink = sink;
    t->sink_ctx = sink_ctx;
    D.steps = D.ctx->steps;
    if (rc == FILO_PAUSED) {
        capture();
        return;
    }
    D.ncur = 0;
    D.done = true;
    set_ending(rc, &v);
    (void)snprintf(D.msg, sizeof(D.msg), "%s", D.ending);
}

/* One instruction; false when the run had ended. */
static bool step(void) {
    if (D.done) {
        return false;
    }
    memcpy(D.last, D.cur, sizeof(D.cur));
    D.nlast = D.ncur;
    go(1);
    return true;
}

/* On to the first instruction at or past target steps. */
static void run_to(uint32_t target) {
    if (!D.done && D.steps < target) {
        memcpy(D.last, D.cur, sizeof(D.cur));
        D.nlast = D.ncur;
        go(target - D.steps);
    }
}

static void follow(void) {
    uint32_t line = 0;
    uint32_t col = 0;
    shown_at(&line, &col);
    if (line > 0) {
        D.cursor = line;
    }
}

/* A fresh context, the unit loaded, the run held before its first
   instruction. False, with the reason in D.ending, when the unit does not
   load. */
static bool restart(void) {
    D.steps = 0;
    D.msg[0] = '\0';
    D.ncur = 0;
    D.nlast = 0;
    D.done = false;
    D.ending[0] = '\0';
    D.err_line = 0;
    D.out_len = 0;
    D.out_esc = 0;
#if ROC_APP_SCREENS
    if (D.app) {
        screen_context_init(D.m, &app_ctx, app_persistent, SCR_MEM_PERSISTENT, app_run,
                            SCR_MEM_RUN);
        D.ctx = &app_ctx;
    } else {
        D.ctx = script_context(D.m);
    }
#else
    D.ctx = script_context(D.m);
#endif
    filo_value none = {0};
    if (!D.app && filo_list(D.ctx, NULL, 0, &none) == FILO_OK) {
        (void)filo_set_global(D.ctx, "ARGS", none); /* a command run with no arguments */
    }
    if (filo_bc_load(D.ctx, unit_mem, D.ulen, &D.loaded) != FILO_OK) {
        D.done = true;
        (void)snprintf(D.ending, sizeof(D.ending), "%s", filo_error(D.ctx));
        return false;
    }
    filo_limits limits = {SC_STEPS, 0};
    if (filo_bc_begin(D.ctx, D.loaded, D.entry, &limits) != FILO_PAUSED) {
        D.done = true;
        set_ending(FILO_ERR, NULL);
        return true;
    }
    capture();
    follow();
    return true;
}

/* Runs a movement, remembering where it began so back can undo it. */
static bool move_begin(void) {
    D.msg[0] = '\0';
    if (D.done) {
        (void)snprintf(D.msg, sizeof(D.msg), "the run has ended: r starts again, b goes back");
        return false;
    }
    if (D.nmarks == DBG_MARKS_MAX) {
        memmove(D.marks, D.marks + 1, sizeof(D.marks) - sizeof(D.marks[0]));
        D.nmarks--;
    }
    D.marks[D.nmarks] = D.steps;
    D.nmarks++;
    return true;
}

static uint32_t line_now(void) {
    uint32_t line = 0;
    uint32_t col = 0;
    if (D.ncur > 0) {
        (void)position(D.cur[0].pc, &line, &col);
    }
    return line;
}

/* Runs until the source line changes: into calls, or over them when over,
   as gdb's step and next. */
static void by_line(bool over) {
    if (!move_begin()) {
        return;
    }
    uint32_t start = line_now();
    uint32_t depth = D.ncur;
    if (start == 0) { /* a stripped unit has no lines: an instruction is the step */
        (void)step();
        follow();
        return;
    }
    while (step()) {
        if (D.done) {
            break;
        }
        if (over && D.ncur > depth) {
            continue;
        }
        uint32_t line = line_now();
        if (line != 0 && (line != start || D.ncur != depth)) {
            break;
        }
    }
    follow();
}

static void instruction(void) {
    if (move_begin()) {
        (void)step();
        follow();
    }
}

static bool has_breaks(void) {
    for (uint32_t i = 0; i < D.nlines; i++) {
        if (breaks[i]) {
            return true;
        }
    }
    return false;
}

/* To the end: once in long strides to know the last pause before it, then
   again from the start to that pause and on one instruction at a time, so
   the screen shows where the run ended. Stepping all the way, the calls
   read at every instruction, is what it saves. */
static void to_end(void) {
    uint32_t before = D.steps;
    while (!D.done) {
        before = D.steps;
        go(DBG_STRIDE);
    }
    (void)restart();
    run_to(before);
    while (step()) {
    }
}

/* Runs to the end, or until the run enters a line with a breakpoint: comes
   to it from another line, or a call starts on it. */
static void finish(void) {
    if (!move_begin()) {
        return;
    }
    if (!has_breaks()) {
        to_end();
        follow();
        return;
    }
    uint32_t line = line_now();
    uint32_t depth = D.ncur;
    while (step()) {
        if (D.done) {
            break;
        }
        uint32_t now = line_now();
        bool entered = false;
        if (now != line || D.ncur != depth) {
            entered = true;
        }
        line = now;
        depth = D.ncur;
        int32_t f = D.cur[0].fn < FBC_FNS_MAX ? D.file_of[D.cur[0].fn] : -1;
        if (entered && f >= 0 && now > 0 && now <= files[f].nlines &&
            breaks[files[f].first + now - 1U]) {
            (void)snprintf(D.msg, sizeof(D.msg), "breakpoint at %s:%u", files[f].name, now);
            break;
        }
    }
    follow();
}

static void back(void) {
    if (D.nmarks == 0) {
        (void)snprintf(D.msg, sizeof(D.msg), "at the start");
        return;
    }
    D.nmarks--;
    uint32_t target = D.marks[D.nmarks];
    (void)restart();
    run_to(target);
    follow();
}

static void start_over(void) {
    D.nmarks = 0;
    (void)restart();
}

static void move_cursor(int32_t n) {
    int32_t f = file_shown();
    int32_t lines = f >= 0 ? (int32_t)files[f].nlines : 0;
    int32_t at = (int32_t)D.cursor + n;
    if (at > lines) {
        at = lines;
    }
    if (at < 1) {
        at = 1;
    }
    D.cursor = (uint32_t)at;
}

static void toggle_break(void) {
    int32_t f = file_shown();
    if (f < 0 || D.cursor < 1 || D.cursor > files[f].nlines) {
        return;
    }
    uint32_t i = files[f].first + D.cursor - 1U;
    if (breaks[i]) {
        breaks[i] = false;
        return;
    }
    breaks[i] = true;
}

/* ---- drawing ---- */

static int32_t put(canvas *c, int32_t row, int32_t col, const char *s) {
    return cv_put(c, row, col, cv_cstr(s));
}

static size_t rune_len(uint8_t b) {
    if (b >= 0xf0) {
        return 4;
    }
    if (b >= 0xe0) {
        return 3;
    }
    if (b >= 0xc0) {
        return 2;
    }
    return 1;
}

/* s cut to cols runes. */
static int32_t put_cut(canvas *c, int32_t row, int32_t col, const char *s, int32_t cols) {
    if (cols <= 0) {
        return 0;
    }
    size_t n = strlen(s);
    size_t at = 0;
    int32_t k = 0;
    while (at < n && k < cols) {
        size_t r = rune_len((uint8_t)s[at]);
        at += r < n - at ? r : n - at;
        k++;
    }
    cv_text t = {(const uint8_t *)s, at};
    return cv_put(c, row, col, t);
}

static int32_t max32(int32_t a, int32_t b) {
    return a > b ? a : b;
}

static int32_t min32(int32_t a, int32_t b) {
    return a < b ? a : b;
}

/* The columns the first n bytes of a line take once its tabs are
   expanded: a column in the debug section counts bytes. */
static int32_t columns(const uint8_t *p, size_t len, size_t n) {
    int32_t col = 0;
    size_t at = 0;
    while (at < len && at < n) {
        if (p[at] == '\t') {
            col = ((col / DBG_TAB) + 1) * DBG_TAB;
            at++;
            continue;
        }
        at += rune_len(p[at]);
        col++;
    }
    return col;
}

/* A line with its tabs expanded, in dst (cap bytes); its length. */
static size_t expand(const uint8_t *p, size_t len, uint8_t *dst, size_t cap) {
    size_t n = 0;
    int32_t col = 0;
    size_t at = 0;
    while (at < len && n + 4 < cap) {
        if (p[at] == '\t') {
            do {
                dst[n] = ' ';
                n++;
                col++;
            } while (col % DBG_TAB != 0 && n + 4 < cap);
            at++;
            continue;
        }
        size_t r = rune_len(p[at]);
        r = r < len - at ? r : len - at;
        memcpy(dst + n, p + at, r);
        n += r;
        at += r;
        col++;
    }
    return n;
}

static void mark_class(void *user, hl_class cls, size_t from, size_t to) {
    uint8_t *classes = user;
    for (size_t k = from; k < to; k++) {
        classes[k] = (uint8_t)cls;
    }
}

/* text from its rune skip on, at most cols runes, each in its class's
   colour over bg. */
static void put_coloured(canvas *c, int32_t row, int32_t col, const uint8_t *text, size_t len,
                         const uint8_t *cls, int32_t skip, int32_t cols, int16_t bg) {
    int32_t at = col;
    int32_t k = 0;
    int32_t drawn = 0;
    size_t i = 0;
    while (i < len && drawn < cols) {
        size_t r = rune_len(text[i]);
        r = r < len - i ? r : len - i;
        if (k >= skip) {
            uint8_t cl = cls[i] < 5 ? cls[i] : 0;
            cv_pen(c, class_fg[cl], bg, 0);
            cv_text t = {text + i, r};
            at += cv_put(c, row, at, t);
            drawn++;
        }
        k++;
        i += r;
    }
}

static void draw_source(canvas *c, uint32_t fn, bool any, uint32_t line, uint32_t col,
                        int32_t nrows, int32_t width) {
    int32_t f = any && fn < FBC_FNS_MAX ? D.file_of[fn] : -1;
    cv_pen(c, COL_DIM, CV_COLOR_DEFAULT, 0);
    (void)put(c, 0, 1, file_name(f));
    if (f < 0) {
        return;
    }
    const source *s = &files[f];
    if (!s->found) {
        char text[160];
        (void)snprintf(text, sizeof(text),
                       "(no source: %s is not beside the unit; -src DIR says where)", s->name);
        /* wrapped at spaces, as the desktop's */
        char words[160];
        size_t ll = 0;
        int32_t row = 2;
        size_t at = 0;
        size_t tn = strlen(text);
        while (at < tn && row < nrows) {
            size_t end = at;
            while (end < tn && text[end] != ' ') {
                end++;
            }
            size_t wl = end - at;
            if (ll > 0 && (int32_t)(ll + 1 + wl) > width - 2) {
                words[ll] = '\0';
                (void)put(c, row, 1, words);
                row++;
                ll = 0;
            }
            if (ll > 0 && ll + 1 < sizeof(words)) {
                words[ll] = ' ';
                ll++;
            }
            if (wl > sizeof(words) - 1 - ll) {
                wl = sizeof(words) - 1 - ll;
            }
            memcpy(words + ll, text + at, wl);
            ll += wl;
            at = end + 1;
        }
        if (ll > 0 && row < nrows) {
            words[ll] = '\0';
            (void)put(c, row, 1, words);
        }
        return;
    }
    int32_t first = max32(1, (int32_t)D.cursor - ((nrows - 1) / 2));
    /* the text scrolls sideways, as the edt's, to keep the column in view */
    int32_t shift = 0;
    const uint8_t *p = NULL;
    size_t len = 0;
    if (line >= 1 && line <= s->nlines && col > 0) {
        line_of(s, line, &p, &len);
        int32_t at = columns(p, len, col - 1U);
        int32_t room = width - 7;
        if (at >= room - 2) {
            shift = at - (room / 2);
        }
    }
    static uint8_t text[DBG_TEXT_MAX];
    static uint8_t cls[DBG_TEXT_MAX];
    hl_state hs;
    hl_begin(&hs, HL_FILO);
    for (uint32_t n = 1; (int32_t)n < first && n <= s->nlines; n++) {
        line_of(s, n, &p, &len);
        size_t tl = expand(p, len, text, sizeof(text));
        hl_spans(&hs, text, tl, NULL, NULL);
    }
    for (int32_t row = 1; row < nrows; row++) {
        uint32_t n = (uint32_t)(first + row - 1);
        if (n > s->nlines) {
            break;
        }
        bool here = n == line;
        int16_t bg = CV_COLOR_DEFAULT;
        if (here) {
            bg = COL_BAR;
            cv_pen(c, CV_COLOR_DEFAULT, COL_BAR, 0);
            cv_fill(c, row, 0, 1, width, cv_cstr(" "));
            cv_pen(c, COL_KEYS, COL_BAR, CV_A_BOLD);
        } else {
            cv_pen(c, COL_DIM, CV_COLOR_DEFAULT, 0);
        }
        char num[16];
        (void)snprintf(num, sizeof(num), "%5u ", n);
        (void)put(c, row, 0, num);
        line_of(s, n, &p, &len);
        size_t tl = expand(p, len, text, sizeof(text));
        memset(cls, 0, tl);
        hl_spans(&hs, text, tl, mark_class, cls);
        put_coloured(c, row, 6, text, tl, cls, shift, width - 7, bg);
        if (here && col > 0) {
            int32_t exp = columns(p, len, col - 1U);
            int32_t at = 6 + exp - shift;
            if (at >= 6 && at < width) {
                /* the rune at column exp of the expanded line, or a space */
                size_t i = 0;
                int32_t k = 0;
                while (i < tl && k < exp) {
                    i += rune_len(text[i]);
                    k++;
                }
                cv_text r = cv_cstr(" ");
                if (i < tl) {
                    size_t rl = rune_len(text[i]);
                    r.ptr = text + i;
                    r.len = rl < tl - i ? rl : tl - i;
                }
                cv_pen(c, COL_HERE_FG, COL_HERE_BG, 0);
                (void)cv_put(c, row, at, r);
            }
        }
    }
}

/* The breakpoints (red dots) and the cursor (an orange arrow, where it is
   not on the run's line) in the source's margin. */
static void draw_marks(canvas *c, uint32_t fn, bool any, uint32_t line, int32_t nrows) {
    int32_t f = any && fn < FBC_FNS_MAX ? D.file_of[fn] : -1;
    if (f < 0 || !files[f].found) {
        return;
    }
    const source *s = &files[f];
    int32_t first = max32(1, (int32_t)D.cursor - ((nrows - 1) / 2));
    for (int32_t row = 1; row < nrows; row++) {
        uint32_t n = (uint32_t)(first + row - 1);
        if (n > s->nlines) {
            break;
        }
        int16_t bg = n == line ? COL_BAR : CV_COLOR_DEFAULT;
        if (breaks[s->first + n - 1U]) {
            cv_pen(c, COL_BREAK, bg, 0);
            (void)put(c, row, 0, "\xe2\x97\x8f"); /* ● */
        }
        if (n == D.cursor && n != line) {
            cv_pen(c, COL_KEYS, bg, CV_A_BOLD);
            (void)put(c, row, 1, "\xe2\x96\xb8"); /* ▸ */
        }
    }
}

static void row_text(const code_row *r, char *dst, size_t cap) {
    if (r->of < 0) {
        const fbc_fn *f = &listing.fns[r->pc];
        char name[64];
        if (entry_name(r->pc, name, sizeof(name))) {
            (void)snprintf(dst, cap, " fn %u (entry \"%s\"): params %u, slots %u", r->pc, name,
                           f->params, f->slots);
            return;
        }
        (void)snprintf(dst, cap, " fn %u: params %u, slots %u", r->pc, f->params, f->slots);
        return;
    }
    char at[24] = "";
    if (r->line > 0) {
        (void)snprintf(at, sizeof(at), "%u:%u", r->line, r->col);
    }
    char text[160];
    (void)insn(r->pc, text, sizeof(text));
    (void)snprintf(dst, cap, " %04u %-6s %s", r->pc, at, text);
}

/* Whether a row is one of the instructions of the cursor's line, while the
   cursor is off the run's line. */
static bool cursor_row(const code_row *r, int32_t file) {
    if (r->of < 0 || r->line != D.cursor) {
        return false;
    }
    return D.file_of[r->of] == file;
}

static void draw_code(canvas *c, uint32_t fn, uint32_t pc, uint32_t line, int32_t nrows,
                      int32_t col, int32_t w) {
    int32_t width = w - col;
    char label[96];
    (void)snprintf(label, sizeof(label), " at fn %u", fn);
    char name[64];
    if (entry_name(fn, name, sizeof(name))) {
        size_t n = strlen(label);
        (void)snprintf(label + n, sizeof(label) - n, " (entry \"%s\")", name);
    }
    cv_pen(c, COL_DIM, CV_COLOR_DEFAULT, 0);
    (void)put_cut(c, 0, col, label, width);
    uint32_t here = 0;
    for (uint32_t i = 0; i < D.nrows; i++) {
        if (rows[i].of >= 0 && rows[i].pc == pc) {
            here = i;
            break;
        }
    }
    /* the cursor moved off the run's line: its instructions, in view and
       marked, so the arrows read the code the source makes */
    int32_t file = fn < FBC_FNS_MAX ? D.file_of[fn] : -1;
    bool off = D.cursor != line;
    if (off) {
        for (uint32_t i = 0; i < D.nrows; i++) {
            if (cursor_row(&rows[i], file)) {
                here = i;
                break;
            }
        }
    }
    int32_t first = max32(0, (int32_t)here - ((nrows - 2) / 2));
    for (int32_t row = 1; row < nrows && (uint32_t)(first + row - 1) < D.nrows; row++) {
        const code_row *r = &rows[first + row - 1];
        if (r->of < 0) {
            cv_pen(c, COL_DIM, CV_COLOR_DEFAULT, 0);
        } else if (r->pc == pc) {
            cv_pen(c, COL_HERE_FG, COL_HERE_BG, 0);
            cv_fill(c, row, col, 1, width, cv_cstr(" "));
        } else if (off && cursor_row(r, file)) {
            cv_pen(c, COL_KEYS, CV_COLOR_DEFAULT, 0);
        } else {
            cv_pen(c, CV_COLOR_DEFAULT, CV_COLOR_DEFAULT, 0);
        }
        char text[256];
        row_text(r, text, sizeof(text));
        (void)put_cut(c, row, col, text, width);
    }
}

/* ---- the bytes ---- */

typedef struct {
    char name[16];
    uint32_t off;
    uint32_t len;
} region;

/* The unit as the file holds it, region by region: the header, each
   section in the order of the file, and what lies between or after. */
static uint32_t regions(region *out) {
    fbc_section secs[FBC_SECTIONS_MAX];
    uint32_t ns = listing.nsections;
    memcpy(secs, listing.sections, sizeof(secs[0]) * ns);
    for (uint32_t i = 1; i < ns; i++) {
        fbc_section s = secs[i];
        uint32_t j = i;
        while (j > 0 && secs[j - 1].span.off > s.span.off) {
            secs[j] = secs[j - 1];
            j--;
        }
        secs[j] = s;
    }
    uint32_t n = 0;
    (void)snprintf(out[n].name, sizeof(out[n].name), "header");
    out[n].off = 0;
    out[n].len = listing.header_size;
    n++;
    uint32_t at = listing.header_size;
    for (uint32_t i = 0; i < ns; i++) {
        const fbc_span *sp = &secs[i].span;
        if (sp->off > at) {
            (void)snprintf(out[n].name, sizeof(out[n].name), "between");
            out[n].off = at;
            out[n].len = sp->off - at;
            n++;
        }
        fbc_section_name(secs[i].kind, out[n].name, sizeof(out[n].name));
        out[n].off = sp->off;
        out[n].len = sp->len;
        n++;
        if (sp->off + sp->len > at) {
            at = sp->off + sp->len;
        }
    }
    if (at < listing.len) {
        (void)snprintf(out[n].name, sizeof(out[n].name), "after");
        out[n].off = at;
        out[n].len = (uint32_t)listing.len - at;
        n++;
    }
    return n;
}

/* Row k of the bytes view: a region's heading (*n is 0) or *n bytes from
 *off; false past the last. */
static bool byte_row(const region *rg, uint32_t nrg, uint32_t per, uint32_t k, uint32_t *which,
                     uint32_t *off, uint32_t *n) {
    for (uint32_t i = 0; i < nrg; i++) {
        uint32_t count = 1U + ((rg[i].len + per - 1U) / per);
        if (k < count) {
            *which = i;
            *off = rg[i].off;
            *n = 0;
            if (k > 0) {
                *off = rg[i].off + ((k - 1U) * per);
                uint32_t left = rg[i].off + rg[i].len - *off;
                *n = left < per ? left : per;
            }
            return true;
        }
        k -= count;
    }
    return false;
}

/* The byte row holding off, 0 when none does. */
static uint32_t byte_row_of(const region *rg, uint32_t nrg, uint32_t per, uint32_t off) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < nrg; i++) {
        uint32_t count = 1U + ((rg[i].len + per - 1U) / per);
        if (off >= rg[i].off && off < rg[i].off + rg[i].len) {
            return k + 1U + ((off - rg[i].off) / per);
        }
        k += count;
    }
    return 0;
}

/* The x view of the right side: the bytes of the unit, the instruction the
   run is at in orange under black, the cursor's line's in orange, kept in
   view as the code's rows are. */
static void draw_bytes(canvas *c, uint32_t fn, uint32_t pc, uint32_t line, int32_t nrows,
                       int32_t col, int32_t w) {
    int32_t width = w - col;
    uint32_t per = 4;
    while (per < 16 && 9 + (8 * (int32_t)per) <= width) { /* " 0000  " hex, a space, text */
        per *= 2;
    }
    uint32_t code = listing.code.off;
    uint32_t here = code + pc;
    uint32_t here_len = 0;
    uint32_t target = here;
    static uint32_t spans[DBG_SPANS_MAX][2];
    uint32_t nspans = 0;
    int32_t file = fn < FBC_FNS_MAX ? D.file_of[fn] : -1;
    bool off = D.cursor != line;
    for (uint32_t i = 0; i < D.nrows; i++) {
        const code_row *r = &rows[i];
        if (r->of < 0) {
            continue;
        }
        if (r->pc == pc) {
            here_len = r->len;
        }
        if (off && cursor_row(r, file) && nspans < DBG_SPANS_MAX) {
            if (nspans == 0) {
                target = code + r->pc;
            }
            spans[nspans][0] = code + r->pc;
            spans[nspans][1] = code + r->pc + r->len;
            nspans++;
        }
    }
    char label[96];
    (void)snprintf(label, sizeof(label), " at fn %u, byte %04x of %zu", fn, here, listing.len);
    cv_pen(c, COL_DIM, CV_COLOR_DEFAULT, 0);
    (void)put_cut(c, 0, col, label, width);
    region rg[(2U * FBC_SECTIONS_MAX) + 2U];
    uint32_t nrg = regions(rg);
    int32_t at = (int32_t)byte_row_of(rg, nrg, per, target);
    int32_t first = max32(0, at - ((nrows - 2) / 2));
    for (int32_t row = 1; row < nrows; row++) {
        uint32_t which = 0;
        uint32_t from = 0;
        uint32_t n = 0;
        if (!byte_row(rg, nrg, per, (uint32_t)(first + row - 1), &which, &from, &n)) {
            break;
        }
        char text[96];
        if (n == 0) {
            (void)snprintf(text, sizeof(text), " %-10s %04x  %u bytes", rg[which].name, from,
                           rg[which].len);
            cv_pen(c, COL_DIM, CV_COLOR_DEFAULT, 0);
            (void)put_cut(c, row, col, text, width);
            continue;
        }
        cv_pen(c, CV_COLOR_DEFAULT, CV_COLOR_DEFAULT, 0);
        (void)snprintf(text, sizeof(text), " %04x  ", from);
        int32_t x = col + put(c, row, col, text);
        int32_t tx = col + 9 + (3 * (int32_t)per);
        for (uint32_t k = 0; k < n; k++) {
            uint32_t b = from + k;
            int16_t fg = CV_COLOR_DEFAULT;
            int16_t bg = CV_COLOR_DEFAULT;
            if (b >= here && b < here + here_len) {
                fg = COL_HERE_FG;
                bg = COL_HERE_BG;
            } else {
                for (uint32_t s = 0; s < nspans; s++) {
                    if (b >= spans[s][0] && b < spans[s][1]) {
                        fg = COL_KEYS;
                    }
                }
            }
            cv_pen(c, fg, bg, 0);
            uint8_t v = listing.data[b];
            (void)snprintf(text, sizeof(text), "%02x", v);
            (void)put(c, row, x, text);
            const char ch[2] = {(char)(v < 0x20 || v > 0x7e ? '.' : v), '\0'};
            (void)put(c, row, tx + (int32_t)k, ch);
            x += 3;
        }
    }
}

/* ---- the calls ---- */

/* "(v1 v2 ...)" into dst. */
static void values(const filo_value *vs, uint32_t n, char *dst, size_t cap) {
    size_t at = 0;
    dst[at++] = '(';
    for (uint32_t i = 0; i < n && at + 2 < cap; i++) {
        if (i > 0) {
            dst[at++] = ' ';
        }
        char text[DBG_VALUE_MAX];
        value_text(&vs[i], text, sizeof(text));
        size_t len = strlen(text);
        if (len > cap - at - 2) {
            len = cap - at - 2;
        }
        memcpy(dst + at, text, len);
        at += len;
    }
    dst[at++] = ')';
    dst[at] = '\0';
}

/* Where frame i of n is in its source: the next instruction for the one
   running, the call it waits on for the others. */
static void place_of(const filo_bc_frame *f, uint32_t i, char *dst, size_t cap) {
    uint32_t pc = f->pc;
    if (i > 0 && pc > 0) {
        pc--; /* after the call: the call's own place */
    }
    int32_t file = f->fn < FBC_FNS_MAX ? D.file_of[f->fn] : -1;
    uint32_t line = 0;
    uint32_t col = 0;
    if (!position(pc, &line, &col)) {
        (void)snprintf(dst, cap, "%s", file_name(file));
        return;
    }
    (void)snprintf(dst, cap, "%s %u:%u", file_name(file), line, col);
}

typedef struct {
    canvas *c;
    int32_t row;
    int32_t col;
    int32_t end;
} writer;

static void write_part(writer *w, const char *text, int16_t fg, uint8_t attr) {
    if (w->col >= w->end) {
        return;
    }
    cv_pen(w->c, fg, CV_COLOR_DEFAULT, attr);
    w->col += put_cut(w->c, w->row, w->col, text, w->end - w->col);
}

/* The running call's slots and operands, the operands its next
   instruction takes in orange, and which instruction that is: the stack is
   where the arguments of a call come from. */
static void draw_operands(canvas *c, int32_t row, int32_t col, int32_t w, const filo_bc_frame *f) {
    char text[160] = "";
    uint32_t takes = 0;
    if (insn(f->pc, text, sizeof(text)) > 0) {
        takes = consumes(text);
    }
    if (takes > f->noperands) {
        takes = f->noperands;
    }
    writer wr = {c, row, col, w - 1};
    static char slots[1024];
    values(f->slots, f->nslots, slots, sizeof(slots));
    write_part(&wr, "slots ", CV_COLOR_DEFAULT, 0);
    write_part(&wr, slots, CV_COLOR_DEFAULT, 0);
    write_part(&wr, "  operands (", CV_COLOR_DEFAULT, 0);
    for (uint32_t k = 0; k < f->noperands; k++) {
        if (k > 0) {
            write_part(&wr, " ", CV_COLOR_DEFAULT, 0);
        }
        char v[DBG_VALUE_MAX];
        value_text(&f->operands[k], v, sizeof(v));
        if (k >= f->noperands - takes) {
            write_part(&wr, v, COL_KEYS, CV_A_BOLD);
        } else {
            write_part(&wr, v, CV_COLOR_DEFAULT, 0);
        }
    }
    write_part(&wr, ")", CV_COLOR_DEFAULT, 0);
    if (takes > 0) {
        size_t k = 0;
        while (text[k] != ' ' && text[k] != '\0') {
            k++;
        }
        char name[16];
        (void)snprintf(name, sizeof(name), "%.*s", (int)k, text);
        char note[48];
        (void)snprintf(note, sizeof(note), "  %s takes %u", name, takes);
        write_part(&wr, note, COL_DIM, 0);
    }
}

static void draw_stack(canvas *c, const filo_bc_frame *f, uint32_t n, int32_t top, int32_t nrows,
                       int32_t w) {
    cv_pen(c, COL_RULE, CV_COLOR_DEFAULT, 0);
    cv_fill(c, top, 0, 1, w, cv_cstr("\xe2\x94\x80")); /* ─ */
    cv_pen(c, COL_DIM, CV_COLOR_DEFAULT, 0);
    int32_t used = 1 + put(c, top, 1, " calls, innermost first ");
    if (D.out_len > 0) {
        char out[DBG_OUT_MAX + 16];
        (void)snprintf(out, sizeof(out), " out: %.*s ", (int)D.out_len, D.out);
        int32_t room = w - used - 2;
        int32_t len = min32((int32_t)strlen(out), room);
        if (len > 8) {
            cv_pen(c, COL_WORDS, CV_COLOR_DEFAULT, 0);
            (void)put_cut(c, top, w - 1 - len, out, len);
        }
    }
    int32_t row = top + 1;
    for (uint32_t i = 0; i < n && row < top + nrows; i++) {
        if ((row == top + nrows - 1 && i < n - 1U) || i >= DBG_FRAMES_MAX) {
            char more[48];
            (void)snprintf(more, sizeof(more), "\xe2\x80\xa6 %u more calls", n - i);
            cv_pen(c, COL_DIM, CV_COLOR_DEFAULT, 0);
            (void)put(c, row, 1, more);
            break;
        }
        char label[256];
        (void)snprintf(label, sizeof(label), "#%u fn %u", i, f[i].fn);
        char name[64];
        if (entry_name(f[i].fn, name, sizeof(name))) {
            size_t k = strlen(label);
            (void)snprintf(label + k, sizeof(label) - k, " \"%s\"", name);
        }
        char where[128];
        place_of(&f[i], i, where, sizeof(where));
        size_t k = strlen(label);
        (void)snprintf(label + k, sizeof(label) - k, "  %s  ", where);
        cv_pen(c, COL_DIM, CV_COLOR_DEFAULT, 0);
        int32_t at = 1 + put(c, row, 1, label);
        if (i > 0) {
            static char slots[1024];
            static char ops[1024];
            static char line[2100];
            values(f[i].slots, f[i].nslots, slots, sizeof(slots));
            values(f[i].operands, f[i].noperands, ops, sizeof(ops));
            (void)snprintf(line, sizeof(line), "slots %s  operands %s", slots, ops);
            cv_pen(c, CV_COLOR_DEFAULT, CV_COLOR_DEFAULT, 0);
            (void)put_cut(c, row, at, line, w - 1 - at);
        } else {
            draw_operands(c, row, at, w, &f[i]);
        }
        row++;
    }
    if (n == 0 && D.msg[0] != '\0') {
        cv_pen(c, CV_COLOR_DEFAULT, CV_COLOR_DEFAULT, 0);
        (void)put_cut(c, row, 1, D.msg, w - 2);
    }
}

typedef struct {
    const char *key;
    const char *rest;
} hint;

/* the footer's keys, the ones a narrow screen drops last first: quit
   among them, as the edt keeps its ^Q */
static const hint hints[] = {
    {"s", " step"},        {"n", " next"},    {"c", " continue"},  {"q", " quit"},
    {"h", " help"},        {"x", " bytes"},   {"space", " break"}, {"b", " back"},
    {"i", " instruction"}, {"r", " restart"},
};

static int32_t put_hint(canvas *c, int32_t y, int32_t col, const char *key, const char *rest) {
    cv_pen(c, COL_KEYS, COL_BAR, CV_A_BOLD);
    col += put(c, y, col, key);
    cv_pen(c, COL_WORDS, COL_BAR, 0);
    return col + put(c, y, col, rest) + 2;
}

static void draw_footer(canvas *c, uint32_t line, uint32_t col, int32_t w, int32_t h) {
    int32_t y = h - 1;
    cv_pen(c, COL_WORDS, COL_BAR, 0);
    cv_fill(c, y, 0, 1, w, cv_cstr(" "));
    char where[96];
    (void)snprintf(where, sizeof(where), "%s  step %u", D.entry, D.steps);
    if (line > 0) {
        size_t k = strlen(where);
        (void)snprintf(where + k, sizeof(where) - k, "  %u:%u", line, col);
    }
    int32_t wl = (int32_t)strlen(where);
    int32_t room = w - wl - 2;
    if (D.msg[0] != '\0') {
        (void)put_cut(c, y, 1, D.msg, room - 1);
    } else {
        int32_t at = 1;
        for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
            const char *rest = hints[i].rest;
            if (strcmp(hints[i].key, "x") == 0 && D.bytes) {
                rest = " code"; /* what x goes to */
            }
            if (at + (int32_t)(strlen(hints[i].key) + strlen(rest)) > room) {
                break;
            }
            at = put_hint(c, y, at, hints[i].key, rest);
        }
    }
    cv_pen(c, COL_WHERE, COL_BAR, 0);
    (void)put(c, y, w - wl - 1, where);
}

/* The h page, the whole screen, scrolled from help_top. */
static void draw_help(canvas *c, int32_t w, int32_t h) {
    int32_t nrows = h - 1;
    D.help_page = nrows;
    D.help_top = max32(0, min32(D.help_top, (int32_t)HELP_LINES - nrows));
    for (int32_t row = 0; row < nrows && D.help_top + row < (int32_t)HELP_LINES; row++) {
        const char *line = help_lines[D.help_top + row];
        if (strncmp(line, "# ", 2) == 0) {
            cv_pen(c, COL_KEYS, CV_COLOR_DEFAULT, CV_A_BOLD);
            line += 2;
        } else {
            cv_pen(c, CV_COLOR_DEFAULT, CV_COLOR_DEFAULT, 0);
        }
        (void)put_cut(c, row, 1, line, w - 2);
    }
    int32_t y = h - 1;
    cv_pen(c, COL_WORDS, COL_BAR, 0);
    cv_fill(c, y, 0, 1, w, cv_cstr(" "));
    int32_t at = put_hint(c, y, 1, "arrows", " scroll");
    at = put_hint(c, y, at, "space", " page");
    (void)put_hint(c, y, at, "h", " back");
    int32_t last = min32(D.help_top + nrows, (int32_t)HELP_LINES);
    char where[48];
    (void)snprintf(where, sizeof(where), "help  %d-%d of %d", D.help_top + 1, last,
                   HELP_LINES);
    cv_pen(c, COL_WHERE, COL_BAR, 0);
    (void)put(c, y, w - (int32_t)strlen(where) - 1, where);
}

static void draw(void) {
    roc *m = D.m;
    canvas *c = &m->cmp.target;
    int32_t h = (int32_t)m->t.rows;
    int32_t w = (int32_t)m->t.cols;
    cv_reset(c, m->t.rows, m->t.cols);
    cv_pen_reset(c);
    cv_pen(c, CV_COLOR_DEFAULT, CV_COLOR_DEFAULT, 0);
    cv_fill(c, 0, 0, h, w, cv_cstr(" "));
    if (w < 40 || h < 10) {
        (void)put(c, 0, 0, "filo debug: the terminal is too small");
    } else if (D.help) {
        draw_help(c, w, h);
    } else {
        int32_t stack_rows = min32(max32(h / 4, 3), 10);
        int32_t top = h - 1 - stack_rows;
        int32_t left = w / 2;
        uint32_t n = 0;
        const filo_bc_frame *f = shown(&n);
        uint32_t fn = n > 0 ? f[0].fn : 0;
        uint32_t pc = n > 0 ? f[0].pc : 0;
        uint32_t line = 0;
        uint32_t col = 0;
        shown_at(&line, &col);
        draw_source(c, fn, n > 0, line, col, top, left);
        draw_marks(c, fn, n > 0, line, top);
        cv_pen(c, COL_RULE, CV_COLOR_DEFAULT, 0);
        cv_fill(c, 0, left, top, 1, cv_cstr("\xe2\x94\x82")); /* │ */
        if (n > 0 && D.bytes) {
            draw_bytes(c, fn, pc, line, top, left + 1, w);
        } else if (n > 0) {
            draw_code(c, fn, pc, line, top, left + 1, w);
        }
        draw_stack(c, f, n, top, stack_rows, w);
        draw_footer(c, line, col, w, h);
    }
    cv_flush(&m->t, &m->cmp.shown, c);
}

/* ---- keys ---- */

static void leave(void) {
    term_puts(&D.m->t, "\x1b[?25h");
    roc_app_leave(D.m, NULL);
}

/* The keys of the h page; false to quit. */
static bool help_key(uint32_t cp) {
    int32_t page = max32(D.help_page - 1, 1);
    switch (cp) {
    case 3:
        return false;
    case 'h':
    case 'q':
    case 'Q':
    case FT_KEY_ESC:
        D.help = false;
        break;
    case FT_KEY_UP:
        D.help_top--;
        break;
    case FT_KEY_DOWN:
        D.help_top++;
        break;
    case FT_KEY_PGUP:
    case 'b':
        D.help_top -= page;
        break;
    case FT_KEY_PGDN:
    case ' ':
        D.help_top += page;
        break;
    default:
        break;
    }
    D.help_top = max32(D.help_top, 0);
    return true;
}

/* A key; false to quit. */
static bool key(uint32_t cp) {
    if (ft_key_is_code(cp)) {
        cp = FT_KEY_BASE(cp);
    }
    if (D.help) {
        return help_key(cp);
    }
    D.msg[0] = '\0';
    if (D.done) {
        (void)snprintf(D.msg, sizeof(D.msg), "%s", D.ending);
    }
    switch (cp) {
    case 'h':
        D.help = true;
        D.help_top = 0;
        break;
    case 'x':
        if (D.bytes) {
            D.bytes = false;
        } else {
            D.bytes = true;
        }
        break;
    case 'q':
    case 'Q':
    case FT_KEY_ESC:
    case 3: /* Esc alone, or Ctrl-C */
        return false;
    case 's':
        by_line(false);
        break;
    case 'n':
        by_line(true);
        break;
    case 'i':
        instruction();
        break;
    case 'c':
        finish();
        break;
    case 'b':
        back();
        break;
    case 'r':
        start_over();
        break;
    case ' ':
        toggle_break();
        break;
    case FT_KEY_UP:
        move_cursor(-1);
        break;
    case FT_KEY_DOWN:
        move_cursor(1);
        break;
    case FT_KEY_PGUP:
        move_cursor(-10);
        break;
    case FT_KEY_PGDN:
        move_cursor(10);
        break;
    default:
        break;
    }
    return true;
}

static void debug_on_key(void *ctx, uint32_t cp) {
    (void)ctx;
    if (!key(cp)) {
        leave();
        return;
    }
    draw();
}

static void debug_on_resize(void *ctx) {
    (void)ctx;
    draw();
}

static const term_app debug_app = {
    .on_key = debug_on_key,
    .on_resize = debug_on_resize,
    .on_tick = NULL,
};

/* The entry named want, or "main", or the first; false when there is none
   by that name. */
static bool pick_entry(const char *want) {
    const char *name = want != NULL ? want : "main";
    for (uint32_t i = 0; i < listing.nexports; i++) {
        const fbc_span *s = &listing.export_names[i];
        if (strlen(name) == s->len && memcmp(listing.data + s->off, name, s->len) == 0) {
            (void)snprintf(D.entry, sizeof(D.entry), "%s", name);
            return true;
        }
    }
    if (want != NULL || listing.nexports == 0) {
        return false;
    }
    return entry_name(listing.export_fns[0], D.entry, sizeof(D.entry));
}

bool debug_enter(roc *m, const uint8_t *unit, size_t len, const char *entry, const char *dir,
                 const char *label) {
    if (len > DBG_UNIT_CAP) {
        roc_err(m, "filo", label, "File too large");
        return false;
    }
    if (!take_memory(m)) {
        roc_err(m, "filo", label, "Not enough memory here to debug");
        return false;
    }
    memset(&D, 0, sizeof(D));
    D.m = m;
    memcpy(unit_mem, unit, len);
    D.ulen = len;
    (void)snprintf(D.dir, sizeof(D.dir), "%s", dir);
    char why[160];
    if (!fbc_read(&listing, unit_mem, len, why, sizeof(why))) {
        roc_err(m, "filo", label, why);
        return false;
    }
    if (!pick_entry(entry)) {
        (void)snprintf(why, sizeof(why), "No entry point named %s", entry != NULL ? entry : "main");
        roc_err(m, "filo", label, why);
        return false;
    }
    D.app = filo_bc_declares(unit_mem, len, "draw");
    fbc_positions(&listing, keep_place, NULL);
    build_rows();
    read_sources();
    D.cursor = 1;
    if (!restart()) {
        roc_err(m, "filo", label, D.ending);
        return false;
    }
    roc_app_enter(m, &debug_app);
    term_puts(&m->t, "\x1b[?25l");
    m->cmp.shown.rows = 0; /* whatever was on the terminal is not ours */
    draw();
    return true;
}
