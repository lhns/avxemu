/* fma_fast_hw.c -- one scalar FMA on this CPU under a given MXCSR (built with FMA enabled). */
#include "fma_fast_hw.h"

/* x*y+z, x*y-z, -(x*y)+z, -(x*y)-z under csr (its flags cleared); *flags gets the raised ones. */
#define HW(insn, mov)                                                                             \
    do {                                                                                             \
        uint32_t old, in = csr & ~0x3Fu, after;                                                      \
        __asm__ volatile("vstmxcsr %[old]\n\t"                                                       \
                         "vldmxcsr %[in]\n\t"                                                        \
                         mov " %[x], %%xmm0\n\t" mov " %[y], %%xmm1\n\t" mov " %[z], %%xmm2\n\t"     \
                         insn " %%xmm1, %%xmm0, %%xmm2\n\t"                                          \
                         mov " %%xmm2, %[r]\n\t"                                                     \
                         "vstmxcsr %[after]\n\t"                                                     \
                         "vldmxcsr %[old]"                                                           \
                         : [old] "=m"(old), [after] "=m"(after), [r] "=m"(r)                         \
                         : [in] "m"(in), [x] "m"(x), [y] "m"(y), [z] "m"(z)                          \
                         : "xmm0", "xmm1", "xmm2", "memory");                                        \
        *flags = after & 0x3Fu;                                                                      \
    } while (0)

uint64_t fma_fast_hw_d(int variant, uint64_t x, uint64_t y, uint64_t z, uint32_t csr, uint32_t *flags) {
    uint64_t r;
    switch (variant) {
    case 0: HW("vfmadd231sd", "vmovq"); break;
    case 1: HW("vfmsub231sd", "vmovq"); break;
    case 2: HW("vfnmadd231sd", "vmovq"); break;
    default: HW("vfnmsub231sd", "vmovq"); break;
    }
    return r;
}

uint32_t fma_fast_hw_s(int variant, uint32_t x, uint32_t y, uint32_t z, uint32_t csr, uint32_t *flags) {
    uint32_t r;
    switch (variant) {
    case 0: HW("vfmadd231ss", "vmovd"); break;
    case 1: HW("vfmsub231ss", "vmovd"); break;
    case 2: HW("vfnmadd231ss", "vmovd"); break;
    default: HW("vfnmsub231ss", "vmovd"); break;
    }
    return r;
}

typedef struct { uint8_t b[32]; } buf32;

#define HWP(insn, reg)                                                                            \
    do {                                                                                             \
        uint32_t old, in = csr & ~0x3Fu, after;                                                      \
        __asm__ volatile("vstmxcsr %[old]\n\t"                                                       \
                         "vldmxcsr %[in]\n\t"                                                        \
                         "vmovups %[x], %%" reg "0\n\t" "vmovups %[y], %%" reg "1\n\t"               \
                         "vmovups %[z], %%" reg "2\n\t"                                              \
                         insn " %%" reg "1, %%" reg "0, %%" reg "2\n\t"                              \
                         "vmovups %%" reg "2, %[r]\n\t"                                              \
                         "vstmxcsr %[after]\n\t"                                                     \
                         "vldmxcsr %[old]"                                                           \
                         : [old] "=m"(old), [after] "=m"(after), [r] "=m"(*(buf32 *)out)             \
                         : [in] "m"(in), [x] "m"(*(const buf32 *)a), [y] "m"(*(const buf32 *)b),     \
                           [z] "m"(*(const buf32 *)c)                                                \
                         : "xmm0", "xmm1", "xmm2", "memory");                                        \
        *flags = after & 0x3Fu;                                                                      \
    } while (0)

/* the 18 ops of one width and type, by oi */
#define CASES(S, REG)                                                                                \
    case 0: HWP("vfmadd132" S, REG); break;    case 1: HWP("vfmadd213" S, REG); break;               \
    case 2: HWP("vfmadd231" S, REG); break;    case 3: HWP("vfmsub132" S, REG); break;               \
    case 4: HWP("vfmsub213" S, REG); break;    case 5: HWP("vfmsub231" S, REG); break;               \
    case 6: HWP("vfnmadd132" S, REG); break;   case 7: HWP("vfnmadd213" S, REG); break;             \
    case 8: HWP("vfnmadd231" S, REG); break;   case 9: HWP("vfnmsub132" S, REG); break;             \
    case 10: HWP("vfnmsub213" S, REG); break;  case 11: HWP("vfnmsub231" S, REG); break;            \
    case 12: HWP("vfmaddsub132" S, REG); break; case 13: HWP("vfmaddsub213" S, REG); break;         \
    case 14: HWP("vfmaddsub231" S, REG); break; case 15: HWP("vfmsubadd132" S, REG); break;         \
    case 16: HWP("vfmsubadd213" S, REG); break; default: HWP("vfmsubadd231" S, REG); break;

void fma_fast_hw_p(int oi, int dbl, int ymm, const void *a, const void *b, const void *c, void *out,
                   uint32_t csr, uint32_t *flags) {
    for (int i = 0; i < 32; i++) ((uint8_t *)out)[i] = 0;
    if (dbl && ymm) switch (oi) { CASES("pd", "ymm") }
    else if (dbl) switch (oi) { CASES("pd", "xmm") }
    else if (ymm) switch (oi) { CASES("ps", "ymm") }
    else switch (oi) { CASES("ps", "xmm") }
}
