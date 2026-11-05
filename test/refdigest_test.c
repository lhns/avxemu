#include "refdigest.h"
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)

static char dir[512];
static char path_a[600], path_b[600];

static void write_file(const char *p, const char *text) {
    FILE *f = fopen(p, "w"); fputs(text, f); fclose(f);
}

static void two_cases(struct ref_op *o, const char *hw, const char *emu, int live_ok) {
    for (int i = 0; i < 2; i++) {
        if (hw) ref_hw(o, hw, strlen(hw));
        ref_emu(o, emu, strlen(emu));
        ref_case(o, live_ok);
    }
}

static int record_x(const char *path, const char *hw, const char *emu, int live_ok) {
    struct ref_op o;
    if (ref_begin(REF_RECORD, "t", path) != 0) return -1;
    ref_op_init(&o, "x");
    two_cases(&o, hw, emu, live_ok);
    return ref_report(&o, 1) + ref_finish();
}

static int check_ops(const char *path, const char *const *names, int n, const char *emu, int cases) {
    struct ref_op o[4];
    if (ref_begin(REF_CHECK, "t", path) != 0) return -1;
    for (int i = 0; i < n; i++) {
        ref_op_init(&o[i], names[i]);
        for (int c = 0; c < cases; c++) { ref_emu(&o[i], emu, strlen(emu)); ref_case(&o[i], 1); }
    }
    return ref_report(o, n) + ref_finish();
}

static int child_status(const char *features) {
    pid_t p = fork();
    if (p == 0) {
        char *argv[] = {"refdigest_test", "record", path_b, NULL};
        if (features) setenv("AVXEMU_TEST_FEATURES", features, 1);
        ref_init(3, argv, "t", CPU_AVX2);
        ref_finish();
        _exit(0);
    }
    int st = 0; waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/refdigest-test.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    snprintf(path_a, sizeof path_a, "%s/a.ref", dir);
    snprintf(path_b, sizeof path_b, "%s/b.ref", dir);
    const char *x[] = {"x"}, *xy[] = {"x", "y"};

    CHECK(ref_fnv1a(REF_FNV_OFFSET, "", 0) == 0xcbf29ce484222325ull, "fnv1a of empty");
    CHECK(ref_fnv1a(REF_FNV_OFFSET, "a", 1) == 0xaf63dc4c8601ec8cull, "fnv1a of a");
    CHECK(ref_fnv1a(REF_FNV_OFFSET, "foobar", 6) == 0x85944171f73967e8ull, "fnv1a of foobar");

    CHECK(record_x(path_a, "ab", "ab", 1) == 0, "record writes a reference");
    CHECK(access(path_a, R_OK) == 0, "the reference exists");
    CHECK(check_ops(path_a, x, 1, "ab", 2) == 0, "replay of the same outputs passes");
    CHECK(check_ops(path_a, x, 1, "ac", 2) == 1, "different emulator output fails");
    CHECK(check_ops(path_a, x, 1, "ab", 1) == 1, "a different case count fails");
    CHECK(check_ops(path_a, xy, 2, "ab", 2) == 1, "an op missing from the reference fails");

    CHECK(record_x(path_b, "ab", "ab", 1) == 0, "record a second reference");
    { FILE *f = fopen(path_b, "a"); fputs("z\t1\t0000000000000000\n", f); fclose(f); }
    CHECK(check_ops(path_b, x, 1, "ab", 2) == 1, "a reference op no longer tested fails");

    write_file(path_b, "# avxemu reference: other\nx\t2\t0000000000000000\n");
    CHECK(ref_begin(REF_CHECK, "t", path_b) == -1, "a reference for another suite is refused");
    write_file(path_b, "# avxemu reference: t\nx\ttwo\tzz\n");
    CHECK(ref_begin(REF_CHECK, "t", path_b) == -1, "a malformed line is refused");
    write_file(path_b, "# avxemu reference: t\nx\t2\tab\n");
    CHECK(ref_begin(REF_CHECK, "t", path_b) == -1, "a short digest is refused");
    write_file(path_b, "# avxemu reference: t\nx\t2\t0000000000000000\nx\t2\t0000000000000000\n");
    CHECK(ref_begin(REF_CHECK, "t", path_b) == -1, "a duplicated op is refused");
    write_file(path_b, "");
    CHECK(ref_begin(REF_CHECK, "t", path_b) == -1, "an empty reference is refused");
    unlink(path_b);
    CHECK(ref_begin(REF_CHECK, "t", path_b) == -1, "a missing reference is refused");

    CHECK(record_x(path_b, "ab", "ac", 0) >= 1, "a live mismatch fails the recording");
    CHECK(access(path_b, F_OK) != 0, "a failed recording writes no reference");
    CHECK(record_x(path_b, "ab", "ac", 1) >= 1, "digests that disagree where live agreed fail");
    CHECK(access(path_b, F_OK) != 0, "and write no reference");

    fflush(stdout);
    CHECK(child_status("0") == 77, "record refuses a CPU without the needed feature");
    CHECK(access(path_b, F_OK) != 0, "a refused recording writes no reference");

    unlink(path_a); rmdir(dir);
    if (fails) { printf("refdigest: %d failure(s)\n", fails); return 1; }
    printf("refdigest: contract holds\n");
    return 0;
}
