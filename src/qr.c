#include <stdbool.h>
#include <string.h>

#include "qr.h"

/* Level M, versions 1 to 6: all codewords, error correction codewords per
   block, blocks (of equal size at these versions), and the one alignment
   pattern's centre (0: none, version 1). */
static const struct {
    uint8_t total;
    uint8_t ec;
    uint8_t blocks;
    uint8_t align;
} versions[7] = {
    {0, 0, 0, 0},     {26, 10, 1, 0},   {44, 16, 1, 18},  {70, 26, 1, 22},
    {100, 18, 2, 26}, {134, 24, 2, 30}, {172, 16, 4, 34},
};

enum { CODEWORDS_MAX = 172, EC_MAX = 26 };

/* ---- GF(256), the polynomial 0x11D ---- */

static uint8_t gf_exp[512];
static uint8_t gf_log[256];

static void gf_init(void) {
    if (gf_exp[0] == 1) {
        return;
    }
    unsigned x = 1;
    for (unsigned i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1U;
        if (x & 0x100U) {
            x ^= 0x11DU;
        }
    }
    for (unsigned i = 255; i < 512; i++) {
        gf_exp[i] = gf_exp[i - 255];
    }
}

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) {
        return 0;
    }
    return gf_exp[gf_log[a] + gf_log[b]];
}

/* The ec codewords of a block: the remainder of data * x^ec by the
   generator (x - a^0)(x - a^1)...(x - a^(ec-1)). */
static void rs_block(const uint8_t *data, size_t n, size_t ec, uint8_t *out) {
    uint8_t gen[EC_MAX + 1];
    memset(gen, 0, sizeof(gen));
    gen[0] = 1;
    for (size_t i = 0; i < ec; i++) {
        /* gen *= (x - a^i), highest power first */
        for (size_t j = i + 1; j > 0; j--) {
            gen[j] = (uint8_t)(gen[j] ^ gf_mul(gen[j - 1], gf_exp[i]));
        }
    }
    memset(out, 0, ec);
    for (size_t i = 0; i < n; i++) {
        uint8_t factor = (uint8_t)(data[i] ^ out[0]);
        memmove(out, out + 1, ec - 1);
        out[ec - 1] = 0;
        for (size_t j = 0; j < ec; j++) {
            out[j] = (uint8_t)(out[j] ^ gf_mul(gen[j + 1], factor));
        }
    }
}

/* ---- the codewords: header, data, padding, then error correction ---- */

static void put_bits(uint8_t *buf, size_t *at, unsigned value, unsigned count) {
    for (unsigned i = count; i > 0; i--) {
        if ((value >> (i - 1U)) & 1U) {
            buf[*at / 8] = (uint8_t)(buf[*at / 8] | (0x80U >> (*at % 8)));
        }
        (*at)++;
    }
}

static size_t codewords(int v, const uint8_t *data, size_t n, uint8_t out[CODEWORDS_MAX]) {
    size_t total = versions[v].total;
    size_t ec = versions[v].ec;
    size_t blocks = versions[v].blocks;
    size_t cap = total - (ec * blocks);
    uint8_t buf[CODEWORDS_MAX];
    memset(buf, 0, sizeof(buf));
    size_t at = 0;
    put_bits(buf, &at, 4, 4); /* byte mode */
    put_bits(buf, &at, (unsigned)n, 8);
    for (size_t i = 0; i < n; i++) {
        put_bits(buf, &at, data[i], 8);
    }
    size_t room = (cap * 8) - at;
    put_bits(buf, &at, 0, room < 4 ? (unsigned)room : 4); /* the terminator */
    at = (at + 7) / 8 * 8;
    for (size_t i = at / 8, k = 0; i < cap; i++, k++) {
        buf[i] = (k % 2 == 0) ? 0xEC : 0x11;
    }
    /* the blocks' data interleaved a codeword at a time, then their ec */
    size_t per = cap / blocks;
    uint8_t ecc[4][EC_MAX];
    for (size_t b = 0; b < blocks; b++) {
        rs_block(buf + (b * per), per, ec, ecc[b]);
    }
    size_t k = 0;
    for (size_t i = 0; i < per; i++) {
        for (size_t b = 0; b < blocks; b++) {
            out[k++] = buf[(b * per) + i];
        }
    }
    for (size_t i = 0; i < ec; i++) {
        for (size_t b = 0; b < blocks; b++) {
            out[k++] = ecc[b][i];
        }
    }
    return k;
}

/* ---- the matrix ---- */

static uint8_t mod[QR_SIZE_MAX][QR_SIZE_MAX];
static bool fixed[QR_SIZE_MAX][QR_SIZE_MAX]; /* a function module, no data */
static int size;

static void set_fixed(int r, int c, unsigned dark) {
    if (r >= 0 && r < size && c >= 0 && c < size) {
        mod[r][c] = (uint8_t)(dark & 1U);
        fixed[r][c] = true;
    }
}

static void finder(int r0, int c0) {
    for (int r = -1; r <= 7; r++) {
        for (int c = -1; c <= 7; c++) {
            unsigned ring = (r == 0 || r == 6 || c == 0 || c == 6) ? 1U : 0U;
            unsigned core = (r >= 2 && r <= 4 && c >= 2 && c <= 4) ? 1U : 0U;
            unsigned inside = (r >= 0 && r <= 6 && c >= 0 && c <= 6) ? 1U : 0U;
            set_fixed(r0 + r, c0 + c, inside & (ring | core)); /* -1 and 7: the light separator */
        }
    }
}

static void alignment(int at) {
    for (int r = -2; r <= 2; r++) {
        for (int c = -2; c <= 2; c++) {
            int d = r < 0 ? -r : r;
            int e = c < 0 ? -c : c;
            set_fixed(at + r, at + c, ((d > e ? d : e) != 1) ? 1U : 0U);
        }
    }
}

/* the format information: level M (00) and the mask, BCH(15,5), masked */
static void format(int mask) {
    unsigned data = (unsigned)mask; /* M's two bits are 00 */
    unsigned rem = data;
    for (int i = 0; i < 10; i++) {
        rem = (rem << 1U) ^ (((rem >> 9U) & 1U) * 0x537U);
    }
    unsigned bits = ((data << 10U) | rem) ^ 0x5412U;
    for (int i = 0; i <= 5; i++) {
        set_fixed(i, 8, (bits >> (unsigned)i) & 1U);
    }
    set_fixed(7, 8, (bits >> 6U) & 1U);
    set_fixed(8, 8, (bits >> 7U) & 1U);
    set_fixed(8, 7, (bits >> 8U) & 1U);
    for (int i = 9; i < 15; i++) {
        set_fixed(8, 14 - i, (bits >> (unsigned)i) & 1U);
    }
    for (int i = 0; i < 8; i++) {
        set_fixed(8, size - 1 - i, (bits >> (unsigned)i) & 1U);
    }
    for (int i = 8; i < 15; i++) {
        set_fixed(size - 15 + i, 8, (bits >> (unsigned)i) & 1U);
    }
    set_fixed(size - 8, 8, 1U); /* the dark module */
}

static void function_patterns(int v) {
    memset(mod, 0, sizeof(mod));
    memset(fixed, 0, sizeof(fixed));
    for (int i = 0; i < size; i++) {
        set_fixed(6, i, (i % 2 == 0) ? 1U : 0U); /* timing */
        set_fixed(i, 6, (i % 2 == 0) ? 1U : 0U);
    }
    finder(0, 0);
    finder(0, size - 7);
    finder(size - 7, 0);
    if (versions[v].align > 0) {
        alignment(versions[v].align);
    }
    format(0); /* reserves the format modules; drawn again with the mask */
}

/* the codewords' bits, two columns at a time from the right, up and down */
static void place(const uint8_t *cw, size_t n) {
    size_t bit = 0;
    int up = 1;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) {
            right = 5; /* the vertical timing pattern is no data column */
        }
        for (int k = 0; k < size; k++) {
            int r = up != 0 ? size - 1 - k : k;
            for (int j = 0; j < 2; j++) {
                int c = right - j;
                if (fixed[r][c]) {
                    continue;
                }
                unsigned dark = bit < n * 8 ? (unsigned)(cw[bit / 8] >> (7U - (bit % 8))) & 1U : 0U;
                mod[r][c] = (uint8_t)dark;
                bit++;
            }
        }
        up = 1 - up;
    }
}

static bool masked(int mask, int r, int c) {
    switch (mask) {
    case 0:
        return (r + c) % 2 == 0;
    case 1:
        return r % 2 == 0;
    case 2:
        return c % 3 == 0;
    case 3:
        return (r + c) % 3 == 0;
    case 4:
        return ((r / 2) + (c / 3)) % 2 == 0;
    case 5:
        return ((r * c) % 2) + ((r * c) % 3) == 0;
    case 6:
        return (((r * c) % 2) + ((r * c) % 3)) % 2 == 0;
    default:
        return (((r + c) % 2) + ((r * c) % 3)) % 2 == 0;
    }
}

static void apply_mask(int mask) {
    for (int r = 0; r < size; r++) {
        for (int c = 0; c < size; c++) {
            if (!fixed[r][c] && masked(mask, r, c)) {
                mod[r][c] ^= 1U;
            }
        }
    }
}

/* ---- the penalty of a masked symbol ---- */

static uint8_t module_at(int r, int c, bool across) {
    return across ? mod[r][c] : mod[c][r];
}

/* runs of five or more alike, and the finder's 1:1:3:1:1 with four light
   beside it, along every row (across) or every column */
static long lines_penalty(bool across) {
    static const uint8_t finder_a[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
    static const uint8_t finder_b[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
    long p = 0;
    for (int i = 0; i < size; i++) {
        int run = 1;
        for (int j = 1; j <= size; j++) {
            if (j < size && module_at(i, j, across) == module_at(i, j - 1, across)) {
                run++;
                continue;
            }
            if (run >= 5) {
                p += 3 + (run - 5);
            }
            run = 1;
        }
        for (int j = 0; j + 11 <= size; j++) {
            int a = 0; /* modules matching each way round */
            int b = 0;
            for (int k = 0; k < 11; k++) {
                a += module_at(i, j + k, across) == finder_a[k] ? 1 : 0;
                b += module_at(i, j + k, across) == finder_b[k] ? 1 : 0;
            }
            p += (a == 11 ? 40 : 0) + (b == 11 ? 40 : 0);
        }
    }
    return p;
}

static long penalty(void) {
    long p = lines_penalty(true) + lines_penalty(false);
    long dark = 0;
    for (int r = 0; r < size; r++) {
        for (int c = 0; c < size; c++) {
            dark += mod[r][c];
            if (r + 1 < size && c + 1 < size && mod[r][c] == mod[r][c + 1] &&
                mod[r][c] == mod[r + 1][c] && mod[r][c] == mod[r + 1][c + 1]) {
                p += 3;
            }
        }
    }
    long total = (long)size * size;
    long percent = (dark * 100) / total;
    long off = percent >= 50 ? percent - 50 : 50 - percent;
    return p + ((off / 5) * 10);
}

int qr_encode(const uint8_t *data, size_t n, uint8_t out[QR_SIZE_MAX][QR_SIZE_MAX]) {
    int v = 1;
    while (v <= 6 && n + 2 > (size_t)(versions[v].total - (versions[v].ec * versions[v].blocks))) {
        v++;
    }
    if (v > 6) {
        return 0;
    }
    gf_init();
    uint8_t cw[CODEWORDS_MAX];
    size_t k = codewords(v, data, n, cw);
    size = 17 + (4 * v);
    int best = 0;
    long best_penalty = -1;
    for (int mask = 0; mask < 8; mask++) {
        function_patterns(v);
        place(cw, k);
        apply_mask(mask);
        format(mask);
        long p = penalty();
        if (best_penalty < 0 || p < best_penalty) {
            best_penalty = p;
            best = mask;
        }
    }
    function_patterns(v);
    place(cw, k);
    apply_mask(best);
    format(best);
    for (int r = 0; r < size; r++) {
        memcpy(out[r], mod[r], (size_t)size);
    }
    return size;
}
