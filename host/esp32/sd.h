#ifndef ROC_ESP32_SD_H
#define ROC_ESP32_SD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The microSD card, as the shell's filesystem. Each call hands its bytes
   to a flush function rather than a buffer: an index of a full card does
   not fit anywhere this board has room for, and roc takes its answers in
   pieces anyway. */

bool sd_begin(void);
bool sd_mounted(void);

/* Why it is not mounted, in the IDF's own words. */
const char *sd_why(void);

/* Walks the card and emits the index roc parses: path, size, date, title,
   tab separated, directories with a trailing slash. The date is "-" —
   there is no clock here and a wrong one would be worse. */
void sd_index(void (*flush)(const uint8_t *, size_t));

/* False when there is no such file. */
bool sd_read(const char *path, void (*flush)(const uint8_t *, size_t));
bool sd_write(const char *path, const uint8_t *data, size_t len);
bool sd_unlink(const char *path);

/* A whole file, in a buffer that stays valid until the next call — the
   shape the core wants for a synchronous read. NULL when there is none,
   or when it is larger than the buffer. */
const uint8_t *sd_slurp(const char *path, size_t *len);

/* Makes the directories a path needs, so writing to /home/crg/x works on
   a card that has never seen /home. */
void sd_mkpath(const char *path);

#endif
