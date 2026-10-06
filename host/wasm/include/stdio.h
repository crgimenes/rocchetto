/* Minimal stdio.h for the freestanding wasm32 build: the text of integers
   and strings the Filo tools write listings with (fbc_dump.c,
   fbc_decompile.c), implemented in host/wasm/libc.c. No floating point:
   numbers come from the runtime's own writer. */
#ifndef ROC_WASM_STDIO_H
#define ROC_WASM_STDIO_H

#include <stdarg.h>
#include <stddef.h>

int snprintf(char *dst, size_t cap, const char *fmt, ...);
int vsnprintf(char *dst, size_t cap, const char *fmt, va_list ap);

#endif
