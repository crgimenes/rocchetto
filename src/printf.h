/* printf FORMAT [ARG...], as POSIX's: the format again while arguments are
   left; %s %b %c %d %i %u %o %x %X %%, the flags - 0 + space #, a width and
   a precision (* takes either from an argument), and \ escapes in the format
   (\n \t \\ \a \b \f \r \v \" \NNN octal). A number may be decimal, 0 octal,
   0x hex, or 'c for the code of c. $? 1 when an argument was no number. */
#ifndef ROC_PRINTF_H
#define ROC_PRINTF_H

typedef struct roc roc;

void roc_cmd_printf(roc *m, int argc, char *const *argv);

#endif
