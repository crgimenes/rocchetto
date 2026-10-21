#ifndef ROC_QR_H
#define ROC_QR_H

#include <stddef.h>
#include <stdint.h>

/* QR codes (ISO/IEC 18004), byte mode, error correction level M, versions 1
   to 6: up to 106 bytes, a URL's worth, with no version information to
   draw. The mask is chosen by the standard's penalty rules. */

enum { QR_SIZE_MAX = 41, QR_BYTES_MAX = 106 };

/* The modules of the code for data, 1 dark, size by size (row by row in
   out, QR_SIZE_MAX apart); the size, or 0 when data is too long. */
int qr_encode(const uint8_t *data, size_t n, uint8_t out[QR_SIZE_MAX][QR_SIZE_MAX]);

#endif
