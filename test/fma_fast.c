/*
 * fma_fast.c -- softfma's fast path (round to nearest, normal operands and result) against
 *   ref:     softfma.c built with SOFTFMA_NO_FAST, every FMA op and type, random MXCSR including
 *            unmasked exceptions: result bytes, return value and MXCSR must be identical. Any CPU.
 *   silicon: this CPU's vfmadd/vfmsub/vfnmadd/vfnmsub ss/sd under masked MXCSRs: result bits and
 *            raised flags must be identical. Needs FMA.
 * Inputs are structured (ties, cancellation, every alignment, the normal-range edges), not just
 * random bits. Fast-path hits are counted so a pass cannot come from the fallback alone.
 *   usage: fma-fast ref|silicon [cases (default 2000000)] [seed]
 */
#include "vexops.h"
#include "cpu.h"
#include "fma_fast_hw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long hits[8];
#define SOFTFMA_HIT(k) (hits[k]++)
#define fma_exec fast_fma_exec
#define fp_raise fast_fp_raise
#define half_to_float fast_half_to_float
#include "softfma.c"
#undef fma_exec
#undef fp_raise
#undef half_to_float

/* softfma.c without the fast path (built with SOFTFMA_NO_FAST) */
int fma_exec(vex_op op, int type, const ymm256 *a, const ymm256 *b, const ymm256 *c, ymm256 *out,
             uint32_t *mxcsr);

static uint64_t rng;
static uint64_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}
static int below(int n) { return (int)(rnd() % (uint64_t)n); }

typedef struct { int dbl, mb, bias, p; } fmt;
static const fmt FMT_D = {1, 52, 1023, 53}, FMT_S = {0, 23, 127, 24};

static uint64_t mk(const fmt *f, int sign, int be, uint64_t man) {
    return ((uint64_t)sign << (f->dbl ? 63 : 31)) | ((uint64_t)be << f->mb) | (man & ((1ull << f->mb) - 1));
}
static int bexp(const fmt *f, uint64_t v) { return (int)(v >> f->mb) & (f->dbl ? 0x7ff : 0xff); }
static uint64_t smask(const fmt *f) { return f->dbl ? 1ull << 63 : 1ull << 31; }

static uint64_t mant(const fmt *f) {
    uint64_t all = (1ull << f->mb) - 1;
    switch (below(8)) {
    case 0: { int k = below(8); return k ? (rnd() & all) >> (f->mb - k) << (f->mb - k) : 0; }   /* few bits: ties */
    case 1: return all;
    case 2: return all ^ (1ull << below(f->mb));
    case 3: return 1ull << below(f->mb);
    case 4: return f->dbl ? (1ull << 26) | 1 | (rnd() & 1 ? 1ull << 27 : 0) : (1ull << 12) | 1;
    default: return rnd() & all;
    }
}
static int clampe(const fmt *f, int be) { int max = f->dbl ? 2046 : 254; return be < 1 ? 1 : be > max ? max : be; }
static uint64_t normal(const fmt *f, int be) { return mk(f, (int)(rnd() & 1), clampe(f, be), mant(f)); }

/* x*y rounded to nearest by the host (the test runs in the default MXCSR) */
static uint64_t host_mul(const fmt *f, uint64_t x, uint64_t y) {
    if (f->dbl) { double a, b, r; memcpy(&a, &x, 8); memcpy(&b, &y, 8); r = a * b; uint64_t o; memcpy(&o, &r, 8); return o; }
    float a, b; uint32_t xs = (uint32_t)x, ys = (uint32_t)y, o;
    memcpy(&a, &xs, 4); memcpy(&b, &ys, 4);
    float r = (float)((double)a * (double)b);
    memcpy(&o, &r, 4);
    return o;
}
static uint64_t ulps(const fmt *f, uint64_t v, int k) {   /* k ulps away in magnitude, staying finite-ish */
    uint64_t s = v & smask(f), m = v & ~smask(f);
    if (k < 0 && m < (uint64_t)-k) return s;
    return s | (m + (uint64_t)(int64_t)k);
}
static uint64_t special(const fmt *f) {
    uint64_t all = (1ull << f->mb) - 1, e = f->dbl ? 0x7ff : 0xff;
    int s = (int)(rnd() & 1);
    switch (below(8)) {
    case 0: return mk(f, s, 0, 0);
    case 1: return mk(f, s, (int)e, 0);                              /* Inf */
    case 2: return mk(f, s, (int)e, (rnd() & all) | (1ull << (f->mb - 1)));        /* QNaN */
    case 3: return mk(f, s, (int)e, ((rnd() & all) >> 1) | 1);       /* SNaN */
    case 4: return mk(f, s, 0, 1);                                   /* subnormals */
    case 5: return mk(f, s, 0, all);
    default: return mk(f, s, 0, rnd() & all ? rnd() & all : 1);
    }
}

enum { G_RAND, G_NORM, G_CANCEL, G_ALIGN, G_EDGE, G_ZERO, G_SPECIAL, G_N };

/* One element's m1, m2, addend. */
static void gen(const fmt *f, int kind, uint64_t v[3]) {
    int b = f->bias, emax = 2 * b, near = b - 20 + below(40);
    switch (kind) {
    case G_RAND:
        for (int i = 0; i < 3; i++) v[i] = f->dbl ? rnd() : rnd() & 0xffffffffu;
        break;
    case G_NORM:
        v[0] = normal(f, near); v[1] = normal(f, b - 20 + below(40)); v[2] = normal(f, near + below(40) - 20);
        break;
    case G_CANCEL: {   /* z = -+RN(x*y) and k ulps around it */
        v[0] = normal(f, near); v[1] = normal(f, b - 10 + below(20));
        uint64_t p = host_mul(f, v[0], v[1]);
        v[2] = ulps(f, p, below(7) - 3) ^ (rnd() & 1 ? smask(f) : 0);
        if (below(4) == 0) v[2] = mk(f, (int)(rnd() & 1), clampe(f, bexp(f, p)), mant(f));
        break;
    }
    case G_ALIGN: {    /* every addend alignment against the product */
        v[0] = normal(f, near); v[1] = normal(f, b - 10 + below(20));
        int span = 2 * f->p + 10, ep = bexp(f, v[0]) + bexp(f, v[1]) - b;
        v[2] = normal(f, ep + below(2 * span + 1) - span);
        break;
    }
    case G_EDGE: {     /* results and products around the smallest normal and the largest finite */
        int top = below(2), t = top ? emax - 2 + below(4) : -2 + below(5);   /* biased target */
        int ex = b - 30 + below(60);
        v[0] = normal(f, ex); v[1] = normal(f, t - ex + b);
        uint64_t edge = top ? mk(f, 0, emax, (1ull << f->mb) - 1) : mk(f, 0, 1, 0);
        switch (below(5)) {
        case 0: v[2] = mk(f, (int)(rnd() & 1), 0, 0); break;
        case 1: v[2] = ulps(f, edge, below(9) - 4) ^ (rnd() & 1 ? smask(f) : 0); break;
        case 2: v[2] = ulps(f, host_mul(f, v[0], v[1]), below(5) - 2) ^ (rnd() & 1 ? smask(f) : 0); break;
        case 3: v[2] = normal(f, t + below(8) - 4); break;
        default: v[2] = normal(f, t - f->p - below(10)); break;
        }
        break;
    }
    case G_ZERO:       /* zeros of both signs in any role */
        v[0] = normal(f, near); v[1] = normal(f, near); v[2] = normal(f, near);
        for (int i = 0; i < 3; i++) if (below(2)) v[i] = mk(f, (int)(rnd() & 1), 0, 0);
        break;
    default:           /* at least one Inf, NaN or subnormal */
        v[0] = normal(f, near); v[1] = normal(f, near); v[2] = normal(f, near);
        int k = below(3);
        for (int i = 0; i < 3; i++) if (i != k && below(4) == 0) v[i] = special(f);
        do v[k] = special(f); while (!(v[k] & ~smask(f)));   /* not a zero */
        break;
    }
}

static const vex_op ops[18] = {
    VFMADD132, VFMADD213, VFMADD231, VFMSUB132, VFMSUB213, VFMSUB231,
    VFNMADD132, VFNMADD213, VFNMADD231, VFNMSUB132, VFNMSUB213, VFNMSUB231,
    VFMADDSUB132, VFMADDSUB213, VFMADDSUB231, VFMSUBADD132, VFMSUBADD213, VFMSUBADD231};

static void put(uint8_t *dst, int w, int lane, uint64_t v) { memcpy(dst + lane * w, &v, (size_t)w); }

/* Fill a, b, c so that op's (m1, m2, addend) in each lane come from gen(kind). */
static void fill(const fmt *f, int order, int type, int kind, ymm256 abc[3]) {
    int w = f->dbl ? 8 : 4, n = type <= FT_SD ? 1 : 32 / w;
    for (int i = 0; i < 3; i++) for (int k = 0; k < 4; k++) { uint64_t r = rnd(); memcpy(abc[i].b + 8 * k, &r, 8); }
    static const int role[3][3] = {{2, 1, 0}, {0, 2, 1}, {0, 1, 2}};   /* order -> index of m1, m2, addend */
    for (int lane = 0; lane < n; lane++) {
        uint64_t v[3];
        gen(f, kind < 0 ? below(G_N) : kind, v);
        for (int r = 0; r < 3; r++) put(abc[role[order][r]].b, w, lane, v[r]);
    }
}

static unsigned long long total_hits(void) {
    unsigned long long t = 0;
    for (int i = 0; i < 8; i++) t += hits[i];
    return t;
}

static int fails;
static void fail(const char *what, vex_op op, int type, uint32_t csr, const ymm256 abc[3]) {
    if (fails++ >= 10) return;
    printf("FAIL %s: op %d type %d mxcsr %#x\n", what, (int)op, type, csr);
    for (int i = 0; i < 3; i++) {
        printf("  %c:", "abc"[i]);
        for (int k = 3; k >= 0; k--) { uint64_t q; memcpy(&q, abc[i].b + 8 * k, 8); printf(" %016llx", (unsigned long long)q); }
        printf("\n");
    }
}

/* One op on both implementations; returns 1 if they agree. */
static int compare(vex_op op, int type, uint32_t csr, const ymm256 abc[3]) {
    ymm256 o1, o2;
    uint32_t m1 = csr, m2 = csr;
    memset(&o1, 0xCC, 32); memset(&o2, 0xCC, 32);
    int r1 = fast_fma_exec(op, type, &abc[0], &abc[1], &abc[2], &o1, &m1);
    int r2 = fma_exec(op, type, &abc[0], &abc[1], &abc[2], &o2, &m2);
    if (r1 != r2 || m1 != m2 || memcmp(&o1, &o2, 32)) { fail("fast != softfma", op, type, csr, abc); return 0; }
    return 1;
}

static uint32_t random_csr(void) {
    if (below(10) < 7) return 0x1F80u | (uint32_t)(rnd() & 0x3F);   /* default, any sticky flags */
    return (uint32_t)(rnd() & 0xFFFF);
}

static int run_ref(long n) {
    /* 1: everything, random MXCSRs */
    for (long i = 0; i < n; i++) {
        int oi = below(18), type = oi >= 12 ? FT_PS + below(2) : below(4);
        const fmt *f = type == FT_SD || type == FT_PD ? &FMT_D : &FMT_S;
        ymm256 abc[3];
        fill(f, oi % 3, type, -1, abc);
        compare(ops[oi], type, random_csr(), abc);
    }
    printf("ref: %ld random cases, fast hits by category:", n);
    for (int i = 0; i < 8; i++) printf(" %llu", hits[i]);
    printf("\n");
    for (int i = 0; i < 8; i++)
        if (!hits[i]) { printf("FAIL: fast-path category %d never taken\n", i); fails++; }
    /* 2: normal-range sets in round to nearest must almost always take the fast path */
    memset(hits, 0, sizeof hits);
    long m = n / 4, lanes = 0;
    for (long i = 0; i < m; i++) {
        int oi = below(12), type = below(4), kind = G_NORM + below(3);
        const fmt *f = type == FT_SD || type == FT_PD ? &FMT_D : &FMT_S;
        ymm256 abc[3];
        fill(f, oi % 3, type, kind, abc);
        lanes += type <= FT_SD ? 1 : f->dbl ? 4 : 8;
        compare(ops[oi], type, 0x1F80u | (uint32_t)(rnd() & 0x3F), abc);
    }
    printf("ref: %ld normal-range cases, %llu of %ld elements fast\n", m, total_hits(), lanes);
    if (total_hits() < (unsigned long long)lanes * 9 / 10) { printf("FAIL: fast path rarely taken\n"); fails++; }
    /* 3: never fast with a special operand, or outside round to nearest */
    memset(hits, 0, sizeof hits);
    for (long i = 0; i < m; i++) {
        int oi = below(18), type = oi >= 12 ? FT_PS + below(2) : below(4), sp = below(2);
        const fmt *f = type == FT_SD || type == FT_PD ? &FMT_D : &FMT_S;
        ymm256 abc[3];
        fill(f, oi % 3, type, sp ? G_SPECIAL : -1, abc);
        uint32_t csr = random_csr();
        if (!sp && !(csr & 0x6000)) csr |= (uint32_t)(1 + below(3)) << 13;
        compare(ops[oi], type, csr, abc);
    }
    printf("ref: %ld special or directed-rounding cases, %llu fast\n", m, total_hits());
    if (total_hits()) { printf("FAIL: fast path taken where it must not be\n"); fails++; }
    return fails;
}

static int run_silicon(long n) {
    if (!(cpu_features() & CPU_FMA)) { printf("SKIP: no FMA on this CPU\n"); return 77; }
    memset(hits, 0, sizeof hits);
    long fast_cases = 0;
    for (long i = 0; i < n; i++) {
        int dbl = below(2), variant = below(4);
        const fmt *f = dbl ? &FMT_D : &FMT_S;
        uint64_t v[3];
        gen(f, below(G_N), v);
        uint32_t csr = below(10) < 8 ? 0x1F80u : 0x1F80u | (uint32_t)(rnd() & 0xE040);   /* RC, FTZ, DAZ */
        uint32_t hwfl, mx = csr;
        uint64_t hw = dbl ? fma_fast_hw_d(variant, v[0], v[1], v[2], csr, &hwfl)
                          : fma_fast_hw_s(variant, (uint32_t)v[0], (uint32_t)v[1], (uint32_t)v[2], csr, &hwfl);
        ymm256 abc[3], out;
        memset(abc, 0, sizeof abc);
        for (int r = 0; r < 3; r++) put(abc[r].b, dbl ? 8 : 4, 0, v[r]);   /* 231: m1=a m2=b addend=c */
        unsigned long long before = total_hits();
        int ret = fast_fma_exec(ops[3 * variant + 2], dbl ? FT_SD : FT_SS, &abc[0], &abc[1], &abc[2], &out, &mx);
        fast_cases += total_hits() != before;
        uint64_t got = 0;
        memcpy(&got, out.b, dbl ? 8 : 4);
        if (ret != 1 || got != hw || (mx & 0x3F) != hwfl) {
            if (fails < 10)
                printf("  cpu %016llx flags %#x, emu %016llx flags %#x ret %d\n", (unsigned long long)hw, hwfl,
                       (unsigned long long)got, mx & 0x3F, ret);
            fail("emulator != cpu", ops[3 * variant + 2], dbl ? FT_SD : FT_SS, csr, abc);
        }
    }
    printf("silicon: %ld cases, %ld through the fast path\n", n, fast_cases);
    if (fast_cases < n / 4) { printf("FAIL: fast path rarely taken\n"); fails++; }
    return fails;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: fma-fast ref|silicon [cases] [seed]\n"); return 2; }
    long n = argc > 2 ? atol(argv[2]) : 2000000;
    rng = argc > 3 ? strtoull(argv[3], NULL, 0) : 0x9e3779b97f4a7c15ull;
    if (!rng) rng = 1;
    int r = !strcmp(argv[1], "silicon") ? run_silicon(n) : run_ref(n);
    if (r == 77) return 77;
    printf("%s: %d failures\n", argv[1], fails);
    return fails ? 1 : 0;
}
