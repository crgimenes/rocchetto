#ifndef ROC_RNG_H
#define ROC_RNG_H

#include <stdint.h>

/* splitmix64: the shell's random numbers, for the screens' (random n), the
   effects and the doors alike. One multiply-xorshift step a number. */
static inline uint64_t splitmix64(uint64_t *s) {
    *s += 0x9E3779B97F4A7C15ULL;
    uint64_t z = *s;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

#endif
