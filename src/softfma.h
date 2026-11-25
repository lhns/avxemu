#ifndef AVXEMU_SOFTFMA_H
#define AVXEMU_SOFTFMA_H

#include <stdint.h>
#include "vexops.h"

/* IEEE half -> float (exact; no rounding needed). For F16C VCVTPH2PS. */
float half_to_float(uint16_t h);

/* OR exception flags (MXCSR bit order) into *mxcsr. Returns 0 if one is unmasked there: the
 * caller must then write no result and report AVXEMU_FP_PENDING. Never faults. */
int fp_raise(uint32_t *mxcsr, uint32_t flags);

/*
 * FMA executor: out = fma-variant(a, b, c) per element.
 * `type` is FT_SS/FT_SD/FT_PS/FT_PD. For scalar types the upper lanes follow
 * Intel's rule (preserve c's upper bits, zero 255:128). Exact integer arithmetic: rounding
 * mode, FTZ, DAZ and exception masks come from *mxcsr, raised flags are ORed into it, and an
 * unmasked one returns AVXEMU_FP_PENDING with `out` unwritten. One exception: with every mask set
 * and round to nearest in both *mxcsr and the live MXCSR, a lane may be computed with native SSE
 * arithmetic when that is provably the correctly rounded result. The function then reads the live
 * MXCSR and writes it back unchanged, so the native operations' flags never reach the caller.
 */
int fma_exec(vex_op op, int type,
             const ymm256 *a, const ymm256 *b, const ymm256 *c, ymm256 *out,
             uint32_t *mxcsr);

#endif /* AVXEMU_SOFTFMA_H */
