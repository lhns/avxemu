/*
 * softfma.c — software FMA + F16C, SSE-only (compile -mno-avx -mno-fma).
 *
 * The FMA is exact integer arithmetic, not libm fma(): libm's result, NaN choice and flags
 * depend on the host's MXCSR handling, so it cannot match the CPU under every rounding mode,
 * FTZ/DAZ, sticky flag and unmasked exception. The MXCSR value passed in supplies rounding,
 * FTZ, DAZ and the exception masks; the raised flags are ORed into it.
 *
 * Fast paths (absent from SOFTFMA_NO_FAST builds): with every exception masked and round to nearest,
 * packed forms first try a kernel in native SSE arithmetic on the lanes it can prove; the rest, and
 * the scalar forms, go to an integer path and then to the exact code. The kernel reads and restores
 * the live MXCSR itself.
 */

#include "softfma.h"
#include <string.h>

float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t man  = h & 0x3FF;
    uint32_t f;
    if (exp == 0) {
        if (man == 0) {
            f = sign;                                /* +-0 */
        } else {
            exp = 1;                                 /* subnormal half -> normal float */
            while (!(man & 0x400)) { man <<= 1; exp--; }
            man &= 0x3FF;
            f = sign | ((exp + (127 - 15)) << 23) | (man << 13);
        }
    } else if (exp == 0x1F) {
        f = sign | 0x7F800000u | (man << 13) | (man ? 0x400000u : 0);   /* inf / nan; quiet an sNaN */
    } else {
        f = sign | ((exp + (127 - 15)) << 23) | (man << 13);
    }
    float out;
    memcpy(&out, &f, 4);
    return out;
}

typedef unsigned __int128 u128;
enum { F_IE = 1, F_DE = 2, F_ZE = 4, F_OE = 8, F_UE = 16, F_PE = 32 };   /* MXCSR bit order */

static int never_x(void) { int v = 0; __asm__("" : "+r"(v)); return v; }   /* LAB: opaque zero */
typedef struct { int mb, eb; } ffmt;       /* mantissa and exponent bits */
static const ffmt FS = {23, 8}, FD = {52, 11};

typedef struct { int cls, sign, e; u128 m; uint64_t bits; } fop;   /* value = m * 2^e */
enum { C_ZERO, C_FIN, C_INF, C_QNAN, C_SNAN };

static int msb128(u128 v) {
    uint64_t h = (uint64_t)(v >> 64);
    return h ? 127 - __builtin_clzll(h) : 63 - __builtin_clzll((uint64_t)v);
}

static fop unpack(ffmt f, uint64_t bits, int daz, int *denorm) {
    fop o; int bias = (1 << (f.eb - 1)) - 1;
    uint64_t man = bits & ((1ull << f.mb) - 1), ex = (bits >> f.mb) & ((1u << f.eb) - 1);
    o.bits = bits; o.sign = (int)(bits >> (f.mb + f.eb)) & 1; o.m = 0; o.e = 0;
    if (ex == (1u << f.eb) - 1) o.cls = man ? ((man >> (f.mb - 1)) & 1 ? C_QNAN : C_SNAN) : C_INF;
    else if (ex == 0 && (man == 0 || daz)) o.cls = C_ZERO;     /* DAZ: a denormal is zero, no DE */
    else {
        o.cls = C_FIN;
        if (ex == 0) { *denorm = 1; o.m = man; o.e = 1 - bias - f.mb; }
        else { o.m = man | (1ull << f.mb); o.e = (int)ex - bias - f.mb; }
    }
    return o;
}

/* Round (r + sticky fraction in (0,1) lsb) * 2^e0 with sign. rm: 0 nearest-even, 1 down, 2 up, 3 zero. */
#define ROUND(SH, QOUT, INEX) do { \
    int sh_ = (SH), above_, eq_, up_; u128 q_, rem_, half_; \
    if (sh_ <= 0) { q_ = r << -sh_; (INEX) = sticky; above_ = eq_ = 0; } \
    else if (sh_ >= 128) { q_ = 0; (INEX) = 1; above_ = eq_ = 0; } \
    else { q_ = r >> sh_; rem_ = r & (((u128)1 << sh_) - 1); half_ = (u128)1 << (sh_ - 1); \
           (INEX) = rem_ != 0 || sticky; above_ = rem_ > half_ || (rem_ == half_ && sticky); \
           eq_ = rem_ == half_ && !sticky; } \
    up_ = rm == 0 ? (above_ || (eq_ && (q_ & 1))) : rm == 1 ? (sign && (INEX)) : rm == 2 ? (!sign && (INEX)) : 0; \
    (QOUT) = q_ + up_; } while (0)

static uint64_t round_pack(ffmt f, int sign, u128 r, int e0, int sticky, int rm, int ftz, int um, int *fl) {
    int bias = (1 << (f.eb - 1)) - 1, p = f.mb + 1, emin = 1 - bias, emax = bias;
    int ev = msb128(r) + e0;                       /* value in [2^ev, 2^(ev+1)) */
    int lsb = (ev > emin ? ev : emin) - f.mb;      /* exponent of the last kept bit */
    uint64_t signbit = (uint64_t)sign << (f.mb + f.eb);
    int inexact, tiny;
    u128 q;
    ROUND(lsb - e0, q, inexact);
    /* tiny = below the minimum normal after rounding to p bits with an unbounded exponent */
    tiny = ev < emin;
    if (tiny && ev == emin - 1) { u128 qu; int iu; ROUND(lsb - e0 - 1, qu, iu); (void)iu; if (qu >> p) tiny = 0; }
    if (q >> p) { q >>= 1; ev++; }                 /* rounding carried out of the top */
    if (tiny && !um && ftz) { *fl |= F_UE | F_PE; return signbit; }
    if (tiny && (inexact || um)) *fl |= F_UE;
    if (ev > emax) {
        *fl |= F_OE | F_PE;
        int inf = rm == 0 || (rm == 1 && sign) || (rm == 2 && !sign);
        return signbit | (inf ? ((uint64_t)((1u << f.eb) - 1) << f.mb)
                              : (((uint64_t)((1u << f.eb) - 2) << f.mb) | ((1ull << f.mb) - 1)));
    }
    if (inexact) *fl |= F_PE;
    uint64_t qq = (uint64_t)q;
    uint64_t field = (qq >> f.mb) ? (uint64_t)((ev > emin ? ev : emin) + bias - 1) : 0;   /* implicit bit: normal */
    return signbit | ((field << f.mb) + qq);
}

/* One element: (+-)(m1*m2) (+-) ad. *den: a denormal operand was read (and DAZ is off). */
static uint64_t fma1x(ffmt f, uint64_t m1b, uint64_t m2b, uint64_t adb,
                      int negp, int negc, uint32_t csr, int *fl, int *den) {
    int rm = (csr >> 13) & 3, ftz = (csr >> 15) & 1, daz = (csr >> 6) & 1, um = !((csr >> 11) & 1);
    int anynan;
    uint64_t qbit = 1ull << (f.mb - 1);
    fop a = unpack(f, m1b, daz, den), b = unpack(f, m2b, daz, den), c = unpack(f, adb, daz, den);
    anynan = a.cls >= C_QNAN || b.cls >= C_QNAN || c.cls >= C_QNAN;
    if (anynan) {      /* the first NaN in role order m1, m2, addend, quieted; IE for any SNaN among them */
        if (a.cls == C_SNAN || b.cls == C_SNAN || c.cls == C_SNAN) *fl |= F_IE;
        *den = 0;
        return (a.cls >= C_QNAN ? a.bits : b.cls >= C_QNAN ? b.bits : c.bits) | qbit;
    }
    uint64_t indef = (1ull << (f.mb + f.eb)) | (((1ull << f.eb) - 1) << f.mb) | qbit;
    uint64_t inf = ((1ull << f.eb) - 1) << f.mb;
    int sp = a.sign ^ b.sign ^ negp, sc = c.sign ^ negc;
    if ((a.cls == C_INF && b.cls == C_ZERO) || (a.cls == C_ZERO && b.cls == C_INF)) { *fl |= F_IE; return indef; }
    if (a.cls == C_INF || b.cls == C_INF) {
        if (c.cls == C_INF && sc != sp) { *fl |= F_IE; return indef; }
        return ((uint64_t)sp << (f.mb + f.eb)) | inf;
    }
    if (c.cls == C_INF) return ((uint64_t)sc << (f.mb + f.eb)) | inf;
    u128 mp = a.m * b.m, mc = c.m, r;
    int ep = a.e + b.e, ec = c.e, sign, sticky = 0, e0;
    if (mp == 0 && mc == 0)
        return (uint64_t)(sp == sc ? sp : rm == 1) << (f.mb + f.eb);
    if (mp) { int s = 125 - msb128(mp); mp <<= s; ep -= s; }
    if (mc) { int s = 125 - msb128(mc); mc <<= s; ec -= s; }
    if (mp == 0) { r = mc; e0 = ec; sign = sc; }
    else if (mc == 0) { r = mp; e0 = ep; sign = sp; }
    else {
        u128 x, y; int sx, sy;
        if (ep >= ec) { x = mp; sx = sp; y = mc; sy = sc; e0 = ep; } else { x = mc; sx = sc; y = mp; sy = sp; e0 = ec; }
        int d = ep >= ec ? ep - ec : ec - ep;
        if (d >= 128) { sticky = 1; y = 0; }
        else if (d) { sticky = (y & (((u128)1 << d) - 1)) != 0; y >>= d; }
        if (sx == sy) { r = x + y; sign = sx; }
        else if (x >= y) { r = x - y; sign = sx; if (sticky) r--; }
        else { r = y - x; sign = sy; }
        if (r == 0 && !sticky) return (uint64_t)(rm == 1) << (f.mb + f.eb);     /* exact cancellation */
    }
    return round_pack(f, sign, r, e0, sticky, rm, ftz, um, fl);
}

/*
 * Fast path, taken only in round-to-nearest with every operand normal or zero and a normal result:
 * then DAZ, FTZ, DE, IE, UE and OE cannot arise and only PE is computed. Anything else returns 0 and
 * the element goes to fma1. x, y, z already carry the op's negations.
 * The product and the addend sit at the top of one integer; bits shifted out of the smaller one
 * become `sticky`. Both have many trailing zeros, so a sticky operand is far below the other and a
 * subtraction cancels at most one bit; an exact zero sum has no sticky.
 */
#ifndef SOFTFMA_NO_FAST    /* test builds: softfma alone, the reference for the fast path */
#ifndef SOFTFMA_HIT
#define SOFTFMA_HIT(k) ((void)0)    /* test builds count fast-path hits by category */
#else
#define SOFTFMA_REASONS 1           /* ... and the kernel's outcome per lane */
#endif
#ifndef SOFTFMA_HW_ON
#define SOFTFMA_HW_ON() 1           /* test builds switch the kernel off to exercise the integer path */
#endif
#include <immintrin.h>

/* RN-even of (r + sticky fraction) * 2^e0 with sign s, r != 0, to p bits; 0 unless the result is normal. */
#define FAST_ROUND_FN(name, T, msb, p, bias, sbit)                                                  \
    static int name(T r, int e0, int s, int sticky, uint64_t *out, int *fl) {                      \
        int m = msb(r), be = e0 + m + (bias), sh = m - ((p) - 1), inexact = sticky;                \
        if (be < 1) { SOFTFMA_HIT(8); return 0; } /* below the normal range before rounding */     \
        if (sh > 0) {                                                                              \
            T rem = r & (((T)1 << sh) - 1), half = (T)1 << (sh - 1);                               \
            r >>= sh;                                                                              \
            inexact |= rem != 0;                                                                   \
            if (rem == half) SOFTFMA_HIT(sticky ? 11 : r & 1 ? 9 : 10);                            \
            r += rem > half || (rem == half && (sticky || (r & 1)));                               \
            if (r >> (p)) { SOFTFMA_HIT(12); r >>= 1; be++; }                                      \
        } else if (sticky) return 0;              /* unreachable: sticky implies a large r */      \
        else r <<= -sh;                                                                            \
        if (be >= 2 * (bias) + 1) { SOFTFMA_HIT(13); return 0; }   /* overflow */                 \
        if (be == 1) SOFTFMA_HIT(14);                                                              \
        if (be == 2 * (bias)) SOFTFMA_HIT(15);                                                     \
        if (inexact) *fl |= F_PE;                                                                  \
        *out = ((uint64_t)s << (sbit)) + ((uint64_t)(be - 1) << ((p) - 1)) + (uint64_t)r;          \
        return 1;                                                                                  \
    }

/* Align y (smaller exponent) to x, then add or subtract: magnitude r, sign s. */
#define FAST_ADD(T, x, ex, sx, y, ey, sy, r, s, sticky) do {                                       \
    int d_ = (ex) - (ey); T y_ = (y);                                                              \
    if (d_ >= (int)(sizeof(T) * 8)) { (sticky) = 1; y_ = 0; }                                      \
    else if (d_) { (sticky) = (y_ & (((T)1 << d_) - 1)) != 0; y_ >>= d_; }                         \
    if ((sx) == (sy)) { (r) = (x) + y_; (s) = (sx); }                                              \
    else if ((x) >= y_) { (r) = (x) - y_ - (T)(sticky); (s) = (sx); if (sticky) SOFTFMA_HIT(16); } \
    else { (r) = y_ - (x); (s) = (sy); }                                                           \
} while (0)

static int msb64(uint64_t v) { return 63 - __builtin_clzll(v); }
FAST_ROUND_FN(fast_round_d, u128, msb128, 53, 1023, 63)
FAST_ROUND_FN(fast_round_s, uint64_t, msb64, 24, 127, 31)

/* binary64: the 106-bit product at bit 125 or 126 of a u128, the addend's 53 bits at bit 125. */
static int fast_d(uint64_t x, uint64_t y, uint64_t z, uint64_t *out, int *fl) {
    const uint64_t M = (1ull << 52) - 1;
    unsigned ex = (unsigned)(x >> 52) & 0x7ff, ey = (unsigned)(y >> 52) & 0x7ff, ez = (unsigned)(z >> 52) & 0x7ff;
    if ((ex - 1 >= 0x7fe && (x << 1)) || (ey - 1 >= 0x7fe && (y << 1)) || (ez - 1 >= 0x7fe && (z << 1)))
        return 0;                                          /* Inf, NaN or subnormal */
    int sp = (int)((x ^ y) >> 63), sz = (int)(z >> 63), s, sticky = 0, e0;
    if (!(x << 1) || !(y << 1)) {                          /* product +-0: z exactly, or the RN zero sign */
        SOFTFMA_HIT(0);
        *out = (z << 1) ? z : (uint64_t)(sp & sz) << 63;
        return 1;
    }
    u128 p = (u128)((x & M) | (M + 1)) * ((y & M) | (M + 1)) << 21, r;
    int ep = (int)(ex + ey) - 2 * 1075 - 21;
    if (!(z << 1)) { SOFTFMA_HIT(1); return fast_round_d(p, ep, sp, 0, out, fl); }
    u128 c = (u128)((z & M) | (M + 1)) << 73;
    int ec = (int)ez - 1075 - 73;
    if (ep >= ec) { FAST_ADD(u128, p, ep, sp, c, ec, sz, r, s, sticky); e0 = ep; }
    else          { FAST_ADD(u128, c, ec, sz, p, ep, sp, r, s, sticky); e0 = ec; }
    if (r == 0) { SOFTFMA_HIT(2); *out = 0; return 1; }   /* exact cancellation: +0 in RN */
    SOFTFMA_HIT(3);
    return fast_round_d(r, e0, s, sticky, out, fl);
}

/* binary32: the 48-bit product at bit 61 or 62 of a u64, the addend's 24 bits at bit 61. */
static int fast_s(uint64_t x, uint64_t y, uint64_t z, uint64_t *out, int *fl) {
    const uint64_t M = (1u << 23) - 1;
    unsigned ex = (unsigned)(x >> 23) & 0xff, ey = (unsigned)(y >> 23) & 0xff, ez = (unsigned)(z >> 23) & 0xff;
    uint64_t mag = 0x7fffffffu;
    if ((ex - 1 >= 0xfe && (x & mag)) || (ey - 1 >= 0xfe && (y & mag)) || (ez - 1 >= 0xfe && (z & mag)))
        return 0;
    int sp = (int)((x ^ y) >> 31) & 1, sz = (int)(z >> 31) & 1, s, sticky = 0, e0;
    if (!(x & mag) || !(y & mag)) {
        SOFTFMA_HIT(4);
        *out = (z & mag) ? z : (uint64_t)(sp & sz) << 31;
        return 1;
    }
    uint64_t p = ((x & M) | (M + 1)) * ((y & M) | (M + 1)) << 15, r;
    int ep = (int)(ex + ey) - 2 * 150 - 15;
    if (!(z & mag)) { SOFTFMA_HIT(5); return fast_round_s(p, ep, sp, 0, out, fl); }
    uint64_t c = ((z & M) | (M + 1)) << 38;
    int ec = (int)ez - 150 - 38;
    if (ep >= ec) { FAST_ADD(uint64_t, p, ep, sp, c, ec, sz, r, s, sticky); e0 = ep; }
    else          { FAST_ADD(uint64_t, c, ec, sz, p, ep, sp, r, s, sticky); e0 = ec; }
    if (r == 0) { SOFTFMA_HIT(6); *out = 0; return 1; }
    SOFTFMA_HIT(7);
    return fast_round_s(r, e0, s, sticky, out, fl);
}

/* The kernel stage of a packed form (below, after the scalar code): writes the lanes it proves to out and
 * returns them as a mask. */
__attribute__((noinline, section(".text.softfma_kernel"))) static unsigned hw_stage(int dbl, int order, int negp, int negc, int alt, int n,
                         const uint8_t *a, const uint8_t *b, const uint8_t *c, uint8_t *out, uint32_t csr, int *fl);

#endif /* SOFTFMA_NO_FAST */

/* DE is not reported when the element also raises IE or propagates a NaN. */
__attribute__((noinline)) static uint64_t fma1(ffmt f, uint64_t m1b, uint64_t m2b, uint64_t adb,
                     int negp, int negc, uint32_t csr, int *fl) {
    int l = 0, den = 0;
    uint64_t r = fma1x(f, m1b, m2b, adb, negp, negc, csr, &l, &den);
    if (den && !(l & F_IE)) l |= F_DE;
    *fl |= l;
    return r;
}

/* alt: 1 subtracts the addend in even elements (vfmaddsub), 2 in odd ones (vfmsubadd).
 * Inlined per format so the element width is a constant. `done` are the lanes already in out. */
__attribute__((always_inline)) static inline void fma_lanes(ffmt f, int order, int negp, int negc, int alt, int n,
                      const uint8_t *a, const uint8_t *b, const uint8_t *c, uint8_t *out,
                      uint32_t csr, int *fl, unsigned done) {
    int w = (f.mb + f.eb + 1) / 8;
#ifndef SOFTFMA_NO_FAST
    int fast = ((csr >> 13) & 3) == 0, dbl = w == 8;   /* round to nearest */
    uint64_t sign = 1ull << (w * 8 - 1);
#endif
    for (int i = 0; i < n; i++) {
        if (done >> i & 1) continue;
        uint64_t av = 0, bv = 0, cv = 0, m1, m2, ad;
        memcpy(&av, a + i * w, w); memcpy(&bv, b + i * w, w); memcpy(&cv, c + i * w, w);
        switch (order) {
        case 0: m1 = cv; m2 = bv; ad = av; break;      /* 132: dst*src3 + src2 */
        case 1: m1 = av; m2 = cv; ad = bv; break;      /* 213: src2*dst + src3 */
        default: m1 = av; m2 = bv; ad = cv; break;     /* 231: src2*src3 + dst */
        }
        int nc = negc ^ (alt == 1 ? !(i & 1) : alt == 2 ? (i & 1) : 0);
        uint64_t r;
#ifndef SOFTFMA_NO_FAST
        if (fast && (dbl ? fast_d : fast_s)(m1 ^ (negp ? sign : 0), m2, ad ^ (nc ? sign : 0), &r, fl)) {
            memcpy(out + i * w, &r, w);
            continue;
        }
#endif
        r = fma1(f, m1, m2, ad, negp, nc, csr, fl);
        memcpy(out + i * w, &r, w);
    }
}

int fp_raise(uint32_t *mxcsr, uint32_t fl) {
    if (!fl) return 1;
    uint32_t csr = *mxcsr;
    *mxcsr = csr | fl;
    return !(fl & ~(csr >> 7) & 0x3F);
}

int fma_exec(vex_op op, int type,
             const ymm256 *a, const ymm256 *b, const ymm256 *c, ymm256 *out,
             uint32_t *mxcsr) {
    int order, negp, negc, alt = 0;
    switch (op) {
    case VFMADDSUB132: order=0; negp=0; negc=0; alt=1; break;
    case VFMADDSUB213: order=1; negp=0; negc=0; alt=1; break;
    case VFMADDSUB231: order=2; negp=0; negc=0; alt=1; break;
    case VFMSUBADD132: order=0; negp=0; negc=0; alt=2; break;
    case VFMSUBADD213: order=1; negp=0; negc=0; alt=2; break;
    case VFMSUBADD231: order=2; negp=0; negc=0; alt=2; break;
    case VFMADD132: order=0; negp=0; negc=0; break;
    case VFMADD213: order=1; negp=0; negc=0; break;
    case VFMADD231: order=2; negp=0; negc=0; break;
    case VFMSUB132: order=0; negp=0; negc=1; break;
    case VFMSUB213: order=1; negp=0; negc=1; break;
    case VFMSUB231: order=2; negp=0; negc=1; break;
    case VFNMADD132:order=0; negp=1; negc=0; break;
    case VFNMADD213:order=1; negp=1; negc=0; break;
    case VFNMADD231:order=2; negp=1; negc=0; break;
    case VFNMSUB132:order=0; negp=1; negc=1; break;
    case VFNMSUB213:order=1; negp=1; negc=1; break;
    case VFNMSUB231:order=2; negp=1; negc=1; break;
    default: return 0;
    }
    int scalar = type == FT_SD || type == FT_SS, dbl = type == FT_SD || type == FT_PD;
    int n = scalar ? 1 : dbl ? 4 : 8, fl = 0;
    uint32_t csr = *mxcsr;
    ymm256 tmp;
    if (scalar) { memcpy(tmp.b, c->b, 16); memset(tmp.b + 16, 0, 16); }   /* keep dst's upper bits, zero 255:128 */
    else memset(tmp.b, 0, 32);
    unsigned done = 0;
#ifndef SOFTFMA_NO_FAST
    if (!scalar && never_x()) done = hw_stage(dbl, order, negp, negc, alt, n, a->b, b->b, c->b, tmp.b, csr, &fl);
#endif
    if (done != (1u << n) - 1) {
        if (dbl) fma_lanes(FD, order, negp, negc, alt, n, a->b, b->b, c->b, tmp.b, csr, &fl, done);
        else fma_lanes(FS, order, negp, negc, alt, n, a->b, b->b, c->b, tmp.b, csr, &fl, done);
    }
    /* An unmasked IE or DE stops silicon before it computes: the MXCSR then holds those two only,
     * not the OE/UE/PE other lanes would have raised (test/fma_pending.c). */
    if ((fl & 3) & ~(csr >> 7) & 3) fl &= 3;
    if (!fp_raise(mxcsr, (uint32_t)fl)) return AVXEMU_FP_PENDING;
    *out = tmp;
    return 1;
}

/*
 * The kernel stage is in a section of its own on purpose: its size in front of fma_exec moved the scalar code's
 * addresses, and the scalar forms ran up to a fifth slower on AMD for it (the instruction count was the same).
 */
#ifndef SOFTFMA_NO_FAST
/*
 * Hardware-arithmetic kernel: a lane's result by native SSE arithmetic where that is provably the
 * correctly rounded FMA, else "not ok" and the lane takes fast_* and fma1. It runs only when the
 * incoming MXCSR and the live one both have every exception masked and round to nearest, so the
 * native operations neither trap nor round differently. The flags they set are discarded: the live
 * MXCSR is read first and written back if it changed. PE comes from the inexact bit alone. FTZ and
 * DAZ do not matter: the ranges below keep every operand, intermediate and result normal.
 *
 * binary64, a*b+c with |a|,|b| in [2^-400, 2^401) and |c| in [2^-800, 2^801). Veltkamp split and
 * Dekker's TwoProduct give a*b = uh + ul exactly. TwoSum(c,uh) = (th,tl), TwoSum(tl,ul) = (v,w),
 * TwoSum(th,v) = (r,z): the exact x = a*b+c = r + z + w. Every nonzero intermediate is in
 * [2^-904, 2^803), so nothing is subnormal or overflows (the split's a*(2^27+1) included) and every
 * step is exact. If w == 0, x = th+v and r = RN(x). Otherwise let H = ulp(r)/2, or half that when r
 * is a power of two (its lower gap is half): H is at most half the gap to either neighbour of r, so
 * |x-r| <= |z|+|w| < H gives r = RN(x). RN is monotone and H is representable, so
 * RN(|z|+|w|) < H implies the exact sum is below H. r == 0 is left to the exact code. The result is
 * inexact iff z+w != 0; a sum of two normal doubles is 0 only if they cancel.
 * A lane whose three operands are all zero (the unused half of a 128-bit form) needs no arithmetic:
 * its result is +0, or -0 when the product and the addend are both negative.
 *
 * binary32: all three operands normal. p = a*b is exact in double (48 bits); TwoSum(p,c) = (d,err),
 * d = RN53(x), f = RN24(d). f = RN24(x) unless d is exactly a float rounding midpoint and x is not:
 * the midpoints are doubles and RN53 is monotone, so x and d lie on the same side of every midpoint
 * but d itself. That case, d == 0 and |d| outside [2^-125, FLT_MAX] (so f is normal and finite) are
 * not ok. Inexact iff err != 0 or f != d.
 */
#define HWB(v) __asm__ volatile("" : "+x"(v))    /* keeps the compiler's FP ops between the MXCSR accesses */

typedef struct {
    unsigned ok, inex, ran;     /* lanes proven, lanes inexact, the kernel ran at all */
#ifdef SOFTFMA_REASONS
    unsigned rng, nz, g, mid, zero;
#endif
} hwmask;

#define HW_INLINE static inline __attribute__((always_inline))
#define HW_ABSM _mm_castsi128_pd(_mm_set1_epi64x(0x7fffffffffffffffll))

HW_INLINE void hw_twosum(__m128d x, __m128d y, __m128d *s, __m128d *e) {
    __m128d t = _mm_add_pd(x, y), bb = _mm_sub_pd(t, x);
    *e = _mm_add_pd(_mm_sub_pd(x, _mm_sub_pd(t, bb)), _mm_sub_pd(y, bb));
    *s = t;
}

HW_INLINE __m128d hw_between(__m128d v, double lo, double hi) {    /* lo <= |v| < hi; false for NaN */
    __m128d a = _mm_and_pd(v, HW_ABSM);
    return _mm_and_pd(_mm_cmpge_pd(a, _mm_set1_pd(lo)), _mm_cmplt_pd(a, _mm_set1_pd(hi)));
}

/* Two binary64 lanes. */
HW_INLINE void hw_pair_d(__m128d a, __m128d b, __m128d c, __m128d *res, __m128d *rng, __m128d *nz,
                         __m128d *grd, __m128d *inex) {
    const __m128d K = _mm_set1_pd(134217729.0), zero = _mm_setzero_pd();
    __m128d t, ah, al, bh, bl, uh, ul, th, tl, v, w, r, z;
    *rng = _mm_and_pd(_mm_and_pd(hw_between(a, 0x1p-400, 0x1p401), hw_between(b, 0x1p-400, 0x1p401)),
                      hw_between(c, 0x1p-800, 0x1p801));
    t = _mm_mul_pd(a, K); ah = _mm_sub_pd(t, _mm_sub_pd(t, a)); al = _mm_sub_pd(a, ah);
    t = _mm_mul_pd(b, K); bh = _mm_sub_pd(t, _mm_sub_pd(t, b)); bl = _mm_sub_pd(b, bh);
    uh = _mm_mul_pd(a, b);
    ul = _mm_add_pd(_mm_add_pd(_mm_add_pd(_mm_sub_pd(_mm_mul_pd(ah, bh), uh), _mm_mul_pd(ah, bl)),
                               _mm_mul_pd(al, bh)), _mm_mul_pd(al, bl));
    hw_twosum(c, uh, &th, &tl);
    hw_twosum(tl, ul, &v, &w);
    hw_twosum(th, v, &r, &z);
    /* H = 2^(E-53) from r's exponent bits, one binade lower when r's mantissa field is zero */
    __m128i rb = _mm_castpd_si128(r);
    __m128i e = _mm_and_si128(_mm_srli_epi64(rb, 52), _mm_set1_epi64x(0x7ff));
    __m128i p2 = _mm_cmpeq_epi64(_mm_and_si128(rb, _mm_set1_epi64x(0xfffffffffffffll)), _mm_setzero_si128());
    __m128d H = _mm_castsi128_pd(_mm_slli_epi64(_mm_add_epi64(_mm_sub_epi64(e, _mm_set1_epi64x(53)), p2), 52));
    __m128d s = _mm_add_pd(_mm_and_pd(z, HW_ABSM), _mm_and_pd(w, HW_ABSM));
    *grd = _mm_or_pd(_mm_cmpeq_pd(w, zero), _mm_cmplt_pd(s, H));
    *nz = _mm_cmpneq_pd(r, zero);
    *inex = _mm_cmpneq_pd(_mm_add_pd(z, w), zero);
    *res = r;
}

/* Two binary32 lanes, already widened to double. */
HW_INLINE void hw_pair_s(__m128d a, __m128d b, __m128d c, __m128 *res, __m128d *rng, __m128d *nz,
                         __m128d *grd, __m128d *mid, __m128d *inex) {
    const __m128d zero = _mm_setzero_pd();
    __m128d d, err;
    *rng = _mm_and_pd(_mm_and_pd(hw_between(a, 0x1p-126, 0x1p128), hw_between(b, 0x1p-126, 0x1p128)),
                      hw_between(c, 0x1p-126, 0x1p128));
    hw_twosum(_mm_mul_pd(a, b), c, &d, &err);
    __m128 f = _mm_cvtpd_ps(d);
    __m128d ad = _mm_and_pd(d, HW_ABSM);
    *grd = _mm_and_pd(_mm_cmpge_pd(ad, _mm_set1_pd(0x1p-125)), _mm_cmple_pd(ad, _mm_set1_pd(0x1.fffffep127)));
    __m128d errnz = _mm_cmpneq_pd(err, zero);
    __m128i lo = _mm_and_si128(_mm_castpd_si128(d), _mm_set1_epi64x(0x1fffffff));
    *mid = _mm_and_pd(_mm_castsi128_pd(_mm_cmpeq_epi64(lo, _mm_set1_epi64x(0x10000000))), errnz);
    *nz = _mm_cmpneq_pd(d, zero);
    *inex = _mm_or_pd(errnz, _mm_cmpneq_pd(_mm_cvtps_pd(f), d));
    *res = f;
}

/*
 * The kernel over n = 2, 4 or 8 lanes of x1*x2 + x3, where x1, x2, x3 are the lanes at p1, p2, p3 (the op's
 * m1, m2 and addend by its order) with the op's negations applied: all of x1 when negp, the addend's
 * even or odd lanes by negc and alt. Lanes it proves are stored to out and set in .ok; the rest of
 * out is not written. A lane whose three operands are all zero has a result that needs no arithmetic
 * (the sign is that of both the product and the addend, else +): it is ok, exact.
 */
#define HW_BEGIN(live) do {                                                                      \
        __asm__ volatile("stmxcsr %0" : "=m"(live));                                             \
        if (((live) & 0x7F80u) != 0x1F80u) { SOFTFMA_HIT(24); return m; }  /* live: not masked, nearest */ \
        m.ran = 1;                                                                               \
    } while (0)
#define HW_END(live, after) do {                                                                 \
        __asm__ volatile("stmxcsr %0" : "=m"(after));                                            \
        if ((after) != (live)) __asm__ volatile("ldmxcsr %0" : : "m"(live));                     \
    } while (0)

#ifdef SOFTFMA_REASONS
#define HW_NOTE(i, ok2, rng2, nz2, g2, mid2, zero2) do {                                         \
        m.rng |= (rng2) << (i); m.nz |= (nz2) << (i); m.g |= (g2) << (i); m.mid |= (mid2) << (i); \
        m.zero |= (zero2) << (i); } while (0)
#else
#define HW_NOTE(i, ok2, rng2, nz2, g2, mid2, zero2) ((void)(rng2), (void)(nz2), (void)(g2), (void)(mid2), (void)(zero2))
#endif

/* Everything a lane pair's loop shares: the sign masks, and the zero-pair test and result. */
#define HW_MASKS                                                                                 \
    const __m128i S = _mm_set1_epi64x((long long)0x8000000000000000ull), Z = _mm_setzero_si128(); \
    const __m128i m1m = negp ? S : Z;                                                            \
    const __m128i adm = _mm_unpacklo_epi64((negc ^ (alt == 1)) ? S : Z, (negc ^ (alt == 2)) ? S : Z)

HW_INLINE unsigned hw_zero_pair(__m128d a, __m128d b, __m128d c) {
    return (unsigned)_mm_testz_si128(_mm_castpd_si128(_mm_or_pd(_mm_or_pd(a, b), c)), _mm_castpd_si128(HW_ABSM));
}
HW_INLINE __m128d hw_zero_result(__m128d a, __m128d b, __m128d c) {    /* sign of a*b and of c both set, else +0 */
    return _mm_and_pd(_mm_and_pd(_mm_xor_pd(a, b), c), _mm_castsi128_pd(_mm_set1_epi64x((long long)0x8000000000000000ull)));
}

HW_INLINE hwmask hw_run_d(int n, const uint8_t *p1, const uint8_t *p2, const uint8_t *p3, int negp, int negc,
                          int alt, uint8_t *out) {
    hwmask m = {0};
    uint32_t live, after;
    HW_BEGIN(live);
    HW_MASKS;
    for (int i = 0; i < n; i += 2) {
        size_t o = (size_t)i * 8;
        __m128i ia = _mm_loadu_si128((const __m128i *)(p1 + o)), ib = _mm_loadu_si128((const __m128i *)(p2 + o)),
                ic = _mm_loadu_si128((const __m128i *)(p3 + o));
        __m128d a = _mm_castsi128_pd(_mm_xor_si128(ia, m1m)), b = _mm_castsi128_pd(ib),
                c = _mm_castsi128_pd(_mm_xor_si128(ic, adm)), r;
        HWB(a); HWB(b); HWB(c);
        unsigned ok2, inex2 = 0, rng2 = 0, nz2 = 0, g2 = 0, zero2 = 0;
        if (hw_zero_pair(a, b, c)) {
            r = hw_zero_result(a, b, c);
            ok2 = zero2 = 3;
            HWB(r);
        } else {
            __m128d rng, nz, grd, inex;
            hw_pair_d(a, b, c, &r, &rng, &nz, &grd, &inex);
            __m128d ok = _mm_and_pd(_mm_and_pd(rng, nz), grd);
            HWB(r); HWB(ok); HWB(inex);
            ok2 = (unsigned)_mm_movemask_pd(ok); inex2 = (unsigned)_mm_movemask_pd(inex);
#ifdef SOFTFMA_REASONS
            HWB(rng); HWB(nz); HWB(grd);
            rng2 = (unsigned)_mm_movemask_pd(rng); nz2 = (unsigned)_mm_movemask_pd(nz);
            g2 = (unsigned)_mm_movemask_pd(grd);
#endif
        }
        _mm_storeu_si128((__m128i *)(out + o), _mm_castpd_si128(r));
        m.ok |= ok2 << i; m.inex |= inex2 << i;
        HW_NOTE(i, ok2, rng2, nz2, g2, 0, zero2);
    }
    HW_END(live, after);
    m.inex &= m.ok;
    return m;
}

HW_INLINE hwmask hw_run_s(int n, const uint8_t *p1, const uint8_t *p2, const uint8_t *p3, int negp, int negc,
                          int alt, uint8_t *out) {
    hwmask m = {0};
    uint32_t live, after;
    HW_BEGIN(live);
    HW_MASKS;
    for (int i = 0; i < n; i += 2) {
        size_t o = (size_t)i * 4;
        __m128 fa = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)(p1 + o))),
               fb = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)(p2 + o))),
               fc = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)(p3 + o)));
        HWB(fa); HWB(fb); HWB(fc);
        __m128d a = _mm_xor_pd(_mm_cvtps_pd(fa), _mm_castsi128_pd(m1m)), b = _mm_cvtps_pd(fb),
                c = _mm_xor_pd(_mm_cvtps_pd(fc), _mm_castsi128_pd(adm));
        __m128 f;
        unsigned ok2, inex2 = 0, rng2 = 0, nz2 = 0, g2 = 0, mid2 = 0, zero2 = 0;
        /* tested on the raw bits: under DAZ the widening reads a subnormal as zero */
        if (_mm_testz_si128(_mm_castps_si128(_mm_or_ps(_mm_or_ps(fa, fb), fc)), _mm_set_epi32(0, 0, 0x7fffffff, 0x7fffffff))) {
            f = _mm_cvtpd_ps(hw_zero_result(a, b, c));
            ok2 = zero2 = 3;
            HWB(f);
        } else {
            __m128d rng, nz, grd, mid, inex;
            hw_pair_s(a, b, c, &f, &rng, &nz, &grd, &mid, &inex);
            __m128d ok = _mm_andnot_pd(mid, _mm_and_pd(_mm_and_pd(rng, nz), grd));
            HWB(f); HWB(ok); HWB(inex);
            ok2 = (unsigned)_mm_movemask_pd(ok); inex2 = (unsigned)_mm_movemask_pd(inex);
#ifdef SOFTFMA_REASONS
            HWB(rng); HWB(nz); HWB(grd); HWB(mid);
            rng2 = (unsigned)_mm_movemask_pd(rng); nz2 = (unsigned)_mm_movemask_pd(nz);
            g2 = (unsigned)_mm_movemask_pd(grd); mid2 = (unsigned)_mm_movemask_pd(mid);
#endif
        }
        _mm_storel_epi64((__m128i *)(out + o), _mm_castps_si128(f));
        m.ok |= ok2 << i; m.inex |= inex2 << i;
        HW_NOTE(i, ok2, rng2, nz2, g2, mid2, zero2);
    }
    HW_END(live, after);
    m.inex &= m.ok;
    return m;
}

HW_INLINE void hw_count(int dbl, int n, hwmask m) {
#ifdef SOFTFMA_REASONS
    if (!m.ran) return;
    unsigned all = (1u << n) - 1;
    for (int i = 0; i < n; i++) {
        if (m.zero >> i & 1) SOFTFMA_HIT(26);
        else if (m.ok >> i & 1) SOFTFMA_HIT(dbl ? 17 : 18);
        else if (!(m.rng >> i & 1)) SOFTFMA_HIT(19);
        else if (!(m.nz >> i & 1)) SOFTFMA_HIT(22);
        else if (!(m.g >> i & 1)) SOFTFMA_HIT(dbl ? 20 : 23);
        else SOFTFMA_HIT(21);
    }
    if (n > 1 && m.ok && m.ok != all) SOFTFMA_HIT(25);
#else
    (void)dbl; (void)n; (void)m;
#endif
}

/* The kernel stage of a packed form: writes the lanes it proves to out and returns them as a mask. A
 * function of its own, so the scalar forms run the code they ran before the kernel existed. */
static unsigned hw_stage(int dbl, int order, int negp, int negc, int alt, int n,
                      const uint8_t *a, const uint8_t *b, const uint8_t *c, uint8_t *out, uint32_t csr, int *fl) {
    if (((csr >> 13) & 3) || ((csr >> 7) & 0x3F) != 0x3F || !SOFTFMA_HW_ON()) return 0;   /* nearest, all masked */
    const uint8_t *p1 = order == 0 ? c : a, *p2 = order == 1 ? c : b, *p3 = order == 0 ? a : order == 1 ? b : c;
    hwmask hm = dbl ? hw_run_d(n, p1, p2, p3, negp, negc, alt, out) : hw_run_s(n, p1, p2, p3, negp, negc, alt, out);
    hw_count(dbl, n, hm);
    if (hm.inex) *fl |= F_PE;           /* only proven lanes set a bit */
    return hm.ok;
}

#endif /* SOFTFMA_NO_FAST */
