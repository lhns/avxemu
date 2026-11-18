/*
 * fma_pending.c -- the MXCSR flags at an unmasked FMA exception: the helper's against this CPU's.
 * Silicon stops at an unmasked IE or DE before it computes, so the post-computation flags (OE, UE,
 * PE) of other lanes may be absent from the MXCSR the SIGFPE handler sees; the helper ORs every
 * lane's flags. Each case runs vfmadd231ps/pd on the CPU, reads MXCSR from the signal frame, and runs
 * the same operands through vec_exec. Needs FMA; test/fma_pending_hw.c holds the instruction.
 */
#define _GNU_SOURCE
#include "vexops.h"
#include "cpu.h"
#include "fma_pending_hw.h"
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>

static sigjmp_buf jump;
static volatile uint32_t fault_csr;
static void on_fpe(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si;
    fault_csr = ((ucontext_t *)uc)->uc_mcontext.fpregs->mxcsr;
    siglongjmp(jump, 1);
}

enum { IE = 1, DE = 2, ZE = 4, OE = 8, UE = 16, PE = 32 };
#define NAN_S 0x7fa00000u   /* signalling NaN, float */
#define NAN_D 0x7ff4000000000000ull
#define ONE_S 0x3f800000u
#define BIG_S 0x7f000000u   /* 2^127: its square overflows */
#define TINY_S 0x1e000000u  /* 2^-66: its square underflows */
#define DEN_S 1u
#define ONE_D 0x3ff0000000000000ull
#define BIG_D 0x7fe0000000000000ull
#define DEN_D 1ull

typedef struct { const char *name; int dbl; uint32_t unmask; uint64_t a[8], b[8], c[8]; } fcase;

/* Lanes not listed compute 1*1+1 (exact). */
static const fcase cases[] = {
    {"ps IE lane0, OE lane1",       0, IE, {NAN_S, BIG_S}, {ONE_S, BIG_S}, {0}},
    {"ps IE lane7, OE lane0",       0, IE, {BIG_S, 0, 0, 0, 0, 0, 0, NAN_S}, {BIG_S, 0, 0, 0, 0, 0, 0, ONE_S}, {0}},
    {"ps IE lane0, UE lane1",       0, IE, {NAN_S, TINY_S}, {ONE_S, TINY_S}, {0}},
    {"ps IE lane3, PE lane0",       0, IE, {0x3f800001u, 0, 0, NAN_S}, {0x3f800001u, 0, 0, ONE_S}, {0}},
    {"ps DE lane0, OE lane1",       0, DE, {DEN_S, BIG_S}, {ONE_S, BIG_S}, {0}},
    {"ps DE lane5, UE lane0",       0, DE, {TINY_S, 0, 0, 0, 0, DEN_S}, {TINY_S, 0, 0, 0, 0, ONE_S}, {0}},
    {"ps OE lane0, PE lane1",       0, OE, {BIG_S, 0x3f800001u}, {BIG_S, 0x3f800001u}, {0}},
    {"ps IE+DE, SNaN l0, denormal l1", 0, IE | DE, {NAN_S, DEN_S}, {ONE_S, ONE_S}, {0}},
    {"ps IE only, SNaN l0, denormal l1", 0, IE, {NAN_S, DEN_S}, {ONE_S, ONE_S}, {0}},
    {"ps DE only, denormal l0, SNaN l1", 0, DE, {DEN_S, NAN_S}, {ONE_S, ONE_S}, {0}},
    {"pd IE lane0, OE lane1",       1, IE, {NAN_D, BIG_D}, {ONE_D, BIG_D}, {0}},
    {"pd DE lane0, OE lane1",       1, DE, {DEN_D, BIG_D}, {ONE_D, BIG_D}, {0}},
};

static void fill(ymm256 *y, int dbl, const uint64_t *v, uint64_t one) {
    for (int i = 0; i < (dbl ? 4 : 8); i++) {
        uint64_t x = v[i] ? v[i] : one;     /* an unlisted (zero) lane is 1.0 */
        if (dbl) memcpy(y->b + 8 * i, &x, 8); else { uint32_t s = (uint32_t)x; memcpy(y->b + 4 * i, &s, 4); }
    }
}

int main(void) {
    if (!(cpu_features() & CPU_FMA)) { printf("SKIP: no FMA on this CPU\n"); return 77; }
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fpe; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigemptyset(&sa.sa_mask);
    if (sigaction(SIGFPE, &sa, 0)) return 1;
    int bad = 0;
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        const fcase *t = &cases[k];
        ymm256 a, b, c, out, ref;
        uint64_t one = t->dbl ? ONE_D : ONE_S;
        fill(&a, t->dbl, t->a, one); fill(&b, t->dbl, t->b, one); fill(&c, t->dbl, t->c, one);
        uint32_t csr = 0x1F80u & ~((uint32_t)t->unmask << 7);
        uint32_t hw;
        if (sigsetjmp(jump, 1) == 0) {
            fma_pending_hw(t->dbl, &a, &b, &c, &out, csr);
            printf("%-34s  no exception on this CPU\n", t->name); bad++; continue;
        }
        hw = fault_csr & 0x3Fu;
        uint32_t mx = csr, gpr = 0;
        uint64_t g = gpr;
        int r = vec_exec(VFMADD231, t->dbl ? FT_PD : FT_PS, &a, &b, &c, 0, &ref, &g, &mx);
        uint32_t em = mx & 0x3Fu;
        int ok = r == AVXEMU_FP_PENDING && em == hw;
        printf("%-34s  cpu 0x%02x  helper 0x%02x  %s\n", t->name, hw, em, ok ? "ok" : "DIFFERS");
        bad += !ok;
    }
    return bad ? 1 : 0;
}
