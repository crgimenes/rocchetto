#ifndef ROC_SCRIPT_DATA_H
#define ROC_SCRIPT_DATA_H

/* What utilities written in Filo are made of, beyond the language's
   strings: bytes (positions and lengths count bytes, a NUL is a byte like
   any other), unsigned 32-bit words, compiled regular expressions (the
   shell's own engine, offsets in bytes) and UTF-8 (code points, terminal
   columns). str-len and str-sub keep counting code points. */

#include <stddef.h>

#include "filo.h"

typedef struct {
    const char *name;
    filo_builtin fn;
} script_builtin;

/* A whole number (|x| <= 2^53) in base 2..36 into buf (66 bytes): the
   length. */
size_t script_whole_text(double x, unsigned base, char *buf);

extern const script_builtin script_data_builtins[];
extern const size_t script_data_count;

#endif
