/*
 * softfma.c — software FMA + F16C, SSE-only (compile -mno-avx -mno-fma).
 *
 * The FMA is exact integer arithmetic, not libm fma(): libm's result, NaN choice and flags
 * depend on the host's MXCSR handling, so it cannot match the CPU under every rounding mode,
 * FTZ/DAZ, sticky flag and unmasked exception. The MXCSR value passed in supplies rounding,
 * FTZ, DAZ and the exception masks; the raised flags are ORed into it.
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

/* DE is not reported when the element also raises IE or propagates a NaN. */
static uint64_t fma1(ffmt f, uint64_t m1b, uint64_t m2b, uint64_t adb,
                     int negp, int negc, uint32_t csr, int *fl) {
    int l = 0, den = 0;
    uint64_t r = fma1x(f, m1b, m2b, adb, negp, negc, csr, &l, &den);
    if (den && !(l & F_IE)) l |= F_DE;
    *fl |= l;
    return r;
}

static void fma_lanes(ffmt f, int order, int negp, int negc, int n,
                      const uint8_t *a, const uint8_t *b, const uint8_t *c, uint8_t *out,
                      uint32_t csr, int *fl) {
    int w = (f.mb + f.eb + 1) / 8;
    for (int i = 0; i < n; i++) {
        uint64_t av = 0, bv = 0, cv = 0, m1, m2, ad;
        memcpy(&av, a + i * w, w); memcpy(&bv, b + i * w, w); memcpy(&cv, c + i * w, w);
        switch (order) {
        case 0: m1 = cv; m2 = bv; ad = av; break;      /* 132: dst*src3 + src2 */
        case 1: m1 = av; m2 = cv; ad = bv; break;      /* 213: src2*dst + src3 */
        default: m1 = av; m2 = bv; ad = cv; break;     /* 231: src2*src3 + dst */
        }
        uint64_t r = fma1(f, m1, m2, ad, negp, negc, csr, fl);
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
    int order, negp, negc;
    switch (op) {
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
    fma_lanes(dbl ? FD : FS, order, negp, negc, n, a->b, b->b, c->b, tmp.b, csr, &fl);
    if (!fp_raise(mxcsr, (uint32_t)fl)) return AVXEMU_FP_PENDING;
    *out = tmp;
    return 1;
}
