#ifndef AVXEMU_FMA_PENDING_HW_H
#define AVXEMU_FMA_PENDING_HW_H
#include "ymm.h"
#include <stdint.h>

/* vfmadd231ps (dbl 0) or vfmadd231pd (dbl 1): out = a*b+c under csr. Returns the MXCSR after it if it
 * completes; an unmasked exception raises SIGFPE and does not return. The caller's MXCSR is restored. */
uint32_t fma_pending_hw(int dbl, const ymm256 *a, const ymm256 *b, const ymm256 *c, ymm256 *out, uint32_t csr);

#endif
