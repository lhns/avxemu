/* Expected failures: test/known-failures.txt, one tab-separated line per entry:
 *   id  case-glob  forms-glob|-  reason
 * A failing case must match an entry; an entry whose cases ran and none failed is an unexpected
 * pass. Both turn the run red. forms-glob names the manifest forms the entry marks `failing`. */
#ifndef AVXEMU_KNOWN_H
#define AVXEMU_KNOWN_H
typedef struct { char id[64], cases[160], forms[160], reason[256]; unsigned ran, failed; } known_entry;
int known_load(const char *path);               /* entry count or -1; NULL/"" path = none */
int known_count(void);
const known_entry *known_get(int i);
void known_case(const char *name, int ok);      /* record one case result; prints unexpected failures */
void known_reset_counts(void);
/* Prints a summary; returns the number of problems (unexpected failures + unexpected passes). */
int known_finish(const char *suite);
#endif
