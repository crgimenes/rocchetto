#include "script_data.h"

#include <string.h>

#include "home.h"
#include "regex.h"
#include "roc.h"
#include "utf8.h"

static script_exec *run_of(filo_ctx *ctx) {
    return ctx->host.user;
}

static roc *roc_of(filo_ctx *ctx) {
    return run_of(ctx)->m;
}

static filo_value nil_value(void) {
    filo_value v = {0};
    v.kind = FILO_LIST;
    return v;
}

static int want_string(filo_ctx *ctx, const filo_value *v, const char *what) {
    return v->kind == FILO_STRING ? FILO_OK : filo_fail2(ctx, what, " expects a string");
}

/* A whole number in [lo, hi], for an index, a count or a byte. */
static int want_whole(filo_ctx *ctx, const filo_value *v, const char *what, double lo, double hi,
                      double *out) {
    if (v->kind != FILO_NUMBER || v->u.num != v->u.num || v->u.num < lo || v->u.num > hi ||
        v->u.num != (double)(long long)v->u.num) {
        return filo_fail2(ctx, what, " expects a whole number in range");
    }
    *out = v->u.num;
    return FILO_OK;
}

static int new_string(filo_ctx *ctx, const uint8_t *data, size_t n, const char *what,
                      filo_value *out) {
    if (n > UINT32_MAX) {
        return filo_fail2(ctx, what, ": string too long");
    }
    uint8_t *mem = filo_alloc(ctx, n > 0 ? n : 1);
    if (mem == NULL) {
        return filo_fail2(ctx, what, ": out of memory");
    }
    if (n > 0) {
        memcpy(mem, data, n);
    }
    *out = filo_string(mem, (uint32_t)n);
    return FILO_OK;
}

/* ---- bytes ---- */

/* x held inside 0..len. */
static double clamp(double x, double len) {
    if (x < 0) {
        return 0;
    }
    return x > len ? len : x;
}

/* (byte-len s): how many bytes s is. */
static int b_byte_len(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1 || want_string(ctx, &a[0], "byte-len") != FILO_OK) {
        return n != 1 ? filo_fail(ctx, "byte-len expects a string") : FILO_ERR;
    }
    *out = filo_num((double)a[0].u.str.len);
    return FILO_OK;
}

/* (byte-at s i): the byte at i, 0..255; nil past either end. */
static int b_byte_at(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    double i = 0;
    if (n != 2 || want_string(ctx, &a[0], "byte-at") != FILO_OK ||
        want_whole(ctx, &a[1], "byte-at", -9e15, 9e15, &i) != FILO_OK) {
        return n != 2 ? filo_fail(ctx, "byte-at expects a string and an index") : FILO_ERR;
    }
    if (i < 0 || i >= (double)a[0].u.str.len) {
        *out = nil_value();
        return FILO_OK;
    }
    *out = filo_num((double)a[0].u.str.ptr[(size_t)i]);
    return FILO_OK;
}

/* (byte-sub s from) and (byte-sub s from to): the bytes [from, to), each
   end held inside 0..length; "" when to is not past from. */
static int b_byte_sub(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    double from = 0;
    if ((n != 2 && n != 3) || want_string(ctx, &a[0], "byte-sub") != FILO_OK ||
        want_whole(ctx, &a[1], "byte-sub", -9e15, 9e15, &from) != FILO_OK) {
        return n != 2 && n != 3 ? filo_fail(ctx, "byte-sub expects a string, from and maybe to")
                                : FILO_ERR;
    }
    double len = (double)a[0].u.str.len;
    double to = len;
    if (n == 3 && want_whole(ctx, &a[2], "byte-sub", -9e15, 9e15, &to) != FILO_OK) {
        return FILO_ERR;
    }
    from = clamp(from, len);
    to = clamp(to, len);
    if (to <= from) {
        return new_string(ctx, NULL, 0, "byte-sub", out);
    }
    return new_string(ctx, a[0].u.str.ptr + (size_t)from, (size_t)(to - from), "byte-sub", out);
}

/* (byte-find s needle) and (byte-find s needle from): where needle starts,
   at or after from, in bytes; -1 when it does not. */
static int b_byte_find(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    double from = 0;
    if ((n != 2 && n != 3) || want_string(ctx, &a[0], "byte-find") != FILO_OK ||
        want_string(ctx, &a[1], "byte-find") != FILO_OK ||
        (n == 3 && want_whole(ctx, &a[2], "byte-find", 0, 9e15, &from) != FILO_OK)) {
        return n != 2 && n != 3 ? filo_fail(ctx, "byte-find expects two strings and maybe from")
                                : FILO_ERR;
    }
    size_t len = a[0].u.str.len;
    size_t k = a[1].u.str.len;
    *out = filo_num(-1);
    for (size_t i = (size_t)from; i <= len && k <= len - i; i++) {
        if (k == 0 || memcmp(a[0].u.str.ptr + i, a[1].u.str.ptr, k) == 0) {
            *out = filo_num((double)i);
            break;
        }
    }
    return FILO_OK;
}

/* (byte-cmp a b): -1, 0 or 1, byte by byte, a shorter prefix first. */
static int b_byte_cmp(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 2 || want_string(ctx, &a[0], "byte-cmp") != FILO_OK ||
        want_string(ctx, &a[1], "byte-cmp") != FILO_OK) {
        return n != 2 ? filo_fail(ctx, "byte-cmp expects two strings") : FILO_ERR;
    }
    size_t x = a[0].u.str.len;
    size_t y = a[1].u.str.len;
    int c = memcmp(a[0].u.str.ptr, a[1].u.str.ptr, x < y ? x : y);
    if (c == 0) {
        c = (x > y) - (x < y);
    }
    *out = filo_num((double)((c > 0) - (c < 0)));
    return FILO_OK;
}

/* (bytes 104 105) is "hi": numbers 0..255, or lists of them, as bytes. */
static int b_bytes(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    size_t total = 0;
    for (uint32_t i = 0; i < n; i++) {
        total += a[i].kind == FILO_LIST ? a[i].u.seq.len : 1;
    }
    uint8_t *mem = filo_alloc(ctx, total > 0 ? total : 1);
    if (mem == NULL) {
        return filo_fail(ctx, "bytes: out of memory");
    }
    size_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        const filo_value *one = &a[i];
        uint32_t count = 1;
        if (a[i].kind == FILO_LIST) {
            filo_seq seq; /* a (range 0 n) holds no items until asked */
            if (filo_arg_list(ctx, &a[i], &seq) != FILO_OK) {
                return FILO_ERR;
            }
            one = seq.items;
            count = seq.len;
        }
        for (uint32_t j = 0; j < count; j++) {
            double b = 0;
            if (want_whole(ctx, &one[j], "bytes", 0, 255, &b) != FILO_OK) {
                return FILO_ERR;
            }
            mem[k++] = (uint8_t)b;
        }
    }
    *out = filo_string(mem, (uint32_t)k);
    return FILO_OK;
}

/* (byte-list s): the bytes of s as numbers 0..255. */
static int b_byte_list(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1 || want_string(ctx, &a[0], "byte-list") != FILO_OK) {
        return n != 1 ? filo_fail(ctx, "byte-list expects a string") : FILO_ERR;
    }
    size_t len = a[0].u.str.len;
    filo_value *items = filo_alloc(ctx, (len > 0 ? len : 1) * sizeof(filo_value));
    if (items == NULL) {
        return filo_fail(ctx, "byte-list: out of memory");
    }
    for (size_t i = 0; i < len; i++) {
        items[i] = filo_num((double)a[0].u.str.ptr[i]);
    }
    return filo_list(ctx, items, (uint32_t)len, out);
}

/* ---- unsigned 32-bit words: what a checksum is computed in ---- */

/* A whole number from -2^31 to 2^32-1, taken modulo 2^32 (a negative one
   as its two's complement). */
static int want_u32(filo_ctx *ctx, const filo_value *v, const char *what, uint32_t *out) {
    double x = 0;
    if (want_whole(ctx, v, what, -2147483648.0, 4294967295.0, &x) != FILO_OK) {
        return FILO_ERR;
    }
    *out = x < 0 ? (uint32_t)(int32_t)x : (uint32_t)x;
    return FILO_OK;
}

typedef enum { W_AND, W_OR, W_XOR, W_ADD, W_MUL } word_op;

static int fold_words(filo_ctx *ctx, const filo_value *a, uint32_t n, word_op op, const char *what,
                      filo_value *out) {
    if (n == 0) {
        return filo_fail2(ctx, what, " expects one number or more");
    }
    uint32_t acc = 0;
    if (want_u32(ctx, &a[0], what, &acc) != FILO_OK) {
        return FILO_ERR;
    }
    for (uint32_t i = 1; i < n; i++) {
        uint32_t x = 0;
        if (want_u32(ctx, &a[i], what, &x) != FILO_OK) {
            return FILO_ERR;
        }
        switch (op) {
        case W_AND:
            acc &= x;
            break;
        case W_OR:
            acc |= x;
            break;
        case W_XOR:
            acc ^= x;
            break;
        case W_ADD:
            acc += x;
            break;
        case W_MUL:
            acc = (uint32_t)((uint64_t)acc * x);
            break;
        }
    }
    *out = filo_num((double)acc);
    return FILO_OK;
}

static int b_u32(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "u32 expects a number");
    }
    return fold_words(ctx, a, n, W_OR, "u32", out);
}

static int b_u32_and(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return fold_words(ctx, a, n, W_AND, "u32-and", out);
}

static int b_u32_or(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return fold_words(ctx, a, n, W_OR, "u32-or", out);
}

static int b_u32_xor(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return fold_words(ctx, a, n, W_XOR, "u32-xor", out);
}

/* (u32-add a b ...) and (u32-mul a b ...) wrap around at 2^32. */
static int b_u32_add(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return fold_words(ctx, a, n, W_ADD, "u32-add", out);
}

static int b_u32_mul(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return fold_words(ctx, a, n, W_MUL, "u32-mul", out);
}

static int b_u32_not(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    uint32_t x = 0;
    if (n != 1 || want_u32(ctx, &a[0], "u32-not", &x) != FILO_OK) {
        return n != 1 ? filo_fail(ctx, "u32-not expects a number") : FILO_ERR;
    }
    *out = filo_num((double)~x);
    return FILO_OK;
}

/* (u32-shl x k) and (u32-shr x k): shifted k bits, 0..32 (32 gives 0). */
static int shift(filo_ctx *ctx, const filo_value *a, uint32_t n, bool left, filo_value *out) {
    const char *what = left ? "u32-shl" : "u32-shr";
    uint32_t x = 0;
    double k = 0;
    if (n != 2 || want_u32(ctx, &a[0], what, &x) != FILO_OK ||
        want_whole(ctx, &a[1], what, 0, 32, &k) != FILO_OK) {
        return n != 2 ? filo_fail2(ctx, what, " expects a number and a count 0..32") : FILO_ERR;
    }
    uint32_t r = 0;
    if (k < 32) {
        r = left ? x << (uint32_t)k : x >> (uint32_t)k;
    }
    *out = filo_num((double)r);
    return FILO_OK;
}

static int b_u32_shl(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return shift(ctx, a, n, true, out);
}

static int b_u32_shr(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return shift(ctx, a, n, false, out);
}

/* ---- regular expressions ---- */

/* A handle names a slot and the run that compiled it: one kept from an
   earlier command, or made up, is refused rather than read. */
static sh_regex *regex_of(filo_ctx *ctx, const filo_value *v, const char *what) {
    script_exec *r = run_of(ctx);
    double h = 0;
    if (want_whole(ctx, v, what, 1, 9e15, &h) != FILO_OK) {
        return NULL;
    }
    unsigned long long k = (unsigned long long)h - 1ULL;
    size_t slot = (size_t)(k % SC_REGEX_MAX);
    if (k / SC_REGEX_MAX != r->run_id || slot >= r->nre || (r->re_used & (1U << slot)) == 0) {
        (void)filo_fail2(ctx, what, ": not a regex of this run (re-compile)");
        return NULL;
    }
    return &r->re[slot];
}

/* (re-compile pattern) a BRE, (re-compile pattern "E") an ERE, "i" folding
   ASCII case: a handle (a number) for re-match, or a string saying why the
   pattern cannot be read. At most SC_REGEX_MAX at once (re-free). */
static int b_re_compile(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    script_exec *s = run_of(ctx);
    if ((n != 1 && n != 2) || want_string(ctx, &a[0], "re-compile") != FILO_OK ||
        (n == 2 && want_string(ctx, &a[1], "re-compile") != FILO_OK)) {
        return n != 1 && n != 2 ? filo_fail(ctx, "re-compile expects a pattern and maybe flags")
                                : FILO_ERR;
    }
    bool extended = false;
    bool icase = false;
    for (uint32_t i = 0; n == 2 && i < a[1].u.str.len; i++) {
        uint8_t f = a[1].u.str.ptr[i];
        if (f == 'E') {
            extended = true;
        } else if (f == 'B') {
            extended = false;
        } else if (f == 'i') {
            icase = true;
        } else {
            return filo_fail(ctx, "re-compile: flags are E, B and i");
        }
    }
    char pattern[SH_LINE_MAX];
    if (a[0].u.str.len >= sizeof(pattern) || memchr(a[0].u.str.ptr, 0, a[0].u.str.len) != NULL) {
        *out = filo_cstring(a[0].u.str.len >= sizeof(pattern) ? "pattern too long"
                                                              : "a NUL in the pattern");
        return FILO_OK;
    }
    memcpy(pattern, a[0].u.str.ptr, a[0].u.str.len);
    pattern[a[0].u.str.len] = '\0';
    size_t slot = 0;
    while (slot < s->nre && (s->re_used & (1U << slot)) != 0) {
        slot++;
    }
    if (slot == s->nre) {
        return filo_fail(ctx, "re-compile: too many regexes at once (re-free one)");
    }
    char why[96] = {0};
    if (!sh_regex_compile(&s->re[slot], pattern, extended, icase, why, sizeof(why))) {
        return new_string(ctx, (const uint8_t *)why, strlen(why), "re-compile", out);
    }
    s->re_used |= 1U << slot;
    *out = filo_num((double)(((unsigned long long)s->run_id * SC_REGEX_MAX) + slot + 1ULL));
    return FILO_OK;
}

/* (re-match re s) and (re-match re s from): ^ is the start of s and $ its
   end, so a match looked for past 0 never takes ^; (re-match re s from #t)
   says s itself continues a line, and ^ matches nowhere. Nil for no match,
   else a
   list of (start end) byte offsets, the whole match first and then each
   group, nil for a group that took no part. A pattern that takes too many
   steps on s is an error, not a no-match. */
static int b_re_match(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n < 2 || n > 4) {
        return filo_fail(ctx, "re-match expects a regex, a string, maybe from and not-bol");
    }
    const sh_regex *re = regex_of(ctx, &a[0], "re-match");
    double from = 0;
    if (re == NULL || want_string(ctx, &a[1], "re-match") != FILO_OK ||
        (n >= 3 && want_whole(ctx, &a[2], "re-match", 0, 9e15, &from) != FILO_OK)) {
        return FILO_ERR;
    }
    bool not_bol = false;
    if (n == 4 && a[3].kind == FILO_BOOL) {
        not_bol = a[3].u.b;
    }
    size_t len = a[1].u.str.len;
    if (from > (double)len) {
        *out = nil_value();
        return FILO_OK;
    }
    long so[RE_GROUPS];
    long eo[RE_GROUPS];
    int r = sh_regex_exec(re, (const char *)a[1].u.str.ptr, len, (size_t)from, not_bol, so, eo);
    if (r < 0) {
        return filo_fail(ctx, "re-match: the pattern takes too many steps on this text");
    }
    if (r == 0) {
        *out = nil_value();
        return FILO_OK;
    }
    int groups = re->ngroups + 1;
    if (groups > RE_GROUPS) {
        groups = RE_GROUPS;
    }
    filo_value items[RE_GROUPS];
    for (int g = 0; g < groups; g++) {
        if (so[g] < 0) {
            items[g] = nil_value();
            continue;
        }
        filo_value pair[2] = {filo_num((double)so[g]), filo_num((double)eo[g])};
        if (filo_tuple(ctx, pair, 2, &items[g]) != FILO_OK) {
            return FILO_ERR;
        }
    }
    return filo_list(ctx, items, (uint32_t)groups, out);
}

/* (re-free re): its slot for another; false for one not in use. */
static int b_re_free(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    script_exec *s = run_of(ctx);
    double h = 0;
    if (n != 1 || want_whole(ctx, &a[0], "re-free", 1, 9e15, &h) != FILO_OK) {
        return n != 1 ? filo_fail(ctx, "re-free expects a regex") : FILO_ERR;
    }
    unsigned long long k = (unsigned long long)h - 1ULL;
    size_t slot = (size_t)(k % SC_REGEX_MAX);
    bool ours = false;
    if ((k / SC_REGEX_MAX) == s->run_id && slot < s->nre) {
        ours = (s->re_used & (1U << slot)) != 0;
    }
    if (ours) {
        s->re_used &= ~(1U << slot);
    }
    *out = filo_bool(ours);
    return FILO_OK;
}

/* ---- UTF-8 ---- */

/* Each rune of s[0..len): the code point, U+FFFD for each byte that does
   not make one, and how many bytes it took. */
typedef void (*rune_fn)(void *user, uint32_t cp, bool valid);

static void runes(const uint8_t *s, size_t len, rune_fn fn, void *user) {
    utf8_dec d;
    utf8_dec_init(&d);
    size_t i = 0;
    while (i < len) {
        uint32_t cp = 0;
        int resync = 0;
        utf8_result r = utf8_dec_feed(&d, s[i], &cp, &resync);
        if (r == UTF8_RUNE) {
            fn(user, cp, true);
        } else if (r == UTF8_ERROR) {
            fn(user, UTF8_REPLACEMENT, false);
            if (resync) {
                continue; /* the same byte starts a new sequence */
            }
        }
        i++;
    }
    if (d.need > 0) { /* cut short at the end */
        fn(user, UTF8_REPLACEMENT, false);
    }
}

static void count_bad(void *user, uint32_t cp, bool valid) {
    (void)cp;
    if (!valid) {
        (*(size_t *)user)++;
    }
}

/* (utf8-valid s): whether every byte of s is part of a well-formed rune. */
static int b_utf8_valid(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1 || want_string(ctx, &a[0], "utf8-valid") != FILO_OK) {
        return n != 1 ? filo_fail(ctx, "utf8-valid expects a string") : FILO_ERR;
    }
    size_t bad = 0;
    runes(a[0].u.str.ptr, a[0].u.str.len, count_bad, &bad);
    *out = filo_bool(bad == 0);
    return FILO_OK;
}

typedef struct {
    filo_value *items;
    size_t n;
    size_t cap;
    size_t width;
} rune_acc;

static void collect(void *user, uint32_t cp, bool valid) {
    (void)valid;
    rune_acc *acc = user;
    if (acc->n < acc->cap) {
        acc->items[acc->n] = filo_num((double)cp);
    }
    acc->n++;
}

/* (utf8-runes s): its code points, U+FFFD for each byte that is no rune. */
static int b_utf8_runes(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1 || want_string(ctx, &a[0], "utf8-runes") != FILO_OK) {
        return n != 1 ? filo_fail(ctx, "utf8-runes expects a string") : FILO_ERR;
    }
    size_t cap = a[0].u.str.len + 1; /* a rune takes at least a byte; a cut tail one more */
    rune_acc acc = {filo_alloc(ctx, cap * sizeof(filo_value)), 0, cap, 0};
    if (acc.items == NULL) {
        return filo_fail(ctx, "utf8-runes: out of memory");
    }
    runes(a[0].u.str.ptr, a[0].u.str.len, collect, &acc);
    return filo_list(ctx, acc.items, (uint32_t)acc.n, out);
}

/* (utf8-encode 99 97 102 233) is "café": each code point, U+0000 to
   U+10FFFF and no surrogate, as its UTF-8 bytes; lists of them too. */
static int b_utf8_encode(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    size_t total = 0;
    for (uint32_t i = 0; i < n; i++) {
        total += (size_t)(a[i].kind == FILO_LIST ? a[i].u.seq.len : 1) * UTF8_MAX_BYTES;
    }
    uint8_t *mem = filo_alloc(ctx, total > 0 ? total : 1);
    if (mem == NULL) {
        return filo_fail(ctx, "utf8-encode: out of memory");
    }
    size_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        const filo_value *one = &a[i];
        uint32_t count = 1;
        if (a[i].kind == FILO_LIST) {
            filo_seq seq; /* a (range 0 n) holds no items until asked */
            if (filo_arg_list(ctx, &a[i], &seq) != FILO_OK) {
                return FILO_ERR;
            }
            one = seq.items;
            count = seq.len;
        }
        for (uint32_t j = 0; j < count; j++) {
            double cp = 0;
            if (want_whole(ctx, &one[j], "utf8-encode", 0, 0x10FFFF, &cp) != FILO_OK) {
                return FILO_ERR;
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                return filo_fail(ctx, "utf8-encode: a surrogate is no code point");
            }
            uint8_t buf[UTF8_MAX_BYTES];
            size_t m = utf8_encode(buf, (uint32_t)cp);
            memcpy(mem + k, buf, m);
            k += m;
        }
    }
    *out = filo_string(mem, (uint32_t)k);
    return FILO_OK;
}

/* Columns on a terminal: controls (tab among them: expand takes its own
   count) take none, combining marks none, wide runes two, and a byte that
   is no rune one, as the shell draws it. */
static size_t rune_width(uint32_t cp, bool valid) {
    if (!valid) {
        return 1;
    }
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) {
        return 0;
    }
    int w = utf8_width(cp);
    return w < 0 ? 0 : (size_t)w;
}

static void add_width(void *user, uint32_t cp, bool valid) {
    ((rune_acc *)user)->width += rune_width(cp, valid);
}

/* (str-width s): the columns s takes on a terminal. */
static int b_str_width(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1 || want_string(ctx, &a[0], "str-width") != FILO_OK) {
        return n != 1 ? filo_fail(ctx, "str-width expects a string") : FILO_ERR;
    }
    rune_acc acc = {NULL, 0, 0, 0};
    runes(a[0].u.str.ptr, a[0].u.str.len, add_width, &acc);
    *out = filo_num((double)acc.width);
    return FILO_OK;
}

/* (rune-width cp): the columns one code point takes, 0, 1 or 2. */
static int b_rune_width(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    double cp = 0;
    if (n != 1 || want_whole(ctx, &a[0], "rune-width", 0, 0x10FFFF, &cp) != FILO_OK) {
        return n != 1 ? filo_fail(ctx, "rune-width expects a code point") : FILO_ERR;
    }
    *out = filo_num((double)rune_width((uint32_t)cp, true));
    return FILO_OK;
}

/* ---- options, as a utility reads them ---- */

/* (getopt args "bns:w:") reads the options as POSIX's getopts does: the
   letters of spec, a ':' after one that takes a value (the rest of its word,
   or the next word); "--" ends them, as does the first word that is not an
   option ("-" alone is an operand). The result is (opts operands bad):
   opts a list of (letter value), value "" for a flag, in order; operands
   what follows; bad "" or the letter that is not in spec, or that lacks its
   value. */
static int b_getopt(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 2 || a[0].kind != FILO_LIST || want_string(ctx, &a[1], "getopt") != FILO_OK) {
        return n != 2 || a[0].kind != FILO_LIST
                   ? filo_fail(ctx, "getopt expects the words and a spec")
                   : FILO_ERR;
    }
    filo_seq words;
    if (filo_arg_list(ctx, &a[0], &words) != FILO_OK) {
        return FILO_ERR;
    }
    const filo_value *w = words.items;
    uint32_t nw = words.len;
    const uint8_t *spec = a[1].u.str.ptr;
    uint32_t sl = a[1].u.str.len;
    filo_value *opts = filo_alloc(ctx, (((size_t)nw * 8) + 1) * sizeof(filo_value));
    if (opts == NULL) {
        return filo_fail(ctx, "getopt: out of memory");
    }
    uint32_t no = 0;
    uint32_t i = 0;
    char bad[2] = {0, 0};
    while (i < nw && bad[0] == 0) {
        if (w[i].kind != FILO_STRING) {
            return filo_fail(ctx, "getopt: each word is a string");
        }
        const uint8_t *word = w[i].u.str.ptr;
        uint32_t len = w[i].u.str.len;
        if (len == 2 && word[0] == '-' && word[1] == '-') {
            i++;
            break;
        }
        if (len < 2 || word[0] != '-') {
            break;
        }
        i++;
        for (uint32_t k = 1; k < len; k++) {
            const uint8_t *at = memchr(spec, word[k], sl);
            if (at == NULL || word[k] == ':') {
                bad[0] = (char)word[k];
                break;
            }
            size_t pos = (size_t)(at - spec);
            bool takes = false;
            if (pos + 1 < sl && at[1] == ':') {
                takes = true;
            }
            filo_value pair[2] = {{0}, filo_cstring("")};
            if (new_string(ctx, word + k, 1, "getopt", &pair[0]) != FILO_OK) {
                return FILO_ERR;
            }
            if (takes && k + 1 < len) {
                if (new_string(ctx, word + k + 1, len - k - 1, "getopt", &pair[1]) != FILO_OK) {
                    return FILO_ERR;
                }
                k = len;
            } else if (takes && i < nw && w[i].kind == FILO_STRING) {
                pair[1] = w[i];
                i++;
            } else if (takes) {
                bad[0] = (char)word[k];
                break;
            }
            if (filo_tuple(ctx, pair, 2, &opts[no]) != FILO_OK) {
                return FILO_ERR;
            }
            no++;
        }
    }
    filo_value parts[3] = {{0}, {0}, {0}};
    if (filo_list(ctx, opts, no, &parts[0]) != FILO_OK ||
        filo_list(ctx, w + (i < nw ? i : nw), nw - (i < nw ? i : nw), &parts[1]) != FILO_OK ||
        new_string(ctx, (const uint8_t *)bad, bad[0] != 0 ? 1 : 0, "getopt", &parts[2]) !=
            FILO_OK) {
        return FILO_ERR;
    }
    return filo_tuple(ctx, parts, 3, out);
}

/* ---- numbers as text ---- */

/* A whole number of at most 2^53 in base 2..36, a minus sign when below
   zero; into buf (66 bytes), its length returned. */
size_t script_whole_text(double x, unsigned base, char *buf) {
    char digits[66];
    size_t k = 0;
    bool neg = x < 0;
    unsigned long long v = (unsigned long long)(neg ? -x : x);
    do {
        unsigned d = (unsigned)(v % base);
        digits[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    } while (v > 0);
    size_t n = 0;
    if (neg) {
        buf[n++] = '-';
    }
    while (k > 0) {
        buf[n++] = digits[--k];
    }
    return n;
}

/* (int-text n) is n's digits, (int-text n 8) in octal, 16 in hex (small
   letters): exact for a whole number up to 2^53, where (string n) turns
   to an exponent past 2^31. */
static int b_int_text(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    double x = 0;
    double base = 10;
    if ((n != 1 && n != 2) ||
        want_whole(ctx, &a[0], "int-text", -9007199254740992.0, 9007199254740992.0, &x) !=
            FILO_OK ||
        (n == 2 && want_whole(ctx, &a[1], "int-text", 2, 36, &base) != FILO_OK)) {
        return n != 1 && n != 2 ? filo_fail(ctx, "int-text expects a number and maybe a base")
                                : FILO_ERR;
    }
    char buf[66] = {0};
    size_t k = script_whole_text(x, (unsigned)base, buf);
    return new_string(ctx, (const uint8_t *)buf, k, "int-text", out);
}

/* ---- files: what a path is, what a directory holds ---- */

static const char *kind_name(const roc_stat *st) {
    if (st->link) {
        return "link";
    }
    return st->dir ? "dir" : "file";
}

/* A path argument, resolved against the shell's directory and ~. */
static int want_path(filo_ctx *ctx, const filo_value *v, const char *what, char *out, size_t cap) {
    char arg[VFS_PATH_MAX];
    if (want_string(ctx, v, what) != FILO_OK) {
        return FILO_ERR;
    }
    if (v->u.str.len >= sizeof(arg) || memchr(v->u.str.ptr, 0, v->u.str.len) != NULL) {
        return filo_fail2(ctx, what, ": not a path (too long, or a NUL in it)");
    }
    memcpy(arg, v->u.str.ptr, v->u.str.len);
    arg[v->u.str.len] = '\0';
    if (!roc_resolve_arg(roc_of(ctx), arg, out, cap)) {
        return filo_fail2(ctx, what, ": File name too long");
    }
    return FILO_OK;
}

/* (path-resolve p): p as an absolute path, ~, . and .. read as the shell
   reads them. */
static int b_path_resolve(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    char path[VFS_PATH_MAX] = {0};
    if (n != 1 || want_path(ctx, &a[0], "path-resolve", path, sizeof(path)) != FILO_OK) {
        return n != 1 ? filo_fail(ctx, "path-resolve expects a path") : FILO_ERR;
    }
    return new_string(ctx, (const uint8_t *)path, strlen(path), "path-resolve", out);
}

/* (file-stat p) is nil when nothing is at p, else (kind size mtime from):
   kind "file", "dir" or "link" (a link as itself; (file-stat p #t) is what
   it names), size in bytes, mtime in seconds since 1970 UTC, either nil
   where the store does not know it, and from "home", "tree" or "site". */
static int b_file_stat(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    char path[VFS_PATH_MAX];
    if ((n != 1 && n != 2) || want_path(ctx, &a[0], "file-stat", path, sizeof(path)) != FILO_OK) {
        return n != 1 && n != 2 ? filo_fail(ctx, "file-stat expects a path and maybe #t")
                                : FILO_ERR;
    }
    bool follow = false;
    if (n == 2 && a[1].kind == FILO_BOOL) {
        follow = a[1].u.b;
    }
    roc_stat st;
    roc_origin from = ROC_FROM_HOME;
    if (!roc_stat_path(roc_of(ctx), path, follow, &st, &from)) {
        *out = nil_value();
        return FILO_OK;
    }
    static const char *const origins[] = {"home", "tree", "site"};
    filo_value parts[4] = {
        filo_cstring(kind_name(&st)),
        st.has_size ? filo_num((double)st.size) : nil_value(),
        st.has_mtime ? filo_num((double)st.mtime) : nil_value(),
        filo_cstring(origins[from]),
    };
    return filo_tuple(ctx, parts, 4, out);
}

/* (file-runs p): "program" or "script" when the shell runs the file at p by
   its first bytes, nil when it does not (or nothing is there). */
static int b_file_runs(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    char path[VFS_PATH_MAX];
    if (n != 1 || want_path(ctx, &a[0], "file-runs", path, sizeof(path)) != FILO_OK) {
        return n != 1 ? filo_fail(ctx, "file-runs expects a path") : FILO_ERR;
    }
    int kind = roc_exec_kind(roc_of(ctx), path, false);
    if (kind == 0) {
        *out = nil_value();
        return FILO_OK;
    }
    *out = filo_cstring(kind == 'p' ? "program" : "script");
    return FILO_OK;
}

/* (file-id p) and (file-id p #t): the file at p as one string that two
   names of the same file share ("dev:ino"); nil when nothing is there or
   the store does not tell files apart. */
static int b_file_id(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    char path[VFS_PATH_MAX];
    if ((n != 1 && n != 2) || want_path(ctx, &a[0], "file-id", path, sizeof(path)) != FILO_OK) {
        return n != 1 && n != 2 ? filo_fail(ctx, "file-id expects a path and maybe #t") : FILO_ERR;
    }
    bool follow = false;
    if (n == 2 && a[1].kind == FILO_BOOL) {
        follow = a[1].u.b;
    }
    roc_stat st;
    roc_origin from = ROC_FROM_HOME;
    if (!roc_stat_path(roc_of(ctx), path, follow, &st, &from) || !st.has_id) {
        *out = nil_value();
        return FILO_OK;
    }
    char id[48];
    size_t k = 0;
    const uint64_t parts[2] = {st.dev, st.ino};
    for (size_t p = 0; p < 2; p++) {
        char digits[20];
        size_t nd = 0;
        uint64_t v = parts[p];
        do {
            digits[nd++] = (char)('0' + (v % 10));
            v /= 10;
        } while (v != 0);
        if (p == 1) {
            id[k++] = ':';
        }
        while (nd > 0) {
            id[k++] = digits[--nd];
        }
    }
    return new_string(ctx, (const uint8_t *)id, k, "file-id", out);
}

/* (file-touch p secs): the file at p last written at secs; #t, or why
   not. A store with no times for directories says so. */
static int b_file_touch(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    char path[VFS_PATH_MAX];
    double secs = 0;
    if (n != 2 || want_path(ctx, &a[0], "file-touch", path, sizeof(path)) != FILO_OK ||
        want_whole(ctx, &a[1], "file-touch", -9e15, 9e15, &secs) != FILO_OK) {
        return n != 2 ? filo_fail(ctx, "file-touch expects a path and seconds") : FILO_ERR;
    }
    roc *m = roc_of(ctx);
    const char *why = NULL;
    int r = ROC_HOST_NOT_MINE;
    if (m->host.set_mtime != NULL) {
        r = m->host.set_mtime(m->host.ctx, path, (int64_t)secs);
    }
    roc_stat st;
    roc_origin from = ROC_FROM_HOME;
    if (r == ROC_HOST_NO) {
        why = roc_stat_path(m, path, true, &st, &from) ? "Operation not permitted"
                                                       : "No such file or directory";
    } else if (r == ROC_HOST_NOT_MINE) {
        if (ufs_set_mtime(&m->uf, path, (int64_t)secs)) {
            roc_home_changed(m);
        } else if (roc_stat_path(m, path, true, &st, &from)) {
            why = st.dir ? "Operation not supported (no times for directories here)"
                         : "Read-only file system";
        } else {
            why = "No such file or directory";
        }
    }
    *out = why == NULL ? filo_bool(true) : filo_cstring(why);
    return FILO_OK;
}

/* (time-zone): the host's offset from UTC in minutes east, nil without a
   clock. */
static int b_time_zone(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    const roc *m = roc_of(ctx);
    if (n != 0) {
        return filo_fail(ctx, "time-zone takes nothing");
    }
    int64_t secs = 0;
    int32_t tz = 0;
    if (m->host.clock == NULL || !m->host.clock(m->host.ctx, &secs, &tz)) {
        *out = nil_value();
        return FILO_OK;
    }
    *out = filo_num((double)tz);
    return FILO_OK;
}

/* (store-info): what the home can hold and keep, as (name value) pairs:
   store "disk" (the host's storage) or "memory"; the memory store's bytes
   and files, used and at most, and per file; kept (the blob kept between
   visits, -1 when this host keeps nothing), its cap and per file; times
   1 when files get the time they were written; conflict 1 when another
   tab kept its home after this one read it (home keep or home reload). */
static int b_store_info(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    roc *m = roc_of(ctx);
    if (n != 0) {
        return filo_fail(ctx, "store-info takes nothing");
    }
    static const char *const names[] = {
        "store", "memory-used", "memory-cap",    "files", "files-max", "file-max",
        "kept",  "kept-cap",    "kept-file-max", "times", "conflict",
    };
    filo_value vals[11] = {
        filo_cstring(m->host.file_stat != NULL ? "disk" : "memory"),
        filo_num((double)m->uf.used),
        filo_num((double)UFS_DATA_CAP),
        filo_num((double)m->uf.nfiles),
        filo_num((double)UFS_FILES_MAX),
        filo_num((double)UFS_FILE_MAX),
        filo_num((double)roc_home_kept(m)),
        filo_num((double)HOME_BLOB_CAP),
        filo_num((double)HOME_FILE_MAX),
        filo_num(m->host.clock != NULL ? 1.0 : 0.0),
        filo_num(roc_home_conflict(m) ? 1.0 : 0.0),
    };
    filo_value items[11];
    for (size_t i = 0; i < 11; i++) {
        filo_value pair[2] = {filo_cstring(names[i]), vals[i]};
        if (filo_tuple(ctx, pair, 2, &items[i]) != FILO_OK) {
            return FILO_ERR;
        }
    }
    return filo_list(ctx, items, 11, out);
}

typedef struct {
    filo_ctx *ctx;
    const char **names;
    uint8_t *kinds; /* 0 file, 1 dir, 2 link */
    size_t n;
    size_t cap;
    bool short_of_memory;
} listing;

static void listing_add(void *user, const char *name, bool dir, bool link) {
    listing *l = user;
    if (l->short_of_memory || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return;
    }
    if (l->n == l->cap) { /* the arena only grows: a larger copy, the old one left */
        size_t cap = l->cap > 0 ? l->cap * 2 : 64;
        const char **names = (const char **)filo_alloc(l->ctx, cap * sizeof(*names));
        uint8_t *kinds = filo_alloc(l->ctx, cap);
        if (names == NULL || kinds == NULL) {
            l->short_of_memory = true;
            return;
        }
        if (l->n > 0) {
            memcpy((void *)names, (const void *)l->names, l->n * sizeof(*names));
            memcpy(kinds, l->kinds, l->n);
        }
        l->names = names;
        l->kinds = kinds;
        l->cap = cap;
    }
    size_t len = strlen(name);
    char *copy = filo_alloc(l->ctx, len + 1);
    if (copy == NULL) {
        l->short_of_memory = true;
        return;
    }
    memcpy(copy, name, len + 1);
    l->names[l->n] = copy;
    uint8_t kind = dir ? 1 : 0;
    if (link) {
        kind = 2;
    }
    l->kinds[l->n] = kind;
    l->n++;
}

/* Byte order (LC_ALL=C), a merge sort: a directory may be large. */
static void sort_listing(listing *l, const char **tmp_names, uint8_t *tmp_kinds) {
    for (size_t width = 1; width < l->n; width *= 2) {
        for (size_t lo = 0; lo < l->n; lo += 2 * width) {
            size_t mid = lo + width < l->n ? lo + width : l->n;
            size_t hi = lo + (2 * width) < l->n ? lo + (2 * width) : l->n;
            size_t i = lo;
            size_t j = mid;
            size_t k = lo;
            while (i < mid || j < hi) {
                bool left = true; /* the right run is done */
                if (j < hi && i >= mid) {
                    left = false;
                } else if (j < hi) {
                    left = strcmp(l->names[i], l->names[j]) <= 0;
                }
                size_t from = left ? i++ : j++;
                tmp_names[k] = l->names[from];
                tmp_kinds[k] = l->kinds[from];
                k++;
            }
        }
        memcpy((void *)l->names, (const void *)tmp_names, l->n * sizeof(*tmp_names));
        memcpy(l->kinds, tmp_kinds, l->n);
    }
}

/* (dir-read d), (dir-read d from) and (dir-read d from count): the
   entries of d, dot files too, in byte order, as (entries next): entries a
   list of (name kind), at most count (256, up to 1024) from from, and next
   where the following page starts, nil after the last. A directory that
   cannot be read is an error; nothing is ever cut without saying so. */
static int b_dir_read(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    char dir[VFS_PATH_MAX];
    double from = 0;
    double count = 256;
    if (n < 1 || n > 3 || want_path(ctx, &a[0], "dir-read", dir, sizeof(dir)) != FILO_OK ||
        (n >= 2 && want_whole(ctx, &a[1], "dir-read", 0, 9e15, &from) != FILO_OK) ||
        (n == 3 && want_whole(ctx, &a[2], "dir-read", 1, 1024, &count) != FILO_OK)) {
        return n < 1 || n > 3 ? filo_fail(ctx, "dir-read expects a directory, maybe from and count")
                              : FILO_ERR;
    }
    const roc *m = roc_of(ctx);
    roc_stat st;
    roc_origin origin = ROC_FROM_HOME;
    if (!roc_stat_path(m, dir, true, &st, &origin)) {
        return filo_fail2(ctx, "dir-read: No such file or directory: ", dir);
    }
    if (!st.dir) {
        return filo_fail2(ctx, "dir-read: Not a directory: ", dir);
    }
    listing l = {ctx, NULL, NULL, 0, 0, false};
    if (!roc_dir_list(m, dir, listing_add, &l)) {
        return filo_fail2(ctx, "dir-read: I/O error reading ", dir);
    }
    if (l.short_of_memory) {
        return filo_fail2(ctx, "dir-read: out of memory listing ", dir);
    }
    if (l.n > 1) {
        const char **tn = (const char **)filo_alloc(ctx, l.n * sizeof(*tn));
        uint8_t *tk = filo_alloc(ctx, l.n);
        if (tn == NULL || tk == NULL) {
            return filo_fail2(ctx, "dir-read: out of memory listing ", dir);
        }
        sort_listing(&l, tn, tk);
    }
    size_t first = from < (double)l.n ? (size_t)from : l.n;
    size_t take = l.n - first < (size_t)count ? l.n - first : (size_t)count;
    filo_value *items = filo_alloc(ctx, (take > 0 ? take : 1) * sizeof(filo_value));
    if (items == NULL) {
        return filo_fail(ctx, "dir-read: out of memory");
    }
    static const char *const kinds[] = {"file", "dir", "link"};
    for (size_t i = 0; i < take; i++) {
        filo_value pair[2] = {
            filo_cstring(l.names[first + i]),
            filo_cstring(kinds[l.kinds[first + i]]),
        };
        if (filo_tuple(ctx, pair, 2, &items[i]) != FILO_OK) {
            return FILO_ERR;
        }
    }
    filo_value parts[2] = {{0}, nil_value()};
    if (filo_list(ctx, items, (uint32_t)take, &parts[0]) != FILO_OK) {
        return FILO_ERR;
    }
    if (first + take < l.n) {
        parts[1] = filo_num((double)(first + take));
    }
    return filo_tuple(ctx, parts, 2, out);
}

/* ---- the environment a utility sees ---- */

/* HOME, PWD and USER are the shell's own, always there and exported. */
static const char *virtual_var(const roc *m, const char *name, char *buf, size_t cap) {
    if (strcmp(name, "HOME") == 0) {
        return roc_home_path(m, "", buf, cap) ? buf : NULL;
    }
    if (strcmp(name, "PWD") == 0) {
        return m->cwd;
    }
    if (strcmp(name, "USER") == 0) {
        return m->user;
    }
    return NULL;
}

/* (env-get name): the value of an exported variable (export, or NAME=v
   before the command), nil for one that is not exported or not set. */
static int b_env_get(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    const roc *m = roc_of(ctx);
    char name[SH_NAME_MAX];
    if (n != 1 || want_string(ctx, &a[0], "env-get") != FILO_OK) {
        return n != 1 ? filo_fail(ctx, "env-get expects a name") : FILO_ERR;
    }
    if (a[0].u.str.len >= sizeof(name)) {
        *out = nil_value();
        return FILO_OK;
    }
    memcpy(name, a[0].u.str.ptr, a[0].u.str.len);
    name[a[0].u.str.len] = '\0';
    char buf[VFS_PATH_MAX];
    const char *v = virtual_var(m, name, buf, sizeof(buf));
    if (v == NULL && sh_var_exported(&m->vars, name)) {
        v = sh_var_get(&m->vars, name, strlen(name));
    }
    if (v == NULL) {
        *out = nil_value();
        return FILO_OK;
    }
    return new_string(ctx, (const uint8_t *)v, strlen(v), "env-get", out);
}

/* (env-list): the exported variables, HOME, PWD and USER among them, as
   (name value) tuples in byte order of their names. */
static int b_env_list(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    const roc *m = roc_of(ctx);
    if (n != 0) {
        return filo_fail(ctx, "env-list takes nothing");
    }
    if (m == NULL) {
        return filo_fail(ctx, "env-list: no shell");
    }
    const char *names[SH_VARS_MAX + 3];
    const char *values[SH_VARS_MAX + 3];
    size_t k = 0;
    static char home[VFS_PATH_MAX];
    static const char *const virt[] = {"HOME", "PWD", "USER"};
    for (size_t i = 0; i < 3; i++) {
        const char *v = virtual_var(m, virt[i], home, sizeof(home));
        if (v != NULL) {
            names[k] = virt[i];
            values[k] = v;
            k++;
        }
    }
    for (int i = 0; i < m->vars.n; i++) {
        if (m->vars.v[i].exported) {
            names[k] = m->vars.v[i].name;
            values[k] = m->vars.v[i].value;
            k++;
        }
    }
    for (size_t i = 1; i < k; i++) { /* a handful: insertion sort */
        for (size_t j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            const char *tn = names[j];
            const char *tv = values[j];
            names[j] = names[j - 1];
            values[j] = values[j - 1];
            names[j - 1] = tn;
            values[j - 1] = tv;
        }
    }
    filo_value items[SH_VARS_MAX + 3];
    for (size_t i = 0; i < k; i++) {
        filo_value pair[2] = {{0}, {0}};
        if (new_string(ctx, (const uint8_t *)names[i], strlen(names[i]), "env-list", &pair[0]) !=
                FILO_OK ||
            new_string(ctx, (const uint8_t *)values[i], strlen(values[i]), "env-list", &pair[1]) !=
                FILO_OK ||
            filo_tuple(ctx, pair, 2, &items[i]) != FILO_OK) {
            return FILO_ERR;
        }
    }
    return filo_list(ctx, items, (uint32_t)k, out);
}

/* (now): seconds since 1970, UTC, from the host's clock; nil where it has
   none. */
static int b_now(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    const roc *m = roc_of(ctx);
    if (n != 0) {
        return filo_fail(ctx, "now takes nothing");
    }
    int64_t secs = 0;
    int32_t tz = 0;
    if (m->host.clock == NULL || !m->host.clock(m->host.ctx, &secs, &tz)) {
        *out = nil_value();
        return FILO_OK;
    }
    *out = filo_num((double)secs);
    return FILO_OK;
}

/* (out-terminal): (columns rows) when what out-write writes goes straight
   to the terminal, nil when a |, a > or a $(...) takes it: ls lays out
   columns for a person, one name a line for a program. */
static int b_out_terminal(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    const roc *m = roc_of(ctx);
    if (n != 0) {
        return filo_fail(ctx, "out-terminal takes nothing");
    }
    if (!roc_out_terminal(m)) {
        *out = nil_value();
        return FILO_OK;
    }
    filo_value size[2] = {filo_num((double)m->t.cols), filo_num((double)m->t.rows)};
    return filo_tuple(ctx, size, 2, out);
}

/* (glob-match pattern s): whether s matches the shell's pattern (* ? [...]
   and \ as sh reads them in a word), the whole of it. */
static int b_glob_match(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    char pat[SH_LINE_MAX];
    char str[SH_LINE_MAX];
    if (n != 2 || want_string(ctx, &a[0], "glob-match") != FILO_OK ||
        want_string(ctx, &a[1], "glob-match") != FILO_OK) {
        return n != 2 ? filo_fail(ctx, "glob-match expects a pattern and a string") : FILO_ERR;
    }
    if (a[0].u.str.len >= sizeof(pat) || a[1].u.str.len >= sizeof(str) ||
        memchr(a[0].u.str.ptr, 0, a[0].u.str.len) != NULL ||
        memchr(a[1].u.str.ptr, 0, a[1].u.str.len) != NULL) {
        return filo_fail(ctx, "glob-match: too long, or a NUL in it");
    }
    memcpy(pat, a[0].u.str.ptr, a[0].u.str.len);
    pat[a[0].u.str.len] = '\0';
    memcpy(str, a[1].u.str.ptr, a[1].u.str.len);
    str[a[1].u.str.len] = '\0';
    *out = filo_bool(sh_match(pat, str));
    return FILO_OK;
}

/* ---- the loop a utility runs in ---- */

enum { SC_ITERATE_SLACK = 64U << 10U };

/* (iterate f state): (f state) again and again, each result the next
   state, until f gives nil; the last state is the value. The language has
   no loop and no tail calls, and a recursion deep enough to walk an input
   is not there: this is the one way over a thousand lines. Only the state
   f returns outlives each call (filo_keep), so a long walk holds one state
   at a time and does not fill the memory, whatever the state holds. */
static int b_iterate(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 2 || a[0].kind != FILO_FUNC) {
        return filo_fail(ctx, "iterate expects a function and a first state");
    }
    filo_value state = a[1];
    size_t mark = ctx->run.used; /* the states live here */
    size_t live = 0;             /* what the last state took when they were last packed */
    for (;;) {
        size_t top = ctx->run.used;
        filo_value next = {0};
        if (filo_call(ctx, &a[0], &state, 1, &next) != FILO_OK) {
            return FILO_ERR;
        }
        if (next.kind == FILO_LIST && next.u.seq.len == 0) {
            *out = state;
            return FILO_OK;
        }
        /* what the call made goes, all but the new state; the states
           before are packed away once they take half again the room of the
           last one packed, or the arena runs short, so a state that grows
           is not copied whole each time */
        filo_keep(ctx, top, &next);
        if (ctx->run.used - mark > live + (live / 2) + SC_ITERATE_SLACK ||
            ctx->run.cap - ctx->run.used < ctx->run.cap / 4) {
            filo_keep(ctx, mark, &next);
            live = ctx->run.used - mark;
        }
        state = next;
    }
}

/* (list-sort items cmp), (list-sort items cmp key) and (list-sort items cmp
   key #t): items in the order cmp gives, cmp called with two of them (with
   key, with the keys key made of them, each once; key may be nil) and
   answering a number, below 0 when the first goes first (as byte-cmp
   does); equal ones keep their order (a merge sort); #t keeps the first of
   each run of equal ones (sort -u).
   What a key or a comparison leaves behind goes back at once: a sort of
   many lines holds their keys, not every piece made to get them. */
static int b_list_sort(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    bool keyed = false;
    if (n >= 3 && a[2].kind == FILO_FUNC) {
        keyed = true;
    }
    bool once = false;
    if (n == 4 && a[3].kind == FILO_BOOL) {
        once = a[3].u.b;
    }
    if (n < 2 || n > 4 || a[0].kind != FILO_LIST || a[1].kind != FILO_FUNC ||
        (n >= 3 && !keyed && !(a[2].kind == FILO_LIST && a[2].u.seq.len == 0)) ||
        (n == 4 && a[3].kind != FILO_BOOL)) {
        return filo_fail(ctx, "list-sort expects a list, a function of two, maybe a key and #t");
    }
    filo_seq items; /* a (range 0 n) holds no items until asked */
    if (filo_arg_list(ctx, &a[0], &items) != FILO_OK) {
        return FILO_ERR;
    }
    uint32_t len = items.len;
    if (len < 2) {
        *out = a[0];
        return FILO_OK;
    }
    /* with a key, pairs: [2i] what is compared, [2i+1] the item it stands
       for; without, the items alone */
    size_t w = keyed ? 2U : 1U;
    filo_value *cur = filo_alloc(ctx, (size_t)len * w * sizeof(filo_value));
    filo_value *next = filo_alloc(ctx, (size_t)len * w * sizeof(filo_value));
    if (cur == NULL || next == NULL) {
        return filo_fail(ctx, "list-sort: out of memory");
    }
    for (uint32_t i = 0; i < len; i++) {
        cur[w * i] = items.items[i];
        cur[(w * i) + (w - 1)] = items.items[i];
    }
    filo_value key_fn = keyed ? a[2] : a[1];
    filo_value cmp_fn = a[1];
    bool unique = once;
    for (uint32_t i = 0; keyed && i < len; i++) {
        filo_value item = cur[(w * i) + 1];
        size_t mark = ctx->run.used;
        if (filo_call(ctx, &key_fn, &item, 1, &cur[w * i]) != FILO_OK) {
            return FILO_ERR;
        }
        filo_keep(ctx, mark, &cur[w * i]); /* the key stays, its making goes */
    }
    for (uint32_t width = 1; width < len; width *= 2) {
        for (uint32_t lo = 0; lo < len; lo += 2 * width) {
            uint32_t mid = lo + width < len ? lo + width : len;
            uint32_t hi = mid + width < len ? mid + width : len;
            uint32_t i = lo;
            uint32_t j = mid;
            uint32_t k = lo;
            while (i < mid || j < hi) {
                bool right = i >= mid;
                if (i < mid && j < hi) {
                    filo_value pair[2] = {cur[w * j], cur[w * i]};
                    filo_value r = {0};
                    size_t mark = ctx->run.used;
                    if (filo_call(ctx, &cmp_fn, pair, 2, &r) != FILO_OK) {
                        return FILO_ERR;
                    }
                    if (r.kind != FILO_NUMBER) {
                        return filo_fail(ctx, "list-sort: the function answers a number");
                    }
                    filo_keep(ctx, mark, &r);
                    right = r.u.num < 0; /* the right one first only when strictly before */
                }
                uint32_t from = right ? j++ : i++;
                memcpy(&next[w * k], &cur[w * from], w * sizeof(filo_value));
                k++;
            }
        }
        filo_value *t = cur;
        cur = next;
        next = t;
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (unique && kept > 0) {
            /* against the one before it: a run of equal ones keeps its first */
            filo_value pair[2] = {cur[w * (i - 1)], cur[w * i]};
            filo_value r = {0};
            size_t mark = ctx->run.used;
            if (filo_call(ctx, &cmp_fn, pair, 2, &r) != FILO_OK) {
                return FILO_ERR;
            }
            filo_keep(ctx, mark, &r);
            if (r.kind == FILO_NUMBER && r.u.num == 0) {
                continue;
            }
        }
        next[kept++] = cur[(w * i) + (w - 1)];
    }
    return filo_list(ctx, next, kept, out);
}

const script_builtin script_data_builtins[] = {
    {"iterate", b_iterate},
    {"list-sort", b_list_sort},
    {"getopt", b_getopt},
    {"env-get", b_env_get},
    {"env-list", b_env_list},
    {"now", b_now},
    {"glob-match", b_glob_match},
    {"path-resolve", b_path_resolve},
    {"file-stat", b_file_stat},
    {"file-id", b_file_id},
    {"file-runs", b_file_runs},
    {"file-touch", b_file_touch},
    {"time-zone", b_time_zone},
    {"out-terminal", b_out_terminal},
    {"store-info", b_store_info},
    {"dir-read", b_dir_read},
    {"int-text", b_int_text},
    {"byte-len", b_byte_len},
    {"byte-at", b_byte_at},
    {"byte-sub", b_byte_sub},
    {"byte-find", b_byte_find},
    {"byte-cmp", b_byte_cmp},
    {"bytes", b_bytes},
    {"byte-list", b_byte_list},
    {"u32", b_u32},
    {"u32-and", b_u32_and},
    {"u32-or", b_u32_or},
    {"u32-xor", b_u32_xor},
    {"u32-not", b_u32_not},
    {"u32-shl", b_u32_shl},
    {"u32-shr", b_u32_shr},
    {"u32-add", b_u32_add},
    {"u32-mul", b_u32_mul},
    {"re-compile", b_re_compile},
    {"re-match", b_re_match},
    {"re-free", b_re_free},
    {"utf8-valid", b_utf8_valid},
    {"utf8-runes", b_utf8_runes},
    {"utf8-encode", b_utf8_encode},
    {"str-width", b_str_width},
    {"rune-width", b_rune_width},
};
const size_t script_data_count = sizeof(script_data_builtins) / sizeof(script_data_builtins[0]);
