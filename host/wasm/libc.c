/* Tiny libc for the freestanding wasm32 build. Correctness over speed: the
   core moves kilobytes, not gigabytes. */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    size_t i = 0;
    while (i < n) {
        d[i] = s[i];
        i++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        size_t i = 0;
        while (i < n) {
            d[i] = s[i];
            i++;
        }
        return dst;
    }
    size_t i = n;
    while (i > 0) {
        i--;
        d[i] = s[i];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    size_t i = 0;
    while (i < n) {
        d[i] = (unsigned char)c;
        i++;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a;
    const unsigned char *y = b;
    size_t i = 0;
    while (i < n) {
        if (x[i] != y[i]) {
            return x[i] < y[i] ? -1 : 1;
        }
        i++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c) {
            return (void *)(p + i);
        }
    }
    return NULL;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    unsigned char x = (unsigned char)a[i];
    unsigned char y = (unsigned char)b[i];
    if (x == y) {
        return 0;
    }
    return x < y ? -1 : 1;
}

int strncmp(const char *a, const char *b, size_t n) {
    size_t i = 0;
    while (i < n && a[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    if (i == n) {
        return 0;
    }
    unsigned char x = (unsigned char)a[i];
    unsigned char y = (unsigned char)b[i];
    if (x == y) {
        return 0;
    }
    return x < y ? -1 : 1;
}

char *strstr(const char *hay, const char *needle) {
    if (needle[0] == '\0') {
        return (char *)hay;
    }
    for (size_t i = 0; hay[i] != '\0'; i++) {
        size_t k = 0;
        while (needle[k] != '\0' && hay[i + k] == needle[k]) {
            k++;
        }
        if (needle[k] == '\0') {
            return (char *)(hay + i);
        }
    }
    return NULL;
}

/* How far a run of characters that are (strspn) or are not (strcspn) in
   set reaches: the word splitters in the shell lean on both. */
static size_t span(const char *s, const char *set, bool want) {
    size_t n = 0;
    while (s[n] != '\0') {
        bool in_set = strchr(set, s[n]) != NULL;
        if (in_set != want) {
            return n;
        }
        n++;
    }
    return n;
}

size_t strspn(const char *s, const char *set) {
    return span(s, set, true);
}

size_t strcspn(const char *s, const char *set) {
    return span(s, set, false);
}

char *strchr(const char *s, int c) {
    size_t i = 0;
    for (;;) {
        if (s[i] == (char)c) {
            return (char *)(s + i);
        }
        if (s[i] == '\0') {
            return NULL;
        }
        i++;
    }
}

char *strrchr(const char *s, int c) {
    char *last = NULL;
    size_t i = 0;
    for (;;) {
        if (s[i] == (char)c) {
            last = (char *)(s + i);
        }
        if (s[i] == '\0') {
            return last;
        }
        i++;
    }
}

char *strcpy(char *dst, const char *src) {
    size_t i = 0;
    for (;;) {
        dst[i] = src[i];
        if (src[i] == '\0') {
            return dst;
        }
        i++;
    }
}

char *strcat(char *dst, const char *src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

/* ---- snprintf: %d %i %u %x %c %s %%, a width, '-' and '0', a precision
   (".N" or ".*") for strings, and the l, ll and z sizes. Enough for the
   listings; no floating point, which the runtime writes itself. ---- */

typedef struct {
    char *dst;
    size_t cap;
    size_t n; /* written, or that would have been */
} out_buf;

static void emit(out_buf *o, char c) {
    if (o->n + 1 < o->cap) {
        o->dst[o->n] = c;
    }
    o->n++;
}

static void emit_padded(out_buf *o, const char *s, size_t len, int width, bool left, char pad) {
    size_t w = width > 0 ? (size_t)width : 0;
    if (!left) {
        for (size_t i = len; i < w; i++) {
            emit(o, pad);
        }
    }
    for (size_t i = 0; i < len; i++) {
        emit(o, s[i]);
    }
    if (left) {
        for (size_t i = len; i < w; i++) {
            emit(o, ' ');
        }
    }
}

int vsnprintf(char *dst, size_t cap, const char *fmt, va_list ap) {
    out_buf o = {dst, cap, 0};
    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            emit(&o, *p);
            continue;
        }
        p++;
        bool left = false;
        char pad = ' ';
        for (; *p == '-' || *p == '0'; p++) {
            if (*p == '-') {
                left = true;
            } else {
                pad = '0';
            }
        }
        int width = 0;
        for (; *p >= '0' && *p <= '9'; p++) {
            width = width * 10 + (*p - '0');
        }
        int prec = -1;
        if (*p == '.') {
            p++;
            if (*p == '*') {
                prec = va_arg(ap, int);
                p++;
            } else {
                prec = 0;
                for (; *p >= '0' && *p <= '9'; p++) {
                    prec = prec * 10 + (*p - '0');
                }
            }
        }
        int size = 0; /* 1 long, 2 long long, 3 size_t */
        if (*p == 'l') {
            size = 1;
            p++;
            if (*p == 'l') {
                size = 2;
                p++;
            }
        } else if (*p == 'z') {
            size = 3;
            p++;
        }
        char num[24];
        size_t len = 0;
        switch (*p) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == NULL) {
                s = "(null)";
            }
            size_t sl = 0;
            while (s[sl] != '\0' && (prec < 0 || sl < (size_t)prec)) {
                sl++;
            }
            emit_padded(&o, s, sl, width, left, ' ');
            continue;
        }
        case 'c':
            num[0] = (char)va_arg(ap, int);
            emit_padded(&o, num, 1, width, left, ' ');
            continue;
        case '%':
            emit(&o, '%');
            continue;
        case 'd':
        case 'i': {
            long long v = size == 2   ? va_arg(ap, long long)
                          : size == 1 ? va_arg(ap, long)
                          : size == 3 ? (long long)va_arg(ap, size_t)
                                      : va_arg(ap, int);
            unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
            do {
                num[len++] = (char)('0' + (u % 10U));
                u /= 10U;
            } while (u > 0);
            if (v < 0) {
                num[len++] = '-';
            }
            break;
        }
        case 'u':
        case 'x': {
            unsigned long long u = size == 2   ? va_arg(ap, unsigned long long)
                                   : size == 1 ? va_arg(ap, unsigned long)
                                   : size == 3 ? va_arg(ap, size_t)
                                               : va_arg(ap, unsigned);
            unsigned base = *p == 'x' ? 16U : 10U;
            do {
                num[len++] = "0123456789abcdef"[u % base];
                u /= base;
            } while (u > 0);
            break;
        }
        default: /* not a conversion this knows: written as it came */
            emit(&o, '%');
            if (*p == '\0') {
                p--;
            } else {
                emit(&o, *p);
            }
            continue;
        }
        /* num holds the digits backwards, a minus last */
        char text[24];
        for (size_t i = 0; i < len; i++) {
            text[i] = num[len - 1 - i];
        }
        if (pad == '0' && !left && text[0] == '-') {
            emit(&o, '-');
            emit_padded(&o, text + 1, len - 1, width - 1, false, '0');
        } else {
            emit_padded(&o, text, len, width, left, left ? ' ' : pad);
        }
    }
    if (cap > 0) {
        dst[o.n < cap ? o.n : cap - 1] = '\0';
    }
    return (int)o.n;
}

int snprintf(char *dst, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    return n;
}
