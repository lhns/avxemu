/*
 * fma_fast.c -- softfma's fast paths (integer path and hardware-arithmetic kernel; round to nearest,
 * normal operands and result) against
 *   ref:     softfma.c built with SOFTFMA_NO_FAST, every FMA op and type, random MXCSR including
 *            unmasked exceptions: result bytes, return value and MXCSR must be identical. Any CPU.
 *            First with the kernel off (the integer path alone), then with it on.
 *   silicon: this CPU's FMA, scalar and packed, ps/pd xmm/ymm, all 18 ops, under masked MXCSRs: result
 *            bits and raised flags must be identical. Needs FMA.
 * Inputs are structured (ties, midpoints, cancellation, powers of two, every alignment, the range
 * edges), not just random bits. Hits are counted per path and per fallback reason so a pass cannot
 * come from the fallback alone. The live MXCSR is set before and compared after every call: the
 * kernel runs native arithmetic and must leave it as it found it.
 *   usage: fma-fast ref|silicon [cases (default 2000000)] [seed]
 */
#include "vexops.h"
#include "cpu.h"
#include "fma_fast_hw.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* softfma.c: 0-7 integer path taken, 8-16 its boundary events; 17/18 kernel taken (d/s), 19 operand
 * out of range, 20 H guard (d), 21 float midpoint, 22 zero result, 23 float result out of range,
 * 24 live MXCSR declined the kernel, 25 vector with kernel and fallback lanes, 26 all-zero lanes (no arithmetic),
 * 27 kernel ran. The kernel is taken for a vector only if it proves every lane: 17, 18, 26 count such lanes. */
enum { NHIT = 28, H_KD = 17, H_KS = 18, H_LIVE = 24, H_RAN = 27 };
static unsigned long long hits[NHIT];
static int hw_on;                       /* the kernel is switched off to test the integer path alone */
#define SOFTFMA_HIT(k) (hits[k]++)
#define SOFTFMA_HW_ON() (hw_on)
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

/* A mantissa field with `bits` significant bits (the implicit one included), the low ones zero. */
static uint64_t fracbits(const fmt *f, int bits) {
    if (bits <= 1) return 0;
    return (rnd() & ((1ull << (bits - 1)) - 1)) << (f->mb - (bits - 1));
}
static uint64_t flip(const fmt *f, uint64_t v, int s) { return v ^ (s ? smask(f) : 0); }
static double tod(const fmt *f, uint64_t v) {
    if (f->dbl) { double d; memcpy(&d, &v, 8); return d; }
    float x; uint32_t u = (uint32_t)v; memcpy(&x, &u, 4); return x;
}
static uint64_t fromd(const fmt *f, double d) {
    if (f->dbl) { uint64_t v; memcpy(&v, &d, 8); return v; }
    float x = (float)d; uint32_t u; memcpy(&u, &x, 4); return u;
}
/* the gap below / above a positive normal v, as a biased exponent of the power of two (half of it when `half`) */
static int gapexp(const fmt *f, uint64_t v, int below_) {
    return bexp(f, v) - f->p + 1 - (below_ && !(v & ((1ull << f->mb) - 1)));
}

enum { G_RAND, G_NORM, G_CANCEL, G_ALIGN, G_EDGE, G_ZERO, G_TIE, G_SPECIAL,
       G_MID, G_CANC, G_POW2, G_RANGE, G_EXACT, G_N };

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
    case G_TIE: {      /* x*y = +-2^k (1 + 0, +2^-j or -2^-j), 2^k half an ulp of z: exact ties and just off */
        if (below(4) == 0) {
            /* Just above a tie through a borrow: x*y = 2^k (2 - 2^(2-2p)) has 2p-2 ones; aligned with z of
             * the other sign, the ones below z's ulp read as half an ulp after the sticky borrow. */
            int sx = (int)(rnd() & 1), sy = (int)(rnd() & 1), ex = b + below(21) - 10, ey = b + below(21) - 10;
            v[0] = mk(f, sx, ex, (1ull << f->mb) - 2);
            v[1] = mk(f, sy, ey, 1);
            uint64_t m = mant(f);
            v[2] = mk(f, !(sx ^ sy), ex + ey - b + f->p + 1, m);
            break;
        }
        v[2] = normal(f, near);
        int a = below(21) - 10, j = 1 + below(f->mb), kb = bexp(f, v[2]) - f->p;
        v[0] = mk(f, (int)(rnd() & 1), b + a, below(2) ? 1ull << (f->mb - j) : 0);
        switch (below(3)) {
        case 0: v[1] = mk(f, (int)(rnd() & 1), kb - a, 0); break;
        case 1: v[1] = mk(f, (int)(rnd() & 1), kb - a, 1ull << below(f->mb)); break;
        default: v[1] = mk(f, (int)(rnd() & 1), kb - a - 1, ((1ull << j) - 1) << (f->mb - j)); break;
        }
        break;
    }
    case G_ZERO:       /* zeros of both signs in any role */
        v[0] = normal(f, near); v[1] = normal(f, near); v[2] = normal(f, near);
        for (int i = 0; i < 3; i++) if (below(2)) v[i] = mk(f, (int)(rnd() & 1), 0, 0);
        break;
    case G_MID: {      /* x at, or a few units of 2^-2p from, a midpoint of the result's grid, from both sides */
        int p = f->p, sa = (int)(rnd() & 1), sb = (int)(rnd() & 1), dn = (int)(rnd() & 1);
        int e1 = b - 10 + below(21), e2 = b - 10 + below(21);
        uint64_t x, y, c;
        switch (below(3)) {
        case 0: {      /* few-bit x, y: x*y = T is exact; c = half the gap above or below T: an exact tie */
            x = mk(f, 0, e1, fracbits(f, 1 + below(p / 2)));
            y = mk(f, 0, e2, fracbits(f, 1 + below(p / 2)));
            uint64_t T = host_mul(f, x, y);
            c = mk(f, dn, gapexp(f, T, dn) - 1, 0);          /* half of the gap */
            if (below(2)) c = ulps(f, c, below(5) - 2);      /* a few ulps of c off the tie */
            break;
        }
        case 1: {      /* x*y within about 2^-2p of T plus or minus half a gap: c = RN(gap/2 - (x*y - T)) */
            uint64_t T = normal(f, b - 10 + below(21)) & ~smask(f);
            y = mk(f, 0, e2, mant(f));
            x = fromd(f, tod(f, T) / tod(f, y));
            double delta = f->dbl ? fma(tod(f, x), tod(f, y), -tod(f, T)) : tod(f, x) * tod(f, y) - tod(f, T);
            double h = ldexp(1.0, gapexp(f, T, dn) - 1 - f->bias);
            c = fromd(f, (dn ? -h : h) - delta);
            if (below(2)) c = ulps(f, c, below(7) - 3);
            c = flip(f, c, 0);
            break;
        }
        default: {     /* x*y an odd integer of p+1 bits: itself a midpoint; c a tiny addend of either sign */
            int qa = (p + 2) / 2, qb = p + 1 - qa;
            uint64_t A, B;
            do {
                A = (1ull << (qa - 1)) | (rnd() & ((1ull << (qa - 1)) - 1)) | 1;
                B = (1ull << (qb - 1)) | (rnd() & ((1ull << (qb - 1)) - 1)) | 1;
            } while (!((A * B) >> p));
            x = mk(f, 0, e1, (A << (f->mb - (qa - 1))) & ((1ull << f->mb) - 1));
            y = mk(f, 0, e2, (B << (f->mb - (qb - 1))) & ((1ull << f->mb) - 1));
            int j = 1 + below(2 * p + 10);
            c = mk(f, dn, clampe(f, bexp(f, host_mul(f, x, y)) - j), mant(f));
            break;
        }
        }
        v[0] = flip(f, x, sa); v[1] = flip(f, y, sb); v[2] = flip(f, c, sa ^ sb);
        break;
    }
    case G_CANC: {     /* c = -RN(x*y) with its low bits cleared, x and y of 1..p bits: 1..2p bits survive */
        int sa = (int)(rnd() & 1), sb = (int)(rnd() & 1);
        uint64_t x = mk(f, 0, b - 10 + below(21), fracbits(f, 1 + below(f->p)));
        uint64_t y = mk(f, 0, b - 10 + below(21), fracbits(f, 1 + below(f->p)));
        uint64_t c = host_mul(f, x, y) ^ smask(f);
        c &= ~((1ull << below(f->mb + 1)) - 1);
        if (below(3) == 0) c = ulps(f, c, below(5) - 2);
        v[0] = flip(f, x, sa); v[1] = flip(f, y, sb); v[2] = flip(f, c, sa ^ sb);
        break;
    }
    case G_POW2: {     /* (1+2^-s)(1-2^-s) + 2^-2s = 1: results at a power of two and a few ulps either side */
        int s = 1 + below(f->p / 2 - 1), sa = (int)(rnd() & 1), sb = (int)(rnd() & 1);
        int da = below(41) - 20, db = below(41) - 20;
        uint64_t x = mk(f, 0, b + da, 1ull << (f->mb - s));
        uint64_t y = mk(f, 0, b - 1 + db, s > 1 ? (((1ull << (s - 1)) - 1) << (f->mb - (s - 1))) : 0);
        uint64_t c = mk(f, 0, b - 2 * s + da + db, 0);
        if (below(4)) c = ulps(f, c, below(7) - 3);
        v[0] = flip(f, x, sa); v[1] = flip(f, y, sb); v[2] = flip(f, c, sa ^ sb);
        break;
    }
    case G_RANGE: {    /* operands just inside and just outside the kernel's ranges */
        static const int dxy[4] = {400, 401, -400, -401}, dz[4] = {800, 801, -800, -801}, fe[6] = {0, 1, 2, 253, 254, 255};
        for (int i = 0; i < 3; i++) {
            int be = f->dbl ? b + (i < 2 ? dxy : dz)[below(4)] : fe[below(6)];
            if (!f->dbl || below(4)) v[i] = mk(f, (int)(rnd() & 1), be, below(3) ? mant(f) : below(2) ? 0 : (1ull << f->mb) - 1);
            else v[i] = normal(f, b - 30 + below(60));
        }
        if (below(4) == 0) {   /* c = -RN(x*y) when it fits, so the range edge meets a cancellation */
            uint64_t pr = host_mul(f, v[0], v[1]);
            if (bexp(f, pr) > 1 && bexp(f, pr) < 2 * b) v[2] = pr ^ smask(f);
        }
        break;
    }
    case G_EXACT: {    /* small integers: x*y + c exact, so no PE; sometimes exactly zero */
        int pb = f->dbl ? 20 : 11, sh = below(61) - 30;
        double x = (double)(1 + (int)(rnd() % (1u << pb))), y = (double)(1 + (int)(rnd() % (1u << pb)));
        double c = (double)(int64_t)(rnd() % (1ull << (f->dbl ? 40 : 22)));
        if (below(4) == 0) c = x * y;
        if (rnd() & 1) x = -x;
        if (rnd() & 1) y = -y;
        if (rnd() & 1) c = -c;
        if (below(4) == 0) c = -x * y;
        v[0] = fromd(f, ldexp(x, sh)); v[1] = fromd(f, y); v[2] = fromd(f, ldexp(c, sh));
        break;
    }
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

static int fails;

static unsigned long long int_hits(void) {      /* the integer path's taken counters */
    unsigned long long t = 0;
    for (int i = 0; i < 8; i++) t += hits[i];
    return t;
}
static unsigned long long kernel_hits(void) { return hits[H_KD] + hits[H_KS]; }

static void fail(const char *what, vex_op op, int type, uint32_t csr, const ymm256 abc[3]) {
    if (fails++ >= 10) return;
    printf("FAIL %s: op %d type %d mxcsr %#x\n", what, (int)op, type, csr);
    for (int i = 0; i < 3; i++) {
        printf("  %c:", "abc"[i]);
        for (int k = 3; k >= 0; k--) { uint64_t q; memcpy(&q, abc[i].b + 8 * k, 8); printf(" %016llx", (unsigned long long)q); }
        printf("\n");
    }
}

/* The live MXCSR: the kernel runs native arithmetic and must leave it exactly as it found it. */
static uint32_t live_get(void) { uint32_t v; __asm__ volatile("stmxcsr %0" : "=m"(v) : : "memory"); return v; }
static void live_set(uint32_t v) { __asm__ volatile("ldmxcsr %0" : : "m"(v) : "memory"); }
#define LIVE_DEFAULT 0x1F80u

/* Flags clear or set, FTZ/DAZ on or off, all masked and nearest: the kernel may run. Else (2 in 10): an unmasked
 * exception or a rounding mode: it must not, or its first native operation traps or rounds wrongly. */
static uint32_t pick_live(int *may_run) {
    uint32_t v = LIVE_DEFAULT;
    *may_run = 1;
    switch (below(10)) {
    case 0: case 1: v |= (uint32_t)(rnd() & 0x3F); break;
    case 2: case 3: v |= (uint32_t)(rnd() & 0x3F) | (uint32_t)(rnd() & 0x8040); break;
    case 4: v |= (uint32_t)(rnd() & 0x8040); break;
    case 5: v &= ~(1u << (7 + below(6))); *may_run = 0; break;
    case 6: v |= (uint32_t)(1 + below(3)) << 13; *may_run = 0; break;
    default: break;
    }
    return v;
}

static unsigned long long kernel_ran, kernel_calls, kernel_exact_calls, live_calls, live_declined;

/* fast_fma_exec under a chosen live MXCSR, which must be unchanged afterwards. */
static int call_fast(vex_op op, int type, const ymm256 abc[3], ymm256 *out, uint32_t *mx) {
    int may_run;
    uint32_t live = pick_live(&may_run), in = *mx, after;
    unsigned long long k0 = kernel_hits(), r0 = hits[H_RAN], l0 = hits[H_LIVE];
    live_set(live);
    int r = fast_fma_exec(op, type, &abc[0], &abc[1], &abc[2], out, mx);
    after = live_get();
    live_set(LIVE_DEFAULT);
    live_calls++;
    if (after != live) { printf("  live MXCSR %#x -> %#x\n", live, after); fail("live MXCSR changed", op, type, in, abc); }
    if (hits[H_RAN] != r0) {
        kernel_ran++;
        if (!may_run) fail("kernel ran under a live MXCSR that forbids it", op, type, in, abc);
    }
    if (kernel_hits() != k0) {
        kernel_calls++;
        if (!(in & 0x20) && !(*mx & 0x20)) kernel_exact_calls++;
    }
    if (hits[H_LIVE] != l0) live_declined++;
    return r;
}

/* One op on both implementations; returns 1 if they agree. */
static int compare(vex_op op, int type, uint32_t csr, const ymm256 abc[3]) {
    ymm256 o1, o2;
    uint32_t m1 = csr, m2 = csr;
    memset(&o1, 0xCC, 32); memset(&o2, 0xCC, 32);
    int r1 = call_fast(op, type, abc, &o1, &m1);
    int r2 = fma_exec(op, type, &abc[0], &abc[1], &abc[2], &o2, &m2);
    if (r1 != r2 || m1 != m2 || memcmp(&o1, &o2, 32)) { fail("fast != softfma", op, type, csr, abc); return 0; }
    return 1;
}

static uint32_t random_csr(void) {
    if (below(10) < 7) return 0x1F80u | (uint32_t)(rnd() & 0x3F);   /* default, any sticky flags */
    return (uint32_t)(rnd() & 0xFFFF);
}

/* masked, nearest, any sticky flags, FTZ and DAZ either way: the kernel's domain */
static uint32_t kernel_csr(void) { return 0x1F80u | (uint32_t)(rnd() & 0x3F) | (uint32_t)(rnd() & 0x8040); }

static const char *const hit_name[NHIT] = {"d:zero-product", "d:zero-addend", "d:cancel", "d:sum",
    "s:zero-product", "s:zero-addend", "s:cancel", "s:sum", "declined-tiny", "tie-up", "tie-even",
    "half+sticky", "carry", "declined-overflow", "result-min-normal", "result-max-binade", "sub-sticky",
    "kernel-d", "kernel-s", "range", "H-guard", "float-midpoint", "zero-result", "float-result-range",
    "live-declined", "mixed-vector", "zero-lanes", "kernel-ran"};

static void report(const char *mode, int lo, int hi) {
    printf("%s: events:", mode);
    for (int i = lo; i <= hi; i++) printf(" %s=%llu", hit_name[i], hits[i]);
    printf("\n");
    for (int i = lo; i <= hi; i++)
        if (!hits[i]) { printf("FAIL: event %s never seen\n", hit_name[i]); fails++; }
}

static void report_live(const char *mode, int need_declined) {
    printf("%s: %llu calls with a chosen live MXCSR, the kernel ran in %llu and was taken in %llu (%llu of them with no PE), "
           "%llu declined by the live MXCSR\n", mode, live_calls, kernel_ran, kernel_calls, kernel_exact_calls, live_declined);
    if (!kernel_calls || !kernel_exact_calls || (need_declined && !live_declined)) {
        printf("FAIL: the live MXCSR checks did not cover the kernel\n"); fails++;
    }
}

static void lane_counts(int type, const fmt *f, long *lanes) { *lanes += type <= FT_SD ? 1 : f->dbl ? 4 : 8; }

/* The integer path alone (the kernel off): the phases of lhns/avxemu#11. */
static void run_ref_int(long n) {
    hw_on = 0;
    for (long i = 0; i < n; i++) {
        int oi = below(18), type = oi >= 12 ? FT_PS + below(2) : below(4);
        const fmt *f = type == FT_SD || type == FT_PD ? &FMT_D : &FMT_S;
        ymm256 abc[3];
        fill(f, oi % 3, type, -1, abc);
        compare(ops[oi], type, random_csr(), abc);
    }
    printf("ref (integer path): %ld random cases\n", n);
    report("ref (integer path)", 0, 16);
    /* normal-range sets in round to nearest must almost always take the fast path */
    memset(hits, 0, sizeof hits);
    long m = n / 4, lanes = 0;
    for (long i = 0; i < m; i++) {
        int oi = below(12), type = below(4), kind = G_NORM + below(3);
        const fmt *f = type == FT_SD || type == FT_PD ? &FMT_D : &FMT_S;
        ymm256 abc[3];
        fill(f, oi % 3, type, kind, abc);
        lane_counts(type, f, &lanes);
        compare(ops[oi], type, 0x1F80u | (uint32_t)(rnd() & 0x3F), abc);
    }
    printf("ref (integer path): %ld normal-range cases, %llu of %ld elements fast\n", m, int_hits(), lanes);
    if (int_hits() < (unsigned long long)lanes * 9 / 10) { printf("FAIL: fast path rarely taken\n"); fails++; }
    /* never fast with a special operand, or outside round to nearest */
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
    printf("ref (integer path): %ld special or directed-rounding cases, %llu fast\n", m, int_hits());
    if (int_hits()) { printf("FAIL: fast path taken where it must not be\n"); fails++; }
}

/* The kernel on, ahead of the integer path. */
static void run_ref_hw(long n) {
    hw_on = 1;
    memset(hits, 0, sizeof hits);
    kernel_ran = kernel_calls = kernel_exact_calls = live_calls = live_declined = 0;
    for (long i = 0; i < n; i++) {
        int oi = below(18), type = oi >= 12 ? FT_PS + below(2) : below(4);
        const fmt *f = type == FT_SD || type == FT_PD ? &FMT_D : &FMT_S;
        ymm256 abc[3];
        fill(f, oi % 3, type, -1, abc);
        compare(ops[oi], type, below(2) ? random_csr() : kernel_csr(), abc);
    }
    printf("ref (kernel): %ld random cases\n", n);
    /* kind by kind, scalar and packed: what the kernel proves of each */
    static const char *const kname[G_N] = {"random", "normal", "cancel", "align", "edge", "zero", "tie", "special",
        "midpoint", "cancel-bits", "power-of-two", "range-edge", "exact"};
    static const int kinds[] = {G_NORM, G_CANCEL, G_ALIGN, G_EDGE, G_TIE, G_MID, G_CANC, G_POW2, G_RANGE, G_EXACT};
    for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        unsigned long long k0 = kernel_hits();
        long lanes = 0, m = n / 10;
        for (long i = 0; i < m; i++) {
            int oi = below(18), type = oi >= 12 ? FT_PS + below(2) : below(4);
            const fmt *f = type == FT_SD || type == FT_PD ? &FMT_D : &FMT_S;
            ymm256 abc[3];
            fill(f, oi % 3, type, kinds[k], abc);
            unsigned long long l0 = hits[H_LIVE];
            compare(ops[oi], type, kernel_csr(), abc);
            if (type > FT_SD && hits[H_LIVE] == l0) lane_counts(type, f, &lanes);   /* packed, and not kept off the kernel by the live MXCSR */
        }
        printf("ref (kernel): %-12s %llu of %ld packed lanes proven\n", kname[kinds[k]], kernel_hits() - k0, lanes);
        if (kinds[k] == G_NORM && (kernel_hits() - k0) < (unsigned long long)lanes * 9 / 10) {
            printf("FAIL: the kernel rarely takes normal-range lanes\n"); fails++;
        }
    }
    /* mixed vectors: every lane its own kind */
    for (long i = 0; i < n / 4; i++) {
        int oi = below(18), type = FT_PS + below(2);
        const fmt *f = type == FT_PD ? &FMT_D : &FMT_S;
        ymm256 abc[3];
        fill(f, oi % 3, type, -1, abc);
        compare(ops[oi], type, kernel_csr(), abc);
    }
    report("ref (kernel)", H_KD, 27);
    report_live("ref (kernel)", 1);
    /* never the kernel (nor the integer path) with a special operand, or outside round to nearest */
    memset(hits, 0, sizeof hits);
    long m = n / 4;
    for (long i = 0; i < m; i++) {
        int oi = below(18), type = oi >= 12 ? FT_PS + below(2) : below(4), sp = below(2);
        const fmt *f = type == FT_SD || type == FT_PD ? &FMT_D : &FMT_S;
        ymm256 abc[3];
        fill(f, oi % 3, type, sp ? G_SPECIAL : -1, abc);
        uint32_t csr = random_csr();
        if (!sp && !(csr & 0x6000)) csr |= (uint32_t)(1 + below(3)) << 13;
        compare(ops[oi], type, csr, abc);
    }
    printf("ref (kernel): %ld special or directed-rounding cases, %llu integer path, %llu kernel\n", m, int_hits(), kernel_hits());
    if (int_hits() || kernel_hits()) { printf("FAIL: fast path taken where it must not be\n"); fails++; }
    /* an unmasked exception in the incoming MXCSR: never the kernel */
    memset(hits, 0, sizeof hits);
    for (long i = 0; i < m; i++) {
        int oi = below(18), type = oi >= 12 ? FT_PS + below(2) : below(4);
        const fmt *f = type == FT_SD || type == FT_PD ? &FMT_D : &FMT_S;
        ymm256 abc[3];
        fill(f, oi % 3, type, G_NORM + below(3), abc);
        compare(ops[oi], type, 0x1F80u ^ (1u << (7 + below(6))), abc);
    }
    printf("ref (kernel): %ld cases with an exception unmasked, kernel ran %llu times\n", m, hits[H_RAN]);
    if (hits[H_RAN]) { printf("FAIL: the kernel ran with an exception unmasked\n"); fails++; }
}

static int run_ref(long n) {
    run_ref_int(n);
    run_ref_hw(n);
    return fails;
}

/* Scalar vfmadd/vfmsub/vfnmadd/vfnmsub (231) on this CPU against the emulator; *taken counts the cases for which path() grew. */
static void silicon_scalar(long n, unsigned long long (*path)(void), long *taken) {
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
        unsigned long long before = path();
        int ret = call_fast(ops[3 * variant + 2], dbl ? FT_SD : FT_SS, abc, &out, &mx);
        *taken += path() != before;
        uint64_t got = 0;
        memcpy(&got, out.b, dbl ? 8 : 4);
        if (ret != 1 || got != hw || (mx & 0x3F) != hwfl) {
            if (fails < 10)
                printf("  %s: cpu %016llx flags %#x, emu %016llx flags %#x ret %d\n",
                       path() != before ? "FAST PATH" : "general code", (unsigned long long)hw, hwfl,
                       (unsigned long long)got, mx & 0x3F, ret);
            fail("emulator != cpu", ops[3 * variant + 2], dbl ? FT_SD : FT_SS, csr, abc);
        }
    }
}

/* All 18 ops on ps and pd, xmm and ymm, every lane its own kind; returns the cases that took the kernel. */
static long silicon_packed(long n) {
    long taken = 0;
    for (long i = 0; i < n; i++) {
        int oi = below(18), dbl = below(2), ymm = below(2);
        const fmt *f = dbl ? &FMT_D : &FMT_S;
        int type = dbl ? FT_PD : FT_PS;
        ymm256 abc[3], out, hwout;
        fill(f, oi % 3, type, -1, abc);
        if (!ymm) for (int r = 0; r < 3; r++) memset(abc[r].b + 16, 0, 16);   /* a 128-bit form: the caller zeroes the upper lanes */
        uint32_t csr = below(10) < 8 ? 0x1F80u : 0x1F80u | (uint32_t)(rnd() & 0xE040);
        uint32_t hwfl, mx = csr;
        fma_fast_hw_p(oi, dbl, ymm, &abc[0], &abc[1], &abc[2], &hwout, csr, &hwfl);
        memset(&out, 0, 32);
        unsigned long long before = kernel_hits();
        int ret = call_fast(ops[oi], type, abc, &out, &mx);
        taken += kernel_hits() != before;
        size_t cmp = ymm ? 32 : 16;
        if (ret != 1 || memcmp(&out, &hwout, cmp) || (mx & 0x3F) != hwfl) {
            if (fails < 10) {
                printf("  packed %s %s op %d: flags cpu %#x emu %#x ret %d\n", dbl ? "pd" : "ps", ymm ? "ymm" : "xmm", oi,
                       hwfl, mx & 0x3F, ret);
                for (size_t k = 0; k < cmp; k += 8) {
                    uint64_t x, y;
                    memcpy(&x, hwout.b + k, 8); memcpy(&y, out.b + k, 8);
                    if (x != y) printf("    bytes %zu: cpu %016llx emu %016llx\n", k, (unsigned long long)x, (unsigned long long)y);
                }
            }
            fail("emulator != cpu (packed)", ops[oi], type, csr, abc);
        }
    }
    return taken;
}

static int run_silicon(long n) {
    if (!(cpu_features() & CPU_FMA)) { printf("SKIP: no FMA on this CPU\n"); return 77; }
    /* the integer path alone, scalar */
    hw_on = 0;
    memset(hits, 0, sizeof hits);
    long fast_cases = 0;
    silicon_scalar(n / 2, int_hits, &fast_cases);
    printf("silicon (integer path): %ld scalar cases, %ld through the fast path\n", n / 2, fast_cases);
    report("silicon (integer path)", 0, 16);
    if (fast_cases < n / 8) { printf("FAIL: fast path rarely taken\n"); fails++; }
    /* the kernel: scalar and packed */
    hw_on = 1;
    memset(hits, 0, sizeof hits);
    kernel_ran = kernel_calls = kernel_exact_calls = live_calls = live_declined = 0;
    long scalar_cases = 0;
    silicon_scalar(n / 8, kernel_hits, &scalar_cases);        /* scalar forms stay on the integer path */
    long kernel_cases = silicon_packed(n / 8);
    printf("silicon (kernel): %ld scalar cases (%ld took the kernel), %ld packed cases, %ld through the kernel\n",
           n / 8, scalar_cases, n / 8, kernel_cases);
    if (scalar_cases) { printf("FAIL: the kernel ran for a scalar form\n"); fails++; }
    report("silicon (kernel)", H_KD, 23);
    report("silicon (kernel)", 25, 27);
    if (kernel_cases < n / 32) { printf("FAIL: kernel rarely taken\n"); fails++; }
    report_live("silicon (kernel)", 1);
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
