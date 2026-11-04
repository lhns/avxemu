/*
 * oracle.c — differential test of the emulator vs. real AVX2/FMA/BMI silicon.
 * The driver is SSE-only; ground truth lives in oracle_hw.c, compiled for
 * AVX2/FMA/F16C and called only in record mode.
 *
 * For every implemented op: run the real instruction (ground truth), run the
 * SSE-only emulator on identical inputs, assert bit-equality. NaN results are
 * compared NaN-aware (payload not required to match).
 */

#include "vexops.h"
#include "softfma.h"
#include "oracle_hw.h"
#include "refdigest.h"
#include "cpu.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---------- PRNG ---------- */
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t xs(void){ uint64_t x=rng; x^=x<<13; x^=x>>7; x^=x<<17; return rng=x; }
static uint32_t mx_ = 0x1F80;   /* vec_exec takes MXCSR by value: default, all exceptions masked */
static void fill(ymm256*v){ uint64_t*q=(uint64_t*)v->b; for(int i=0;i<4;i++)q[i]=xs(); }

/* ---------- compare helpers ---------- */
static int eq32(const ymm256*x,const ymm256*y){ return memcmp(x->b,y->b,32)==0; }
/* float/double-lane compare that treats NaN==NaN */
static int eq_ps(const ymm256*x,const ymm256*y){
    const float*a=(const float*)x->b,*b=(const float*)y->b;
    for(int i=0;i<8;i++){ if(a[i]!=b[i] && !(a[i]!=a[i] && b[i]!=b[i])) return 0; } return 1; }
static int eq_pd(const ymm256*x,const ymm256*y){
    const double*a=(const double*)x->b,*b=(const double*)y->b;
    for(int i=0;i<4;i++){ if(a[i]!=b[i] && !(a[i]!=a[i] && b[i]!=b[i])) return 0; } return 1; }
/* scalar FMA: element 0 compared by value (+-0 and NaN-payload don't matter),
 * the preserved lanes + zeroed upper compared bitwise. */
static int eq_scalar(const ymm256*r,const ymm256*g,int dbl){
    if(memcmp(r->b+16,g->b+16,16)) return 0;
    if(dbl){ if(memcmp(r->b+8,g->b+8,8)) return 0;
        double x=((const double*)r->b)[0],y=((const double*)g->b)[0];
        return x==y || (x!=x && y!=y); }
    if(memcmp(r->b+4,g->b+4,12)) return 0;
    float x=((const float*)r->b)[0],y=((const float*)g->b)[0];
    return x==y || (x!=x && y!=y); }

/* operand-fill categories */
enum { CAT_BIN, CAT_BIN3, CAT_SHIFT, CAT_VSHIFT, CAT_UN_A, CAT_UN_B, CAT_PERMD, CAT_FLOATCMP_PS, CAT_FLOATCMP_PD };
struct row { vex_op op; int cat; };
static struct row simple_ops[] = {
    {VPADDB,CAT_BIN},{VPADDW,CAT_BIN},{VPADDD,CAT_BIN},{VPADDQ,CAT_BIN},
    {VPSUBB,CAT_BIN},{VPSUBW,CAT_BIN},{VPSUBD,CAT_BIN},{VPSUBQ,CAT_BIN},
    {VPADDSB,CAT_BIN},{VPADDSW,CAT_BIN},{VPADDUSB,CAT_BIN},{VPADDUSW,CAT_BIN},
    {VPSUBSB,CAT_BIN},{VPSUBSW,CAT_BIN},{VPSUBUSB,CAT_BIN},{VPSUBUSW,CAT_BIN},
    {VPAND,CAT_BIN},{VPANDN,CAT_BIN},{VPOR,CAT_BIN},{VPXOR,CAT_BIN},
    {VPCMPEQB,CAT_BIN},{VPCMPEQW,CAT_BIN},{VPCMPEQD,CAT_BIN},{VPCMPEQQ,CAT_BIN},
    {VPCMPGTB,CAT_BIN},{VPCMPGTW,CAT_BIN},{VPCMPGTD,CAT_BIN},{VPCMPGTQ,CAT_BIN},
    {VPMINUB,CAT_BIN},{VPMINUW,CAT_BIN},{VPMINUD,CAT_BIN},{VPMINSB,CAT_BIN},{VPMINSW,CAT_BIN},{VPMINSD,CAT_BIN},
    {VPMAXUB,CAT_BIN},{VPMAXUW,CAT_BIN},{VPMAXUD,CAT_BIN},{VPMAXSB,CAT_BIN},{VPMAXSW,CAT_BIN},{VPMAXSD,CAT_BIN},
    {VPMULLW,CAT_BIN},{VPMULLD,CAT_BIN},{VPMULHW,CAT_BIN},{VPMULHUW,CAT_BIN},{VPMULHRSW,CAT_BIN},
    {VPMULDQ,CAT_BIN},{VPMULUDQ,CAT_BIN},{VPMADDWD,CAT_BIN},{VPMADDUBSW,CAT_BIN},
    {VPAVGB,CAT_BIN},{VPAVGW,CAT_BIN},{VPSADBW,CAT_BIN},
    {VPABSB,CAT_UN_A},{VPABSW,CAT_UN_A},{VPABSD,CAT_UN_A},
    {VPSIGNB,CAT_BIN},{VPSIGNW,CAT_BIN},{VPSIGND,CAT_BIN},{VPHADDD,CAT_BIN},
    {VPSLLW,CAT_SHIFT},{VPSLLD,CAT_SHIFT},{VPSLLQ,CAT_SHIFT},{VPSRLW,CAT_SHIFT},
    {VPSRLD,CAT_SHIFT},{VPSRLQ,CAT_SHIFT},{VPSRAW,CAT_SHIFT},{VPSRAD,CAT_SHIFT},
    {VPSLLVD,CAT_VSHIFT},{VPSLLVQ,CAT_VSHIFT},{VPSRLVD,CAT_VSHIFT},{VPSRLVQ,CAT_VSHIFT},{VPSRAVD,CAT_VSHIFT},
    {VPSHUFB,CAT_BIN},
    {VPACKSSWB,CAT_BIN},{VPACKSSDW,CAT_BIN},{VPACKUSWB,CAT_BIN},{VPACKUSDW,CAT_BIN},
    {VPUNPCKLBW,CAT_BIN},{VPUNPCKHBW,CAT_BIN},{VPUNPCKLWD,CAT_BIN},{VPUNPCKHWD,CAT_BIN},
    {VPUNPCKLDQ,CAT_BIN},{VPUNPCKHDQ,CAT_BIN},{VPUNPCKLQDQ,CAT_BIN},{VPUNPCKHQDQ,CAT_BIN},
    {VPBLENDVB,CAT_BIN3},
    {VPBROADCASTB,CAT_UN_B},{VPBROADCASTW,CAT_UN_B},{VPBROADCASTD,CAT_UN_B},{VPBROADCASTQ,CAT_UN_B},
    {VBROADCASTI128,CAT_UN_B},
    {VPMOVZXBW,CAT_UN_B},{VPMOVZXBD,CAT_UN_B},{VPMOVZXBQ,CAT_UN_B},{VPMOVZXWD,CAT_UN_B},{VPMOVZXWQ,CAT_UN_B},{VPMOVZXDQ,CAT_UN_B},
    {VPMOVSXBW,CAT_UN_B},{VPMOVSXBD,CAT_UN_B},{VPMOVSXBQ,CAT_UN_B},{VPMOVSXWD,CAT_UN_B},{VPMOVSXWQ,CAT_UN_B},{VPMOVSXDQ,CAT_UN_B},
    {VPERMD,CAT_PERMD},{VPERMPS,CAT_PERMD},{VCVTPH2PS,CAT_UN_B},{VPMOVMSKB,CAT_UN_B},
};

/* ---- FMA: all 12 ops x {pd, ps, sd, ss} ---- */
static double rdbl(void){ int e=(int)(xs()%160)-80; return ldexp((double)(int64_t)xs()/9.2e18,e); }
/* roles: pick (m1,m2,add) indices from {a=0,b=1,c=2} by order 0=132,1=213,2=231 */
static void roles_idx(int order,int*m1,int*m2,int*ad){
    if(order==0){*m1=2;*m2=1;*ad=0;} else if(order==1){*m1=0;*m2=2;*ad=1;} else {*m1=0;*m2=1;*ad=2;}
}

/* ---------- canonicalization: exactly the equality eq_ps/eq_pd/eq_scalar use ---------- */
static uint32_t canon32(uint32_t w) {
    if ((w & 0x7f800000u) == 0x7f800000u && (w & 0x007fffffu)) return 0x7fc00000u;
    return w == 0x80000000u ? 0 : w;
}
static uint64_t canon64(uint64_t w) {
    if ((w & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (w & 0x000fffffffffffffull))
        return 0x7ff8000000000000ull;
    return w == 0x8000000000000000ull ? 0 : w;
}
static ymm256 canon_ps(const ymm256 *v) {
    ymm256 r = *v; uint32_t w;
    for (int i = 0; i < 8; i++) { memcpy(&w, r.b + 4*i, 4); w = canon32(w); memcpy(r.b + 4*i, &w, 4); }
    return r;
}
static ymm256 canon_pd(const ymm256 *v) {
    ymm256 r = *v; uint64_t w;
    for (int i = 0; i < 4; i++) { memcpy(&w, r.b + 8*i, 8); w = canon64(w); memcpy(r.b + 8*i, &w, 8); }
    return r;
}
static ymm256 canon_scalar(const ymm256 *v, int dbl) {
    ymm256 r = *v;
    if (dbl) { uint64_t w; memcpy(&w, r.b, 8); w = canon64(w); memcpy(r.b, &w, 8); }
    else     { uint32_t w; memcpy(&w, r.b, 4); w = canon32(w); memcpy(r.b, &w, 4); }
    return r;
}

static int run_simple(int rec) {
    enum { N = sizeof simple_ops / sizeof simple_ops[0] };
    struct ref_op ops[N];
    for (int i = 0; i < N; i++) ref_op_init(&ops[i], vex_op_name(simple_ops[i].op));
    for (int i = 0; i < N; i++) {
        vex_op op = simple_ops[i].op; int cat = simple_ops[i].cat;
        for (int t = 0; t < 20000; t++) {
            ymm256 a, b, c, ref, got; uint64_t gref = 0, ggot = 0;
            fill(&a); fill(&b); fill(&c);
            if (cat == CAT_SHIFT) { memset(b.b, 0, 32); ((uint64_t *)b.b)[0] = xs() % 72; }
            if (cat == CAT_VSHIFT) { for (int k = 0; k < 8; k++) ((uint32_t *)b.b)[k] = (uint32_t)(xs() % 72); }
            memset(&got, 0xCC, 32);
            vec_exec(op, 0, &a, &b, &c, 0, &got, &ggot, &mx_);
            int ok = 1;
            if (rec) {
                memset(&ref, 0, 32);
                oracle_hw_simple(op, &a, &b, &c, &ref, &gref);
                ok = (op == VPMOVMSKB) ? (gref == ggot) : (op == VCVTPH2PS) ? eq_ps(&ref, &got) : eq32(&ref, &got);
            }
            if (op == VPMOVMSKB) {
                if (rec) ref_hw(&ops[i], &gref, 8);
                ref_emu(&ops[i], &ggot, 8);
            } else if (op == VCVTPH2PS) {
                ymm256 x;
                if (rec) { x = canon_ps(&ref); ref_hw(&ops[i], x.b, 32); }
                x = canon_ps(&got); ref_emu(&ops[i], x.b, 32);
            } else {
                if (rec) ref_hw(&ops[i], ref.b, 32);
                ref_emu(&ops[i], got.b, 32);
            }
            ref_case(&ops[i], ok);
        }
    }
    return ref_report(ops, N);
}

static const char *const imm_names[13] = {"vpshufd", "vpshuflw", "vpshufhw", "vpslldq", "vpsrldq",
    "vpblendw", "vpblendd", "vpalignr", "vperm2i128", "vpermq", "vpermpd", "vextracti128", "vinserti128"};
static const vex_op imm_ops[13] = {VPSHUFD, VPSHUFLW, VPSHUFHW, VPSLLDQ, VPSRLDQ, VPBLENDW, VPBLENDD,
    VPALIGNR, VPERM2I128, VPERMQ, VPERMPD, VEXTRACTI128, VINSERTI128};

static int run_imm(int rec) {
    static const uint8_t imms[] = {0x00,0x1B,0x4E,0xD8,0x39,0xAA,0x3C,0xA5,0xFF,1,3,7,15,16,31};
    struct ref_op ops[13];
    for (int k = 0; k < 13; k++) ref_op_init(&ops[k], imm_names[k]);
    for (unsigned ii = 0; ii < sizeof imms / sizeof imms[0]; ii++) {
        uint8_t imm = imms[ii];
        for (int t = 0; t < 3000; t++) {
            ymm256 a, b, c; fill(&a); fill(&b); fill(&c);
            for (int k = 0; k < 13; k++) {
                ymm256 got, ref; uint64_t g;
                memset(&got, 0xCC, 32);
                vec_exec(imm_ops[k], 0, &a, &b, &c, imm, &got, &g, &mx_);
                int ok = 1;
                if (rec) { oracle_hw_imm(k, imm, &a, &b, &ref); ok = eq32(&ref, &got); ref_hw(&ops[k], ref.b, 32); }
                ref_emu(&ops[k], got.b, 32);
                ref_case(&ops[k], ok);
            }
        }
    }
    return ref_report(ops, 13);
}

static const char *const fma_base[12] = {"vfmadd132","vfmadd213","vfmadd231","vfmsub132","vfmsub213",
    "vfmsub231","vfnmadd132","vfnmadd213","vfnmadd231","vfnmsub132","vfnmsub213","vfnmsub231"};

static int run_fma(int rec) {
    static char nm[48][24];
    static const char *const form[4] = {"pd", "ps", "sd", "ss"};
    struct ref_op ops[48];
    for (int i = 0; i < 12; i++)
        for (int f = 0; f < 4; f++) {
            snprintf(nm[i*4 + f], sizeof nm[0], "%s.%s", fma_base[i], form[f]);
            ref_op_init(&ops[i*4 + f], nm[i*4 + f]);
        }
    for (int i = 0; i < 12; i++) {
        vex_op op = (vex_op)(VFMADD132 + i); int variant = i / 3, order = i % 3;
        int m1i, m2i, adi; roles_idx(order, &m1i, &m2i, &adi);
        for (int t = 0; t < 20000; t++) {
            ymm256 a, b, c, got, ref, x; uint64_t g;
            double *ad = (double *)a.b, *bd = (double *)b.b, *cd = (double *)c.b;
            for (int k = 0; k < 4; k++) { ad[k] = rdbl(); bd[k] = rdbl(); cd[k] = rdbl(); }
            const ymm256 *abc[3] = {&a, &b, &c};
            int ok;
            /* pd */
            memset(&got, 0xCC, 32); vec_exec(op, FT_PD, &a, &b, &c, 0, &got, &g, &mx_); ok = 1;
            if (rec) {
                double *o = (double *)ref.b;
                for (int k = 0; k < 4; k++)
                    o[k] = oracle_hw_fma_d(variant, ((const double *)abc[m1i]->b)[k],
                                           ((const double *)abc[m2i]->b)[k], ((const double *)abc[adi]->b)[k]);
                ok = eq_pd(&ref, &got); x = canon_pd(&ref); ref_hw(&ops[i*4 + 0], x.b, 32);
            }
            x = canon_pd(&got); ref_emu(&ops[i*4 + 0], x.b, 32); ref_case(&ops[i*4 + 0], ok);
            /* ps */
            memset(&got, 0xCC, 32); vec_exec(op, FT_PS, &a, &b, &c, 0, &got, &g, &mx_); ok = 1;
            if (rec) {
                float *o = (float *)ref.b;
                for (int k = 0; k < 8; k++)
                    o[k] = oracle_hw_fma_f(variant, ((const float *)abc[m1i]->b)[k],
                                           ((const float *)abc[m2i]->b)[k], ((const float *)abc[adi]->b)[k]);
                ok = eq_ps(&ref, &got); x = canon_ps(&ref); ref_hw(&ops[i*4 + 1], x.b, 32);
            }
            x = canon_ps(&got); ref_emu(&ops[i*4 + 1], x.b, 32); ref_case(&ops[i*4 + 1], ok);
            /* sd: low element computed, upper 64 from c, 255:128 zero */
            memset(&got, 0xCC, 32); vec_exec(op, FT_SD, &a, &b, &c, 0, &got, &g, &mx_); ok = 1;
            if (rec) {
                memcpy(ref.b, c.b, 16); memset(ref.b + 16, 0, 16);
                ((double *)ref.b)[0] = oracle_hw_fma_d(variant, ((const double *)abc[m1i]->b)[0],
                                           ((const double *)abc[m2i]->b)[0], ((const double *)abc[adi]->b)[0]);
                ok = eq_scalar(&ref, &got, 1); x = canon_scalar(&ref, 1); ref_hw(&ops[i*4 + 2], x.b, 32);
            }
            x = canon_scalar(&got, 1); ref_emu(&ops[i*4 + 2], x.b, 32); ref_case(&ops[i*4 + 2], ok);
            /* ss */
            memset(&got, 0xCC, 32); vec_exec(op, FT_SS, &a, &b, &c, 0, &got, &g, &mx_); ok = 1;
            if (rec) {
                memcpy(ref.b, c.b, 16); memset(ref.b + 16, 0, 16);
                ((float *)ref.b)[0] = oracle_hw_fma_f(variant, ((const float *)abc[m1i]->b)[0],
                                           ((const float *)abc[m2i]->b)[0], ((const float *)abc[adi]->b)[0]);
                ok = eq_scalar(&ref, &got, 0); x = canon_scalar(&ref, 0); ref_hw(&ops[i*4 + 3], x.b, 32);
            }
            x = canon_scalar(&got, 0); ref_emu(&ops[i*4 + 3], x.b, 32); ref_case(&ops[i*4 + 3], ok);
        }
    }
    return ref_report(ops, 48);
}

/* FMA over special values: 0, -0, inf, nan, subnormals, overflow, cancellation */
static int run_fma_edge(int rec) {
    static const double sp[] = {
        0.0, -0.0, 1.0, -1.0, 2.0, -2.0, 0.5,
        1.7976931348623157e308, -1.7976931348623157e308,
        2.2250738585072014e-308, 1e308, 1e-308,
    };
    double pool[14]; for (int i = 0; i < 12; i++) pool[i] = sp[i];
    pool[12] = ldexp(1.0, -1074); pool[13] = ldexp(1.0, 1020);
    double nan = pool[0] / pool[0];
    const int NS = 14;
    static char nm[12][24];
    struct ref_op ops[12];
    for (int i = 0; i < 12; i++) { snprintf(nm[i], sizeof nm[0], "%s.edge", fma_base[i]); ref_op_init(&ops[i], nm[i]); }
    for (int i = 0; i < 12; i++) {
        vex_op op = (vex_op)(VFMADD132 + i); int variant = i / 3, order = i % 3;
        int m1i, m2i, adi; roles_idx(order, &m1i, &m2i, &adi);
        for (int x = 0; x <= NS; x++) for (int y = 0; y <= NS; y++) for (int z = 0; z <= NS; z++) {
            double X = (x < NS ? pool[x] : nan), Y = (y < NS ? pool[y] : nan), Z = (z < NS ? pool[z] : nan);
            ymm256 a, b, c, got, ref, v; uint64_t g;
            for (int k = 0; k < 4; k++) { ((double *)a.b)[k] = X; ((double *)b.b)[k] = Y; ((double *)c.b)[k] = Z; }
            const ymm256 *abc[3] = {&a, &b, &c};
            memset(&got, 0xCC, 32); vec_exec(op, FT_PD, &a, &b, &c, 0, &got, &g, &mx_);
            int ok = 1;
            if (rec) {
                for (int k = 0; k < 4; k++)
                    ((double *)ref.b)[k] = oracle_hw_fma_d(variant, ((const double *)abc[m1i]->b)[k],
                                           ((const double *)abc[m2i]->b)[k], ((const double *)abc[adi]->b)[k]);
                ok = eq_pd(&ref, &got); v = canon_pd(&ref); ref_hw(&ops[i], v.b, 32);
            }
            v = canon_pd(&got); ref_emu(&ops[i], v.b, 32); ref_case(&ops[i], ok);
        }
    }
    return ref_report(ops, 12);
}

int main(int argc, char **argv) {
    int rec = ref_init(argc, argv, "oracle", CPU_AVX2 | CPU_FMA | CPU_F16C) == REF_RECORD;
    printf("== simple / shift / extend / broadcast / permd ==\n");
    int f1 = run_simple(rec);
    printf("== immediate ops ==\n");
    int f2 = run_imm(rec);
    printf("== FMA (pd/ps/sd/ss) ==\n");
    int f3 = run_fma(rec);
    printf("== FMA edge cases (0/-0/inf/nan/subnormal/overflow) ==\n");
    int f4 = run_fma_edge(rec);
    int tot = f1 + f2 + f3 + f4 + ref_finish();
    printf("\nTOTAL: %d failing op(s)\n", tot);
    return tot ? 1 : 0;
}
