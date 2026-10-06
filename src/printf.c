#include "printf.h"

#include "roc.h"

#include <string.h>

/* What printf writes, gathered: data (roc_out), byte for byte into a > or
   a |, a \n made \r\n on the terminal. */
typedef struct {
    roc *m;
    uint8_t buf[256];
    size_t n;
    bool stop; /* \c in a %b: nothing more at all */
} out;

static void flush(out *o) {
    roc_out(o->m, o->buf, o->n);
    o->n = 0;
}

static void put(out *o, char c) {
    if (o->n + 1 > sizeof(o->buf)) {
        flush(o);
    }
    o->buf[o->n++] = (uint8_t)c;
}

static void put_n(out *o, char c, size_t n) {
    for (size_t i = 0; i < n; i++) {
        put(o, c);
    }
}

/* The escape at *p (past the \): the byte it stands for, *p past it.
   octal: \NNN as in a format, else \0NNN as in %b. */
static char escape(const char **p, bool format) {
    char c = **p;
    static const char from[] = "\\abfnrtv\"";
    static const char to[] = "\\\a\b\f\n\r\t\v\"";
    const char *hit = c != '\0' ? strchr(from, c) : NULL;
    if (hit != NULL) {
        (*p)++;
        return to[hit - from];
    }
    const char *q = *p;
    if (!format && *q == '0') {
        q++;
    }
    if (*q >= '0' && *q <= '7') {
        unsigned v = 0;
        for (int i = 0; i < 3 && *q >= '0' && *q <= '7'; i++) {
            v = (v * 8U) + (unsigned)(*q - '0');
            q++;
        }
        *p = q;
        return (char)(v & 0xFFU);
    }
    return '\\'; /* not an escape: the \ itself, the byte after it as it is */
}

/* The number an argument is: decimal, 0 octal, 0x hex, a sign before;
   'c or "c the code of c. False, said, for anything else. */
static bool number(roc *m, const char *s, long long *v) {
    *v = 0;
    if (s[0] == '\'' || s[0] == '"') {
        *v = (unsigned char)s[1];
        return true;
    }
    const char *p = s;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    bool neg = false;
    if (*p == '-' || *p == '+') {
        neg = *p == '-';
        p++;
    }
    unsigned base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (p[0] == '0') {
        base = 8;
    }
    unsigned long long u = 0;
    bool any = false;
    for (;; p++) {
        unsigned d = 99;
        if (*p >= '0' && *p <= '9') {
            d = (unsigned)(*p - '0');
        } else if (*p >= 'a' && *p <= 'f') {
            d = (unsigned)(*p - 'a') + 10U;
        } else if (*p >= 'A' && *p <= 'F') {
            d = (unsigned)(*p - 'A') + 10U;
        }
        if (d >= base) {
            break;
        }
        u = (u * base) + d;
        any = true;
    }
    if (!any || *p != '\0') {
        roc_err(m, "printf", s, "Illegal number");
        return false;
    }
    *v = neg ? (long long)(0 - u) : (long long)u;
    return true;
}

typedef struct {
    bool left;  /* - */
    bool zero;  /* 0 */
    bool plus;  /* + */
    bool space; /* ' ' */
    bool alt;   /* # */
    size_t width;
    long prec; /* -1: none */
} spec;

/* text of n bytes in the field spec says: padded to the width. */
static void pad_out(out *o, const spec *sp, const char *text, size_t n) {
    size_t pad = sp->width > n ? sp->width - n : 0;
    if (!sp->left) {
        put_n(o, ' ', pad);
    }
    for (size_t i = 0; i < n; i++) {
        put(o, text[i]);
    }
    if (sp->left) {
        put_n(o, ' ', pad);
    }
}

static void integer(out *o, const spec *sp, char conv, long long v) {
    unsigned base = 10;
    if (conv == 'o') {
        base = 8;
    } else if (conv == 'x' || conv == 'X') {
        base = 16;
    }
    bool is_signed = false;
    if (conv == 'd' || conv == 'i') {
        is_signed = true;
    }
    unsigned long long u = (unsigned long long)v;
    const char *sign = "";
    if (is_signed && v < 0) {
        u = 0 - u;
        sign = "-";
    } else if (is_signed && sp->plus) {
        sign = "+";
    } else if (is_signed && sp->space) {
        sign = " ";
    }
    const char *digits = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    char d[32];
    size_t nd = 0;
    while (u > 0 && nd < sizeof(d)) {
        d[nd++] = digits[u % base];
        u /= base;
    }
    size_t prec = sp->prec >= 0 ? (size_t)sp->prec : 1;
    const char *prefix = "";
    if (sp->alt && conv == 'o' && nd >= prec) {
        prec = nd + 1; /* a 0 leads */
    } else if (sp->alt && (conv == 'x' || conv == 'X') && v != 0) {
        prefix = conv == 'x' ? "0x" : "0X";
    }
    size_t zeros = prec > nd ? prec - nd : 0;
    size_t len = strlen(sign) + strlen(prefix) + zeros + nd;
    size_t pad = sp->width > len ? sp->width - len : 0;
    bool zero_pad = false; /* 0 pads, unless - or a precision says otherwise */
    if (sp->zero && !sp->left && sp->prec < 0) {
        zero_pad = true;
    }
    if (!sp->left && !zero_pad) {
        put_n(o, ' ', pad);
    }
    for (const char *s = sign; *s != '\0'; s++) {
        put(o, *s);
    }
    for (const char *s = prefix; *s != '\0'; s++) {
        put(o, *s);
    }
    if (zero_pad) {
        put_n(o, '0', pad);
    }
    put_n(o, '0', zeros);
    while (nd > 0) {
        put(o, d[--nd]);
    }
    if (sp->left) {
        put_n(o, ' ', pad);
    }
}

/* %b: the argument with its escapes, \c ending everything. */
static void escaped(out *o, const spec *sp, const char *s) {
    static char text[1024];
    size_t n = 0;
    while (*s != '\0' && n < sizeof(text)) {
        if (s[0] == '\\' && s[1] == 'c') {
            o->stop = true;
            break;
        }
        if (s[0] == '\\' && s[1] != '\0') {
            s++;
            text[n++] = escape(&s, false); /* \x not an escape: \ now, x next */
            continue;
        }
        text[n++] = *s++;
    }
    size_t shown = n;
    if (sp->prec >= 0 && (size_t)sp->prec < shown) {
        shown = (size_t)sp->prec;
    }
    pad_out(o, sp, text, shown);
}

/* Reads a width or precision at *p: digits, or * for the next argument. */
static long amount(roc *m, const char **p, int *ai, int argc, char *const *argv) {
    if (**p == '*') {
        (*p)++;
        long long v = 0;
        if (*ai < argc) {
            (void)number(m, argv[(*ai)++], &v);
        }
        if (v < 0) {
            return 0;
        }
        return (long)(v > 4096 ? 4096 : v);
    }
    long v = 0;
    while (**p >= '0' && **p <= '9') {
        if (v < 4096) {
            v = (v * 10) + (**p - '0');
        }
        (*p)++;
    }
    return v;
}

void roc_cmd_printf(roc *m, int argc, char *const *argv) {
    if (argc < 2) {
        roc_err(m, "printf", "", "usage: printf format [arguments]");
        m->status = 2;
        return;
    }
    out o = {.m = m};
    const char *format = argv[1];
    int ai = 2;
    bool bad = false;
    for (;;) {
        int before = ai;
        for (const char *p = format; *p != '\0' && !o.stop;) {
            if (*p == '\\') {
                p++;
                put(&o, escape(&p, true));
                continue;
            }
            if (*p != '%') {
                put(&o, *p++);
                continue;
            }
            p++;
            if (*p == '%') {
                put(&o, '%');
                p++;
                continue;
            }
            spec sp = {.prec = -1};
            for (;; p++) {
                if (*p == '-') {
                    sp.left = true;
                } else if (*p == '0') {
                    sp.zero = true;
                } else if (*p == '+') {
                    sp.plus = true;
                } else if (*p == ' ') {
                    sp.space = true;
                } else if (*p == '#') {
                    sp.alt = true;
                } else {
                    break;
                }
            }
            long w = amount(m, &p, &ai, argc, argv);
            sp.width = (size_t)w;
            if (*p == '.') {
                p++;
                sp.prec = amount(m, &p, &ai, argc, argv);
            }
            char conv = *p;
            if (conv == '\0' || strchr("sbcdiuoxX", conv) == NULL) {
                roc_err(m, "printf", format, "bad conversion");
                m->status = 1;
                flush(&o);
                return;
            }
            p++;
            const char *arg = ai < argc ? argv[ai++] : NULL;
            if (conv == 's') {
                const char *s = arg != NULL ? arg : "";
                size_t n = strlen(s);
                if (sp.prec >= 0 && (size_t)sp.prec < n) {
                    n = (size_t)sp.prec;
                }
                pad_out(&o, &sp, s, n);
            } else if (conv == 'b') {
                escaped(&o, &sp, arg != NULL ? arg : "");
            } else if (conv == 'c') {
                pad_out(&o, &sp, arg != NULL ? arg : "", arg != NULL && arg[0] != '\0' ? 1 : 0);
            } else {
                long long v = 0;
                if (arg != NULL && !number(m, arg, &v)) {
                    bad = true;
                }
                integer(&o, &sp, conv, v);
            }
        }
        /* the format again for the arguments left, as long as it takes some */
        if (o.stop || ai >= argc || ai == before) {
            break;
        }
    }
    flush(&o);
    m->status = bad ? 1 : 0;
}
