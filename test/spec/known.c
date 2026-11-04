#define _GNU_SOURCE
#include "known.h"
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static known_entry *ent; static int nent;
static unsigned unexpected, expected, total;

int known_load(const char *path) {
    nent = 0;
    if (!path || !*path) return 0;
    FILE *fp = fopen(path, "r");
    if (!fp) { perror(path); return -1; }
    char line[1024];
    while (fgets(line, sizeof line, fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        line[strcspn(line, "\n")] = 0;
        char *c[4]; int n = 0;
        for (char *p = line; n < 4; ) { c[n++] = p; p = strchr(p, '\t'); if (!p) break; *p++ = 0; }
        if (n != 4) { fprintf(stderr, "%s: need 4 tab-separated columns: %.60s\n", path, line); return -1; }
        ent = realloc(ent, (nent + 1) * sizeof *ent);
        known_entry *e = &ent[nent++]; memset(e, 0, sizeof *e);
        snprintf(e->id, sizeof e->id, "%s", c[0]); snprintf(e->cases, sizeof e->cases, "%s", c[1]);
        snprintf(e->forms, sizeof e->forms, "%s", c[2]); snprintf(e->reason, sizeof e->reason, "%s", c[3]);
    }
    fclose(fp); return nent;
}
int known_count(void) { return nent; }
const known_entry *known_get(int i) { return &ent[i]; }
void known_reset_counts(void) { for (int i = 0; i < nent; i++) ent[i].ran = ent[i].failed = 0; unexpected = expected = total = 0; }

int known_case(const char *name, int ok) {
    int hit = 0; total++;
    for (int i = 0; i < nent; i++) {
        if (fnmatch(ent[i].cases, name, FNM_EXTMATCH)) continue;
        ent[i].ran++; if (!ok) { ent[i].failed++; hit = 1; }
    }
    if (ok) return 0;
    if (hit) { expected++; return 0; }
    if (unexpected++ < (getenv("KNOWN_VERBOSE") ? 1000000u : 40u)) printf("FAIL %s\n", name);
    return 1;
}

int known_finish(const char *suite) {
    int bad = (int)unexpected;
    for (int i = 0; i < nent; i++) {
        if (strncmp(ent[i].cases, suite, strlen(suite))) continue;
        if (ent[i].ran && getenv("KNOWN_VERBOSE")) printf("  known %s: %u cases ran, %u failed\n", ent[i].id, ent[i].ran, ent[i].failed);
        if (ent[i].ran && !ent[i].failed && !strstr(ent[i].reason, "[compiler-dependent]")) { printf("XPASS known failure %s now passes (%u cases): remove it\n", ent[i].id, ent[i].ran); bad++; }
    }
    printf("%s: %u cases, %u expected failures, %u unexpected failures, %d problems\n", suite, total, expected, unexpected, bad);
    return bad;
}
