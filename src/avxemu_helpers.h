#ifndef AVXEMU_HELPERS_H
#define AVXEMU_HELPERS_H

/*
 * libavxemu_helpers.a: the emulation core (exec.c, exec_bmi.c, softfma.c) without decoder,
 * register file, memory access or fault handling. Link it, include this header, and give it
 * operands by value or pointer. Needs only memcpy/memset/memmove; AVX1 code, no libc state.
 *
 * Contract, for every function:
 *   - It touches no memory except its arguments, and it never faults.
 *   - Return 1: handled, results written. Return 0: op not implemented here, nothing written.
 *
 * vec_exec (vex_op and operand order as in vexops.h; FMA ops included)
 *   - *mxcsr is the guest MXCSR, passed in and out; it must be non-NULL. Rounding mode, FTZ, DAZ
 *     and the exception masks are read from it; the exception flags an op raises (bits 0-5) are
 *     ORed into it. Only FMA and VCVTPH2PS read or write it; every other op leaves it alone.
 *   - An unmasked exception returns AVXEMU_FP_PENDING (negative): `out` is not written, and
 *     *mxcsr holds the input with the raised flags ORed in. The caller raises the #XM, picking
 *     the exception from the raised flags that its masks (bits 7-12) leave unmasked; to isolate
 *     this op's flags, pass an MXCSR with bits 0-5 clear and restore the old flags afterwards.
 *     Linux takes the SIGFPE's si_code from MXCSR alone (flags & ~masks: IE, then ZE, OE, UE or DE,
 *     PE), so ORing the raised flags into the live MXCSR and then executing any SSE op that raises
 *     one of the unmasked ones gives the native si_code at that op's pc; what else that op raises
 *     stays in MXCSR. emulate.c's fp_fault and v3compat's client do this.
 *   - gpr_out is written only by VPMOVMSKB.
 *
 * bmi_exec
 *   - No MXCSR. src1/src2 follow each op's Intel operand order; opsize is 16, 32 or 64.
 *   - *dst gets the result, zero-extended to 64 bits (MULX also writes *dst2, the high half). A
 *     16-bit op writes only the destination's low word on hardware: the caller merges it.
 *   - *flags is RFLAGS in and out. ANDN, BLSI, BLSR, BLSMSK, BZHI, BEXTR, TZCNT and LZCNT replace
 *     CF (bit 0), ZF (6), SF (7) and OF (11) and keep every other bit; no op touches PF or AF.
 *     All other ops leave *flags unchanged, so the caller may always store it back.
 */

#include "vexops.h"

#endif /* AVXEMU_HELPERS_H */
