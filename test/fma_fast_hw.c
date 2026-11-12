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
