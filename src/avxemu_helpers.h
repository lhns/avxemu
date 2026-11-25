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
 *   - VEX.L is not an input: every op reads all 32 bytes of its operands and writes all 32 of `out`.
 *     For a 128-bit form the caller zeroes the unused source lanes (the upper 16 bytes of a, b and c;
 *     for VCVTPH2PS bytes 8-31 of b), commits only the low 16 bytes of `out` and zeroes the
 *     destination's upper half; for VPMOVMSKB it masks gpr_out to 16 bits. It matters most for
 *     the ops that raise MXCSR flags (FMA, VCVTPH2PS): stale lanes would compute and raise flags of
 *     their own. emulate.c does all of this around the call.
 *   - *mxcsr is the guest MXCSR, passed in and out; it must be non-NULL. Rounding mode, FTZ, DAZ
 *     and the exception masks are read from it; the exception flags an op raises (bits 0-5) are
 *     ORed into it. Only FMA and VCVTPH2PS read or write it; every other op leaves it alone.
 *   - FMA ops are exact integer arithmetic, with one exception: a packed form (not a scalar one) with every
 *     exception masked and round to nearest in *mxcsr and in the live MXCSR is first computed by native SSE
 *     arithmetic, and that result is used for each lane where it provably is the correctly rounded one (the other lanes take the integer path)
 *     (softfma.c). Such a call reads the live MXCSR and restores it, so no flag the native operations set
 *     reaches the caller; PE is computed and ORed into *mxcsr as for any other lane. FTZ and DAZ do not
 *     matter. Under any other live MXCSR no native operation runs.
 *   - An unmasked exception returns AVXEMU_FP_PENDING (negative): `out` is not written, and
 *     *mxcsr holds the input with the raised flags ORed in. The caller raises the #XM, picking
 *     the exception from the raised flags that its masks (bits 7-12) leave unmasked; to isolate
 *     this op's flags, pass an MXCSR with bits 0-5 clear and restore the old flags afterwards.
 *     Linux takes the SIGFPE's si_code from MXCSR alone (flags & ~masks: IE, then ZE, OE, UE or DE,
 *     PE), so ORing the raised flags into the live MXCSR and then executing any SSE op that raises
 *     one of the unmasked ones gives the si_code of that MXCSR at that op's pc. emulate.c's fp_fault
 *     and v3compat's client do this. As on silicon, an unmasked IE or DE raises only IE and DE
 *     (test/fma_pending.c); the suite does not compare si_code at a fault.
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
