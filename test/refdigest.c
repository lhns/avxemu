#include "refdigest.h"
#include "cpu.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

#define REF_MAX 2048
struct ref_line { char name[32]; unsigned long cases; uint64_t digest; int seen; };

static struct ref_line lines[REF_MAX];
static int nlines, record_failed;
static enum ref_mode mode;
static char suite[64], path[1024];

uint64_t ref_fnv1a(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = p;
    while (n--) { h ^= *b++; h *= 0x100000001b3ull; }
    return h;
}

void ref_reset(void) { nlines = 0; record_failed = 0; mode = REF_CHECK; suite[0] = 0; path[0] = 0; }

static int find_line(const char *name) {
    for (int k = 0; k < nlines; k++) if (strcmp(lines[k].name, name) == 0) return k;
    return -1;
}

static int add_line(const char *name, unsigned long cases, uint64_t digest) {
    if (nlines == REF_MAX || strlen(name) >= sizeof lines[0].name || find_line(name) >= 0) return 0;
    struct ref_line *l = &lines[nlines++];
    strcpy(l->name, name); l->cases = cases; l->digest = digest; l->seen = 0;
    return 1;
}

int ref_begin(enum ref_mode m, const char *s, const char *p) {
    ref_reset();
    mode = m;
    if (strlen(s) >= sizeof suite || strlen(p) >= sizeof path) {
        fprintf(stderr, "refdigest: suite or path too long\n"); return -1;
    }
    strcpy(suite, s); strcpy(path, p);
    if (m == REF_RECORD) return 0;
    FILE *f = fopen(p, "r");
    if (!f) {
        fprintf(stderr, "refdigest: cannot read %s -- record a reference on real silicon (DEVELOPING.md, \"Re-recording a reference\")\n", p);
        return -1;
    }
    char buf[256], want[128];
    int lineno = 0, rc = 0;
    snprintf(want, sizeof want, "# avxemu reference: %s\n", s);
    while (fgets(buf, sizeof buf, f)) {
        lineno++;
        if (lineno == 1 && strcmp(buf, want) != 0) {
            fprintf(stderr, "refdigest: %s is not a reference for %s\n", p, s); rc = -1; break;
        }
        if (buf[0] == '#' || buf[0] == '\n') continue;
        char name[64], digest[17], extra;
        unsigned long cases;
        int ok = sscanf(buf, "%63s %lu %16s %c", name, &cases, digest, &extra) == 3;
        if (ok && strlen(digest) != 16) ok = 0;
        if (ok) for (const char *c = digest; *c; c++) if (!isxdigit((unsigned char)*c)) { ok = 0; break; }
        if (!ok || !add_line(name, cases, ok ? strtoull(digest, NULL, 16) : 0)) {
            fprintf(stderr, "refdigest: %s:%d is malformed or duplicated\n", p, lineno); rc = -1; break;
        }
    }
    if (rc == 0 && lineno == 0) { fprintf(stderr, "refdigest: %s is empty\n", p); rc = -1; }
    fclose(f);
    return rc;
}

enum ref_mode ref_init(int argc, char **argv, const char *s, unsigned need) {
    if (argc != 3 || (strcmp(argv[1], "check") != 0 && strcmp(argv[1], "record") != 0)) {
        fprintf(stderr, "usage: %s check|record <reference-file>\n", argv[0]);
        exit(2);
    }
    enum ref_mode m = strcmp(argv[1], "record") == 0 ? REF_RECORD : REF_CHECK;
    if (m == REF_RECORD) {
        unsigned missing = need & ~cpu_features();
        if (missing) {
            printf("SKIP: record mode needs");
            for (int i = 0; i < CPU_NFEATURES; i++)
                if (missing & (1u << i)) printf(" %s", cpu_feature_name(1u << i));
            printf(", which this CPU lacks\n");
            exit(77);
        }
    }
    if (ref_begin(m, s, argv[2]) != 0) exit(1);
    return m;
}

void ref_op_init(struct ref_op *o, const char *name) {
    o->name = name; o->hw = o->emu = REF_FNV_OFFSET; o->cases = 0; o->live_bad = 0;
}
void ref_hw(struct ref_op *o, const void *p, size_t n)  { o->hw = ref_fnv1a(o->hw, p, n); }
void ref_emu(struct ref_op *o, const void *p, size_t n) { o->emu = ref_fnv1a(o->emu, p, n); }
void ref_case(struct ref_op *o, int live_ok) { o->cases++; if (!live_ok) o->live_bad++; }

int ref_report(const struct ref_op *ops, int n) {
    int bad = 0;
    for (int i = 0; i < n; i++) {
        const struct ref_op *o = &ops[i];
        const char *why = NULL;
        if (mode == REF_RECORD) {
            if (o->live_bad) why = "the emulator disagrees with this CPU";
            else if (o->hw != o->emu) why = "digests differ where the live comparison agreed: canonicalization bug";
            else if (!add_line(o->name, o->cases, o->hw)) why = "duplicate op name";
            if (why) record_failed = 1;
        } else {
            int k = find_line(o->name);
            if (k < 0) why = "not in the reference";
            else if (lines[k].seen) why = "reported twice";
            else {
                lines[k].seen = 1;
                if (lines[k].cases != o->cases) why = "case count differs from the reference";
                else if (lines[k].digest != o->emu) why = "emulator output differs from the recorded hardware output";
            }
        }
        printf("  %-18s %s%s%s\n", o->name, why ? "FAIL" : "ok", why ? " -- " : "", why ? why : "");
        if (why) bad++;
    }
    return bad;
}

int ref_finish(void) {
    if (mode == REF_RECORD) {
        if (record_failed) { printf("not writing %s: the recording failed\n", path); return 1; }
        FILE *f = fopen(path, "w");
        if (!f) { perror(path); return 1; }
        char brand[49];
        struct utsname u;
        cpu_brand(brand);
        uname(&u);
        fprintf(f, "# avxemu reference: %s\n", suite);
        fprintf(f, "# recorded on real silicon; only the lines that are not comments are compared\n");
        fprintf(f, "# cpu: %s\n# os: %s %s\n# cc: %s\n", brand, u.sysname, u.release, __VERSION__);
        for (int k = 0; k < nlines; k++)
            fprintf(f, "%s\t%lu\t%016llx\n", lines[k].name, lines[k].cases, (unsigned long long)lines[k].digest);
        if (fclose(f) != 0) { perror(path); remove(path); return 1; }
        printf("recorded %d op(s) to %s\n", nlines, path);
        return 0;
    }
    int bad = 0;
    for (int k = 0; k < nlines; k++)
        if (!lines[k].seen) { printf("  %-18s FAIL -- in the reference but no longer tested\n", lines[k].name); bad++; }
    return bad;
}

/* Plain name/cases/digest lines, for suites whose results are not per-op ref_op structs. */
int ref_put(const char *name, unsigned long cases, uint64_t digest) { return add_line(name, cases, digest); }
int ref_get(const char *name, unsigned long *cases, uint64_t *digest) {
    int k = find_line(name);
    if (k < 0) return 0;
    lines[k].seen = 1; *cases = lines[k].cases; *digest = lines[k].digest;
    return 1;
}
