/* Regular expressions as POSIX writes them: basic (BRE: \( \) \{m,n\} \1,
   with GNU's \+ \? \| too) and extended (ERE: ( ) { } + ? |), with
   [...] [^...] [[:class:]], . ^ $ and backreferences. The match is the
   leftmost-longest, as POSIX says, found by backtracking that stops at a
   bound of steps rather than hang on a pathological pattern. No malloc:
   the compiled program lives in the struct. */
#ifndef ROC_REGEX_H
#define ROC_REGEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "roc_config.h"

enum {
    RE_INST_MAX = ROC_CFG_RE_INST,
    RE_CLASS_MAX = ROC_CFG_RE_CLASSES,
    RE_GROUPS = 10, /* \0 the whole match, \1..\9 */
    RE_LOOPS = 32,
};

typedef struct {
    uint8_t op;
    uint8_t arg; /* the byte, the class, the group, the loop */
    int16_t x;   /* jumps: relative */
    int16_t y;
} re_inst;

typedef struct {
    re_inst prog[RE_INST_MAX];
    int n;
    uint8_t classes[RE_CLASS_MAX][32];
    int nclasses;
    int ngroups;
    int nloops;
    bool icase;
    bool anchored; /* starts with ^: only a match at a line's start */
} sh_regex;

/* pattern into re: extended for ERE, icase to fold ASCII case. False with
   why for a pattern it cannot read. */
bool sh_regex_compile(sh_regex *re, const char *pattern, bool extended, bool icase, char *why,
                      size_t cap);

/* The first match in s[0..len) at or after from: 1 with so/eo (RE_GROUPS
   each, -1 for a group that took no part) set, 0 for none, -1 when the
   pattern took too many steps. not_bol: s is not a line's start (^ fails). */
int sh_regex_exec(const sh_regex *re, const char *s, size_t len, size_t from, bool not_bol,
                  long *so, long *eo);

#endif
