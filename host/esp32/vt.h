#ifndef ROC_ESP32_VT_H
#define ROC_ESP32_VT_H

#include <stddef.h>
#include <stdint.h>

/* The board's own screen as a terminal: bytes of ANSI in, cells on the
   LCD out. Only what roc emits is understood — SGR, erase, absolute and
   relative cursor moves — and the modes it turns on and off are ignored
   on purpose, because this terminal has no alternate buffer to switch to
   and no mouse to report. */

void vt_begin(uint16_t cols, uint16_t rows);
void vt_feed(const uint8_t *p, size_t n);

/* Paints the cells that changed since the last call. */
void vt_flush(void);

/* The grid as plain text, for checking the panel from a wire. */
size_t vt_dump(char *out, size_t cap);

#endif
