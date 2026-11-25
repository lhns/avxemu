#ifndef AVXEMU_FMA_FAST_HW_H
#define AVXEMU_FMA_FAST_HW_H
#include <stdint.h>

/* variant: 0 vfmadd, 1 vfmsub, 2 vfnmadd, 3 vfnmsub (231 form: x*y and z as in softfma's m1*m2, addend).
 * csr must have every exception masked. */
uint64_t fma_fast_hw_d(int variant, uint64_t x, uint64_t y, uint64_t z, uint32_t csr, uint32_t *flags);
uint32_t fma_fast_hw_s(int variant, uint32_t x, uint32_t y, uint32_t z, uint32_t csr, uint32_t *flags);

/* Any of the 18 FMA ops on a packed type, numbered as in test/fma_fast.c (oi / 3: madd, sub, nmadd, nsub,
 * addsub, subadd; oi % 3: 132, 213, 231), in the emulator's operand roles: a = src2, b = src3, c = dst.
 * dbl: pd, else ps. ymm: 256-bit, else 128-bit (the upper 16 bytes of out are then zero). Buffers are 32 bytes.
 * csr must have every exception masked. */
void fma_fast_hw_p(int oi, int dbl, int ymm, const void *a, const void *b, const void *c, void *out,
                   uint32_t csr, uint32_t *flags);

#endif
