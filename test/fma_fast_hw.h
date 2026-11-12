#ifndef AVXEMU_FMA_FAST_HW_H
#define AVXEMU_FMA_FAST_HW_H
#include <stdint.h>

/* variant: 0 vfmadd, 1 vfmsub, 2 vfnmadd, 3 vfnmsub (231 form: x*y and z as in softfma's m1*m2, addend).
 * csr must have every exception masked. */
uint64_t fma_fast_hw_d(int variant, uint64_t x, uint64_t y, uint64_t z, uint32_t csr, uint32_t *flags);
uint32_t fma_fast_hw_s(int variant, uint32_t x, uint32_t y, uint32_t z, uint32_t csr, uint32_t *flags);

#endif
