/* fma_pending_hw.c -- one packed FMA on this CPU under a given MXCSR (built with FMA enabled). */
#include "fma_pending_hw.h"

#define HW(insn)                                                                               \
    __asm__ volatile("vstmxcsr %[old]\n\t"                                                     \
                     "vmovups %[a], %%ymm0\n\t" "vmovups %[b], %%ymm1\n\t" "vmovups %[c], %%ymm2\n\t" \
                     "vldmxcsr %[in]\n\t"                                                      \
                     insn " %%ymm1, %%ymm0, %%ymm2\n\t"                                        \
                     "vstmxcsr %[after]\n\t"                                                   \
                     "vldmxcsr %[old]\n\t"                                                     \
                     "vmovups %%ymm2, %[o]"                                                    \
                     : [old] "=m"(old), [after] "=m"(after), [o] "=m"(*out)                    \
                     : [in] "m"(csr), [a] "m"(*a), [b] "m"(*b), [c] "m"(*c)                    \
                     : "ymm0", "ymm1", "ymm2", "memory")

uint32_t fma_pending_hw(int dbl, const ymm256 *a, const ymm256 *b, const ymm256 *c, ymm256 *out, uint32_t csr) {
    uint32_t old, after;
    if (dbl) HW("vfmadd231pd"); else HW("vfmadd231ps");
    return after;
}
