#define _GNU_SOURCE
/*
 * tramp.c — trampoline dispatch, thunk pool, and thunk builder.
 *
 * A "run" is a maximal sequence of consecutive faulting instructions. Each run
 * gets one thunk (tramp.S template + an appended run_record). The patched site
 * jumps to the thunk; the thunk snapshots state into a regfile and calls
 * avxemu_tramp_dispatch(), which emulates each instruction of the run in order.
 */
#include "regfile.h"
#include "decode.h"
#include "vexops.h"
#include "lde.h"
#include "scan.h"
#include "lcache.h"
#include "image.h"
#include "platform/memory.h"
#include "avxemu_cpuid.h"
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>     /* rename() in ophist_dump (else implicit-decl error on clang>=15) */
#include <sys/mman.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>

/* run_record: laid out immediately after the thunk code (see tramp.S). */
typedef struct { uint64_t addr; decoded dec; } tramp_insn;   /* addr feeds rf->rip per instruction */
typedef struct { uint32_t n; uint32_t pad; tramp_insn insns[]; } run_record;

extern char avxemu_tt_start[], avxemu_tt_end[];
extern char avxemu_tt_dispatchptr[], avxemu_tt_resumeptr[], avxemu_tt_record[];
/* GPR-only thunk template: saves only xmm0-15 (128-bit) instead of all ymm. */
extern char avxemu_ttg_start[], avxemu_ttg_end[];
extern char avxemu_ttg_dispatchptr[], avxemu_ttg_resumeptr[], avxemu_ttg_record[];
/* BMI reg-only thunk template: saves GPRs+flags only, no vector save at all. */
extern char avxemu_tt2_start[], avxemu_tt2_end[];
extern char avxemu_tt2_dispatchptr[], avxemu_tt2_resumeptr[], avxemu_tt2_record[];

static void emit(const char *s){ (void)!write(2, s, strlen(s)); }

/* ============================================================================
 * Milestone-B DIAGNOSTIC (env-gated; harmless when unset): execution-weighted
 * histogram of which ops actually go through C emulation (i.e. are NOT native-
 * codegen'd) during a run. AVXEMU_OPHIST=1 turns it on; dump on SIGUSR2 (so a
 * live spinning process can be probed via `kill -USR2 <pid>`) and atexit. The
 * dump goes to /tmp/ophist.out (parent-readable; the child's stderr is on a pty).
 * ========================================================================== */
static uint64_t g_ophist[VEX_OP_COUNT];
static int g_ophist_on = -1;
static int ophist_enabled(void){
    if (g_ophist_on < 0){ const char *e = getenv("AVXEMU_OPHIST"); g_ophist_on = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return g_ophist_on;
}
/* async-signal-safe unsigned decimal writer */
static void as_u64(int fd, uint64_t v){
    char b[24]; int i = 24;
    if (v == 0){ (void)!write(fd, "0", 1); return; }
    while (v){ b[--i] = (char)('0' + (v % 10)); v /= 10; }
    (void)!write(fd, b + i, (size_t)(24 - i));
}
/* async-signal-safe: open/write/close + strlen on static strings only, no malloc */
static void ophist_dump_fd(int fd){
    int idx[VEX_OP_COUNT]; int m = 0;
    for (int i = 0; i < VEX_OP_COUNT; i++) if (g_ophist[i]) idx[m++] = i;
    for (int i = 0; i < m; i++){ int best = i;
        for (int j = i + 1; j < m; j++) if (g_ophist[idx[j]] > g_ophist[idx[best]]) best = j;
        int t = idx[i]; idx[i] = idx[best]; idx[best] = t; }
    static const char hdr[] = "=== AVXEMU_OPHIST: C-emulated op histogram (execution-weighted, desc) ===\n";
    (void)!write(fd, hdr, sizeof hdr - 1);
    uint64_t total = 0;
    for (int i = 0; i < m; i++){
        const char *nm = vex_op_name((vex_op)idx[i]);
        (void)!write(fd, nm, strlen(nm)); (void)!write(fd, "\t", 1);
        as_u64(fd, g_ophist[idx[i]]); (void)!write(fd, "\n", 1);
        total += g_ophist[idx[i]];
    }
    (void)!write(fd, "TOTAL\t", 6); as_u64(fd, total); (void)!write(fd, "\n", 1);
}
/* atomic: write to a temp path then rename, so /tmp/ophist.out is never a partial
 * file even if a kill -9 lands mid-dump (rename is atomic). */
static void ophist_dump(void){
    int fd = open("/tmp/ophist.out.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { ophist_dump_fd(2); return; }
    ophist_dump_fd(fd);
    close(fd);
    (void)rename("/tmp/ophist.out.tmp", "/tmp/ophist.out");
}
static void ophist_sigusr2(int sig){ (void)sig; ophist_dump(); }
/* Robust fallback: the host (Bun/JSC) may reinstall its own SIGUSR2 handler and a
 * kill -9 skips atexit, so also snapshot to /tmp/ophist.out periodically from the
 * hot path. O_TRUNC means the file always holds a recent cumulative snapshot. */
static uint64_t g_ophist_total;
#define OPHIST_SNAP_EVERY (1u << 17)
static inline void ophist_tick(void){
    if (((++g_ophist_total) & (OPHIST_SNAP_EVERY - 1)) == 0) ophist_dump();
}

/* The actual emulation, run on the side stack. */
static void tramp_emulate_run(const void *recordp, void *rfp) {
    const run_record *r = (const run_record *)recordp;
    avxemu_regfile *rf = (avxemu_regfile *)rfp;
    for (uint32_t i = 0; i < r->n; i++) {
        rf->rip = r->insns[i].addr;
        if (g_ophist_on) { g_ophist[r->insns[i].dec.op]++; ophist_tick(); }   /* AVXEMU_OPHIST diagnostic */
        if (!avxemu_emulate(&r->insns[i].dec, rf)) {
            /* Scanner only trampolines ops we emulate, so this is a bug net. */
            emit("avxemu: tramp dispatch: emulate failed mid-run\n");
            return;
        }
    }
}

/*
 * Per-thread side stack for the emulation. A trampolined instruction can fire at
 * any JS recursion depth; running the (multi-KB) emulation on the program stack
 * would add pressure exactly where JSC is near its limit. Moving it to a private
 * stack means a thunk costs only its tiny register-spill frame on the program
 * stack, never the emulation itself.
 */
#define SIDE_SZ (256u * 1024)
extern void avxemu_run_on_stack(uint8_t *base, size_t sz,
                                void (*fn)(const void *, void *), const void *a, void *b);

/* Per-thread side-stack pointer. pthread TSD (not __thread): getspecific is in
 * the baseline system libs, so it is never trampolined and never mallocs — safe
 * to call from a thunk even one that fired inside Bun's lzcnt-using allocator. */
static pthread_key_t g_side_key;
static int g_side_key_ok = 0;

/* Called from the thunk (tramp.S) with the live state spilled into rf. */
void avxemu_tramp_dispatch(const run_record *r, avxemu_regfile *rf) {
    if (!g_side_key_ok) { tramp_emulate_run(r, rf); return; }   /* no key: program stack */
    uint8_t *side = (uint8_t *)pthread_getspecific(g_side_key);
    if (!side) {
        void *m = mmap(0, SIDE_SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) { tramp_emulate_run(r, rf); return; }
        side = (uint8_t *)m;
        pthread_setspecific(g_side_key, side);
    }
    avxemu_run_on_stack(side, SIDE_SZ, tramp_emulate_run, r, rf);
}

/* xmm-CLEAN dispatch for register-only BMI runs. Replicates avxemu_emulate's
 * is_bmi path but with NO memory operands (so no mem_read/memcpy) and calls
 * bmi_exec directly -- the whole chain uses only general registers, so it cannot
 * clobber the program's xmm/ymm. That is what lets the tt2 thunk skip the vector
 * save entirely. (A build-time `otool | grep xmm` over this + bmi_exec guards the
 * invariant; the classifier never routes a memory/seg op here.) */
static int emulate_bmi_reg(const decoded *d, avxemu_regfile *rf) {
    uint64_t s1 = 0, s2 = 0, dst = 0, dst2 = 0, flags = rf->rflags;
    if (d->a_src >= 0) s1 = rf->gpr[d->a_src];
    if (d->bmi_s1_rdx) s1 = rf->gpr[2];
    if (d->op == BMI_RORX)  s2 = d->imm;
    else if (d->b_src >= 0) s2 = rf->gpr[d->b_src];
    if (!bmi_exec(d->op, d->opsize, s1, s2, &dst, &dst2, &flags)) return 0;
    /* 16-bit ops (66-prefixed lzcnt/tzcnt/movbe) write only the low word of the
     * destination register on real hardware; 32/64-bit writes zero-extend. */
    if (d->dst >= 0)      rf->gpr[d->dst] = (d->opsize == 16)
                              ? ((rf->gpr[d->dst] & ~0xFFFFull) | (dst & 0xFFFFull))
                              : dst;
    if (d->bmi_dst2 >= 0) rf->gpr[d->bmi_dst2] = dst2;
    rf->rflags = flags;
    return 1;
}
void avxemu_tramp_dispatch_bmi(const run_record *r, avxemu_regfile *rf) {
    for (uint32_t i = 0; i < r->n; i++) {
        rf->rip = r->insns[i].addr;
        if (g_ophist_on) { g_ophist[r->insns[i].dec.op]++; ophist_tick(); }   /* AVXEMU_OPHIST diagnostic */
        if (!emulate_bmi_reg(&r->insns[i].dec, rf)) {
            emit("avxemu: tramp dispatch (bmi): emulate failed mid-run\n");
            return;
        }
    }
}

/* ---- thunk pool: RWX, bump-allocated. The patcher reaches it with jmp rel32,
 * so it must land within +/-2GB of __text; avxemu_pool_init() takes a hint. ---- */
static uint8_t *g_pool; static size_t g_used, g_cap;

int avxemu_pool_init(void *hint, size_t cap) {
    uintptr_t near = (uintptr_t)hint;
    if (!near) {
        size_t page = avxemu_page_size();
        if (!page) return 0;
        near = ((uintptr_t)avxemu_pool_init + 0x100000) / page * page;
    }
    void *p = avxemu_memory_alloc_near(near, cap);
    if (!p) return 0;
    g_pool = (uint8_t *)p; g_cap = cap; g_used = 0;
    return 1;
}
void *avxemu_pool_base(void){ return g_pool; }
size_t avxemu_pool_used(void){ return g_used; }

/* Raw 16-byte-aligned bump allocation from the shared pool. This is the SINGLE
 * cursor (g_used) over g_pool: the eager trampoliner (build_thunk_t below) and the
 * runtime relocator (reloc.c) both allocate through it, so the two can never hand
 * out overlapping bytes of the live pool. Returns NULL if uninitialized/exhausted. */
void *avxemu_pool_alloc(size_t n) {
    if (n > SIZE_MAX - 15) return 0;
    n = (n + 15) & ~(size_t)15;
    if (!g_pool || n > g_cap - g_used) return 0;
    uint8_t *p = g_pool + g_used; g_used += n;
    return p;
}

/*
 * Build a thunk for a run of n decoded instructions (each with its original
 * address) that resumes at `resume`. Returns the thunk entry, or NULL if the
 * pool is exhausted. Only data slots are written; the template code is verbatim.
 */
static void *build_thunk_t(const tramp_insn *insns, int n, uint64_t resume,
                           const char *tstart, const char *trecord,
                           const char *tdispatch, const char *tresume,
                           void *dispatch_fn) {
    size_t code  = (size_t)(trecord - tstart);                     /* code + 2 ptr slots */
    size_t recsz = sizeof(run_record) + (size_t)n * sizeof(tramp_insn);
    uint8_t *t = avxemu_pool_alloc(code + recsz);                   /* shared single cursor */
    if (!t) return 0;
    memcpy(t, tstart, code);
    *(void   **)(t + (tdispatch - tstart)) = dispatch_fn;
    *(uint64_t *)(t + (tresume   - tstart)) = resume;

    run_record *r = (run_record *)(t + (trecord - tstart));
    r->n = (uint32_t)n; r->pad = 0;
    for (int i = 0; i < n; i++) r->insns[i] = insns[i];
    return t;
}

/* Full thunk: saves all 16 ymm (256-bit). Used for any run with a vector op. */
void *avxemu_build_thunk(const tramp_insn *insns, int n, uint64_t resume) {
    return build_thunk_t(insns, n, resume, avxemu_tt_start, avxemu_tt_record,
                         avxemu_tt_dispatchptr, avxemu_tt_resumeptr,
                         (void *)avxemu_tramp_dispatch);
}

/* GPR-only thunk: saves only xmm0-15 (128-bit). SAFE only for runs whose every
 * instruction is a GPR-domain op (is_bmi: BMI1/2, LZCNT, TZCNT, MOVBE) — those
 * touch no vector register, so the emulation neither reads nor writes ymm and the
 * upper 128 bits are left untouched by the SSE-only C path. Halves the per-run
 * vector spill, the dominant trampoline cost. */
static void *build_thunk_gpr(const tramp_insn *insns, int n, uint64_t resume) {
    return build_thunk_t(insns, n, resume, avxemu_ttg_start, avxemu_ttg_record,
                         avxemu_ttg_dispatchptr, avxemu_ttg_resumeptr,
                         (void *)avxemu_tramp_dispatch);
}

/* BMI reg-only thunk: saves GPRs+flags only, no vector save. Dispatched through
 * the xmm-clean avxemu_tramp_dispatch_bmi, so the program's vector state is
 * untouched and needs no preservation. */
static void *build_thunk_bmi(const tramp_insn *insns, int n, uint64_t resume) {
    return build_thunk_t(insns, n, resume, avxemu_tt2_start, avxemu_tt2_record,
                         avxemu_tt2_dispatchptr, avxemu_tt2_resumeptr,
                         (void *)avxemu_tramp_dispatch_bmi);
}

/* True iff every instruction in the run is a GPR-domain op (no vector reg). */
static int run_is_gpr_only(const tramp_insn *insns, int n) {
    for (int i = 0; i < n; i++) if (!insns[i].dec.is_bmi) return 0;
    return n > 0;
}
/* Stricter: every insn is a register-operand BMI op (no memory, no segment) -> the
 * emulation touches only GPRs, so the no-vector-save tt2 thunk is safe. */
static int run_is_regonly_bmi(const tramp_insn *insns, int n) {
    for (int i = 0; i < n; i++) {
        const decoded *d = &insns[i].dec;
        if (!d->is_bmi || d->a_src == OPND_MEM || d->b_src == OPND_MEM
            || d->dst_kind == DST_MEM || d->seg != 0) return 0;
    }
    return n > 0;
}

/* ----------------------------------------------------------------------------
 * Eager installer: at load, rewrite each run of faulting instructions to a jmp
 * into a thunk, so it never traps. Runs single-threaded in the constructor,
 * before the patched code can execute.
 * -------------------------------------------------------------------------- */

static int g_lack_avx2 = 1, g_lack_fma = 1, g_lack_bmi = 1, g_lack_f16c = 0;
static int g_force_full = 0;   /* AVXEMU_FULLTHUNK: force the register-saving thunk (correctness probe) */

static int avx2_only_op(vex_op op) {
    switch (op) {
    case VPSLLVD: case VPSLLVQ: case VPSRLVD: case VPSRLVQ: case VPSRAVD:
    case VPBROADCASTB: case VPBROADCASTW: case VPBROADCASTD: case VPBROADCASTQ:
    case VBROADCASTI128: case VPBLENDD:
    case VEXTRACTI128: case VINSERTI128: case VPERM2I128:
    case VPERMQ: case VPERMD: case VPERMPD: case VPERMPS: return 1;
    default: return 0;
    }
}
/* Does the instruction #UD on this CPU (and so currently go through SIGILL)? */
static int tramp_faults(const decoded *d) {
    if (d->is_bmi) return g_lack_bmi;
    if (d->op >= VFMADD132 && d->op <= VFNMSUB231) return g_lack_fma;
    if (d->op == VCVTPH2PS) return g_lack_f16c;
    if (avx2_only_op(d->op)) return g_lack_avx2;
    return d->wide && g_lack_avx2;        /* base SSE integer op: only 256-bit faults */
}

static void detect_features(void) {
    g_force_full = getenv("AVXEMU_FULLTHUNK") != 0;
    if (getenv("AVXEMU_FORCETRAMP")) {    /* dev: treat everything emulatable as faulting */
        g_lack_avx2 = g_lack_fma = g_lack_bmi = g_lack_f16c = 1; return;
    }
    uint32_t r1[4], r7[4] = {0}, r8[4];
    if (avxemu_get_cpuid(1, 0, r1)) { g_lack_fma = !(r1[2] & (1u<<12)); g_lack_f16c = !(r1[2] & (1u<<29)); }
    (void)avxemu_get_cpuid(7, 0, r7);
    g_lack_avx2 = !(r7[1] & (1u<<5));
    int bmi1 = (r7[1] & (1u<<3)) != 0, bmi2 = (r7[1] & (1u<<8)) != 0;
    int lz = avxemu_get_cpuid(0x80000001u, 0, r8) && (r8[2] & (1u<<5));
    g_lack_bmi = !(bmi1 && bmi2 && lz);   /* conservative: any BMI-ish missing -> emulate the family */
}

#define MAXRUN 64

/* ============================================================================
 * Milestone B: register-resident native-SSE codegen.
 *
 * For a run whose every instruction is one of a small set of dominant AVX2
 * vector ops, instead of routing through avxemu_tramp_dispatch (a C call that
 * loops avxemu_emulate per instruction), we EMIT a native straight-line SSE
 * function `block(const run_record *r, avxemu_regfile *rf)` (same signature the
 * tt template calls: rdi=run_record (unused here), rsi=&rf). The block reads
 * each op's source operands out of the in-memory regfile (rf), computes the op
 * with native SSE on the two 128-bit halves, and writes the result back into
 * rf. The tt template's spill/reload/resume is unchanged — only the dispatch
 * pointer is swapped from avxemu_tramp_dispatch to this emitted block.
 *
 * CORRECTNESS MODEL (write-through): every instruction's result is stored back
 * to its rf ymm slot immediately, so the in-memory rf is the canonical register
 * file and is up to date at every instruction boundary. Data dependencies
 * within the run therefore resolve through rf automatically. As a register-
 * residency optimisation for the dominant straight-line dependency chain, the
 * LOW 128 bits of the immediately-preceding destination are kept live in an xmm
 * (NX_CACHE) and reused when the next instruction's source slot matches — this
 * is a pure copy of what was just written to rf, so it can never be stale. The
 * HIGH 128 bits are always reloaded from rf (cheap, and sidesteps any stale-
 * upper-half pitfall). The block clobbers only caller-saved GPRs (rax/rcx) and
 * xmm; the tt template reloads ALL of rf into the real registers after the call,
 * so any clobber is irrelevant. rsi (rf) and rbx/rsp/rbp/r12-15 are preserved.
 *
 * This file stays VEX-CLEAN: we only WRITE machine-code bytes (data) for the SSE
 * (and VEX-encoded source, never compiled) instructions; the compiled C emits no
 * VEX. The emitted block uses only SSE2/SSE4.1 (pand/por/pxor/psubb/pcmpeqb/
 * pcmpgtb/pshufd/movd/pmovzxbw/psrldq), all present on the target Ivy Bridge.
 *
 * Op set: VPBROADCASTD, VPAND, VPOR, VPXOR, VPSUBB, VPCMPEQB, VPCMPGTB,
 * VPMOVZXBW (each in register and memory-operand form). Any op outside the set,
 * a segment override (seg!=0), or a memory destination -> decline the WHOLE run
 * (avxemu_build_thunk_native returns NULL; caller falls back to C dispatch).
 * Declining is never a correctness risk.
 * ========================================================================== */
#define RF_GPR_OFF 512   /* byte offset of gpr[0] within avxemu_regfile (16*32) */
/* xmm register roles inside the emitted block (NX_ZERO is a pinned zero reg) */
enum { NX_A_LO = 0, NX_A_HI = 1, NX_B_LO = 2, NX_B_HI = 3, NX_TMP = 4,
       NX_CACHE = 6, NX_ZERO = 15 };
#define NB_RSI 6   /* rf base pointer register (arg2) */
#define NB_RAX 0   /* EA accumulator */
#define NB_RCX 1   /* index scratch */

static void nb(uint8_t **p, uint8_t b){ *(*p)++ = b; }
static void nb32(uint8_t **p, uint32_t v){ memcpy(*p, &v, 4); *p += 4; }
static void nb64(uint8_t **p, uint64_t v){ memcpy(*p, &v, 8); *p += 8; }

/* SSE op  xmm[reg] , xmm[rm]   (ModRM mod=11). pre = mandatory/legacy prefix
 * (0x66/0xF3/0), esc38 = use the 0F38 escape map. */
static void sse_rr(uint8_t **p, uint8_t pre, int esc38, uint8_t op,
                   int reg, int rm, int has_imm, uint8_t imm){
    if (pre) nb(p, pre);
    uint8_t rex = (uint8_t)(((reg >= 8) ? 4 : 0) | ((rm >= 8) ? 1 : 0));
    if (rex) nb(p, (uint8_t)(0x40 | rex));
    nb(p, 0x0F); if (esc38) nb(p, 0x38); nb(p, op);
    nb(p, (uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
    if (has_imm) nb(p, imm);
}
/* SSE op  xmm[reg] , [base + disp32]   (ModRM mod=10). base must not be rsp/r12
 * (would need a SIB) — all callers use rsi or rax/rcx, never reg 4. */
static void sse_rm(uint8_t **p, uint8_t pre, int esc38, uint8_t op,
                   int reg, int base, int32_t disp, int has_imm, uint8_t imm){
    if (pre) nb(p, pre);
    uint8_t rex = (uint8_t)(((reg >= 8) ? 4 : 0) | ((base >= 8) ? 1 : 0));
    if (rex) nb(p, (uint8_t)(0x40 | rex));
    nb(p, 0x0F); if (esc38) nb(p, 0x38); nb(p, op);
    nb(p, (uint8_t)(0x80 | ((reg & 7) << 3) | (base & 7)));
    nb32(p, (uint32_t)disp);
    if (has_imm) nb(p, imm);
}
static void nb_movdqu_load (uint8_t **p, int xmm, int base, int32_t d){ sse_rm(p, 0xF3, 0, 0x6F, xmm, base, d, 0, 0); }
static void nb_movdqu_store(uint8_t **p, int xmm, int base, int32_t d){ sse_rm(p, 0xF3, 0, 0x7F, xmm, base, d, 0, 0); }
static void nb_movdqa      (uint8_t **p, int dst, int src){ sse_rr(p, 0x66, 0, 0x6F, dst, src, 0, 0); }

/* GPR EA helpers. All operate at 64-bit (REX.W). The memory base for the loads
 * is always rsi (rf), encoded with mod=10 disp32. */
static void nb_movabs_rax (uint8_t **p, uint64_t v){ nb(p, 0x48); nb(p, 0xB8); nb64(p, v); }
static void nb_add_rax_mem(uint8_t **p, int32_t d){ nb(p, 0x48); nb(p, 0x03); nb(p, (uint8_t)(0x80 | (0 << 3) | NB_RSI)); nb32(p, (uint32_t)d); }
static void nb_mov_rcx_mem(uint8_t **p, int32_t d){ nb(p, 0x48); nb(p, 0x8B); nb(p, (uint8_t)(0x80 | (1 << 3) | NB_RSI)); nb32(p, (uint32_t)d); }
static void nb_shl_rcx    (uint8_t **p, uint8_t imm){ nb(p, 0x48); nb(p, 0xC1); nb(p, 0xE1); nb(p, imm); }
static void nb_add_rax_rcx(uint8_t **p){ nb(p, 0x48); nb(p, 0x01); nb(p, 0xC8); }

/* Compute the memory operand's effective address into rax, exactly mirroring
 * ea_rf(): EA = disp + (rip_rel ? insn_addr+len : 0) + gpr[base] + gpr[index]*scale.
 * Both the disp and the rip-relative term are known at emit time (insn_addr is
 * the instruction's runtime address, recorded in the run_record), so they fold
 * into a single 64-bit immediate. base/index are read live from rf->gpr. */
static void emit_ea(uint8_t **p, const decoded *d, uint64_t addr){
    uint64_t rip_next = addr + d->len;
    int64_t c = (int64_t)d->disp + (d->rip_rel ? (int64_t)rip_next : 0);
    nb_movabs_rax(p, (uint64_t)c);
    if (d->base != OPND_NONE)  nb_add_rax_mem(p, RF_GPR_OFF + (int)d->base * 8);
    if (d->index != OPND_NONE){
        nb_mov_rcx_mem(p, RF_GPR_OFF + (int)d->index * 8);
        uint8_t sh = (d->scale == 8) ? 3 : (d->scale == 4) ? 2 : (d->scale == 2) ? 1 : 0;
        if (sh) nb_shl_rcx(p, sh);
        nb_add_rax_rcx(p);
    }
    if (d->addr32){ nb(p, 0x89); nb(p, 0xC0); }     /* mov eax,eax: wrap at 32 bits */
}

static int native_op_supported(vex_op op){
    switch (op){
    case VPBROADCASTD: case VPBROADCASTW: case VPAND: case VPOR: case VPXOR:
    case VPSUBB: case VPCMPEQB: case VPCMPGTB: case VPMOVZXBW: return 1;
    default: return 0;
    }
}
static int native_is_binop(vex_op op){
    switch (op){
    case VPAND: case VPOR: case VPXOR: case VPSUBB: case VPCMPEQB: case VPCMPGTB: return 1;
    default: return 0;
    }
}
static int native_insn_supported(const decoded *d){
    if (!native_op_supported(d->op)) return 0;
    if (d->seg != 0) return 0;                 /* gs/fs: not supported -> decline run */
    if (d->dst_kind != DST_YMM) return 0;      /* memory dest not implemented -> decline */
    if (d->dst < 0 || d->dst > 15) return 0;
    if (native_is_binop(d->op)){               /* src1 (VEX.vvvv) is always a register */
        if (d->a_src < 0 || d->a_src > 15) return 0;
    }
    if (d->b_src == OPND_MEM){
        if (d->base  != OPND_NONE && (d->base  < 0 || d->base  > 15)) return 0;
        if (d->index != OPND_NONE && (d->index < 0 || d->index > 15)) return 0;
    } else if (d->b_src < 0 || d->b_src > 15) return 0;
    return 1;
}

/* Load register-slot's low (and, if wide, high) 128 bits into lo_reg/hi_reg.
 * Low half reuses NX_CACHE when the slot is the previous destination. */
static void load_slot(uint8_t **p, int slot, int lo_reg, int hi_reg, int wide, int prev_dst){
    if (slot == prev_dst) nb_movdqa(p, lo_reg, NX_CACHE);
    else                  nb_movdqu_load(p, lo_reg, NB_RSI, slot * 32);
    if (wide)             nb_movdqu_load(p, hi_reg, NB_RSI, slot * 32 + 16);
}
/* Store result to dst slot: low always; high if wide, else zero the upper 128
 * (VEX.128 semantics — matches avxemu_emulate's memset of the upper half). */
static void store_dst(uint8_t **p, int slot, int lo_reg, int hi_reg, int wide){
    nb_movdqu_store(p, lo_reg, NB_RSI, slot * 32);
    if (wide) nb_movdqu_store(p, hi_reg, NB_RSI, slot * 32 + 16);
    else      nb_movdqu_store(p, NX_ZERO, NB_RSI, slot * 32 + 16);
}

/* Emit one instruction. EA (if any) is computed into rax first. Returns 1 on
 * success, 0 if (defensively) unhandled — caller then declines the run. */
static int emit_one(uint8_t **p, const decoded *d, uint64_t addr, int *prev_dst){
    int wide = d->wide;
    int D = d->dst;
    int mem = (d->b_src == OPND_MEM);
    if (mem) emit_ea(p, d, addr);     /* rax = &memory operand */

    switch (d->op){
    case VPAND: case VPOR: case VPXOR: case VPSUBB: case VPCMPEQB: case VPCMPGTB: {
        uint8_t opc = (d->op == VPAND)    ? 0xDB : (d->op == VPOR)     ? 0xEB :
                      (d->op == VPXOR)    ? 0xEF : (d->op == VPSUBB)   ? 0xF8 :
                      (d->op == VPCMPEQB) ? 0x74 : /* VPCMPGTB */        0x64;
        load_slot(p, d->a_src, NX_A_LO, NX_A_HI, wide, *prev_dst);
        if (mem){
            nb_movdqu_load(p, NX_B_LO, NB_RAX, 0);
            if (wide) nb_movdqu_load(p, NX_B_HI, NB_RAX, 16);
        } else {
            load_slot(p, d->b_src, NX_B_LO, NX_B_HI, wide, *prev_dst);
        }
        sse_rr(p, 0x66, 0, opc, NX_A_LO, NX_B_LO, 0, 0);          /* res.lo = a.lo OP b.lo */
        if (wide) sse_rr(p, 0x66, 0, opc, NX_A_HI, NX_B_HI, 0, 0);
        store_dst(p, D, NX_A_LO, NX_A_HI, wide);
        nb_movdqa(p, NX_CACHE, NX_A_LO); *prev_dst = D;
        break;
    }
    case VPBROADCASTD: {
        /* load the source dword into the low dword, then pshufd $0 -> all lanes */
        if (mem) sse_rm(p, 0x66, 0, 0x6E, NX_A_LO, NB_RAX, 0, 0, 0);          /* movd A_LO,[rax]      */
        else     sse_rm(p, 0x66, 0, 0x6E, NX_A_LO, NB_RSI, d->b_src * 32, 0, 0);/* movd A_LO,[rsi+b*32] */
        sse_rr(p, 0x66, 0, 0x70, NX_A_LO, NX_A_LO, 1, 0x00);                  /* pshufd A_LO,A_LO,0   */
        if (wide) nb_movdqa(p, NX_A_HI, NX_A_LO);
        store_dst(p, D, NX_A_LO, NX_A_HI, wide);
        nb_movdqa(p, NX_CACHE, NX_A_LO); *prev_dst = D;
        break;
    }
    case VPBROADCASTW: {
        /* broadcast word 0 of the source to all 8 word lanes (per 128). For a
         * memory source, read EXACTLY 2 bytes (pinsrw) so we never over-read past
         * the word vpbroadcastw would touch; for a register slot, movd the low
         * dword (its low word is word 0). Then pshuflw $0 replicates word 0 into
         * the low 4 words, and pshufd $0 replicates the low qword across all 4
         * dwords -> all 8 words = word 0. Matches vec_exec's 16x uint16 store. */
        if (mem) sse_rm(p, 0x66, 0, 0xC4, NX_A_LO, NB_RAX, 0, 1, 0x00);        /* pinsrw A_LO,[rax],0  */
        else     sse_rm(p, 0x66, 0, 0x6E, NX_A_LO, NB_RSI, d->b_src * 32, 0, 0);/* movd  A_LO,[rsi+b*32] */
        sse_rr(p, 0xF2, 0, 0x70, NX_A_LO, NX_A_LO, 1, 0x00);                  /* pshuflw A_LO,A_LO,0  */
        sse_rr(p, 0x66, 0, 0x70, NX_A_LO, NX_A_LO, 1, 0x00);                  /* pshufd  A_LO,A_LO,0  */
        if (wide) nb_movdqa(p, NX_A_HI, NX_A_LO);
        store_dst(p, D, NX_A_LO, NX_A_HI, wide);
        nb_movdqa(p, NX_CACHE, NX_A_LO); *prev_dst = D;
        break;
    }
    case VPMOVZXBW: {
        /* zero-extend bytes->words. Low lane <- src bytes 0..7; (wide) high lane
         * <- src bytes 8..15. Matches vec_exec's out[i]=src[i] across 16 words. */
        if (mem){
            sse_rm(p, 0x66, 1, 0x30, NX_A_LO, NB_RAX, 0, 0, 0);              /* pmovzxbw A_LO,[rax]   */
            if (wide) sse_rm(p, 0x66, 1, 0x30, NX_A_HI, NB_RAX, 8, 0, 0);    /* pmovzxbw A_HI,[rax+8] */
        } else {
            if (d->b_src == *prev_dst) nb_movdqa(p, NX_B_LO, NX_CACHE);
            else nb_movdqu_load(p, NX_B_LO, NB_RSI, d->b_src * 32);
            sse_rr(p, 0x66, 1, 0x30, NX_A_LO, NX_B_LO, 0, 0);               /* pmovzxbw A_LO,B_LO    */
            if (wide){
                nb_movdqa(p, NX_TMP, NX_B_LO);
                sse_rr(p, 0x66, 0, 0x73, 3, NX_TMP, 1, 8);                  /* psrldq TMP,8 (/3)     */
                sse_rr(p, 0x66, 1, 0x30, NX_A_HI, NX_TMP, 0, 0);           /* pmovzxbw A_HI,TMP     */
            }
        }
        store_dst(p, D, NX_A_LO, NX_A_HI, wide);
        nb_movdqa(p, NX_CACHE, NX_A_LO); *prev_dst = D;
        break;
    }
    default: return 0;
    }
    return 1;
}

/* AVXEMU_NATIVE gate (default ON; =0 forces the legacy C-dispatch thunks). */
static int g_native = -1;
static int native_enabled(void){
    if (g_native < 0){ const char *e = getenv("AVXEMU_NATIVE"); g_native = (e && e[0] == '0') ? 0 : 1; }
    return g_native;
}

/* ---- AVXEMU_NATIVE_STATS: build-time run-composition tally (env-gated). Counts
 * runs the native emitter would accept vs decline, run-length single vs multi,
 * and for declined runs the first unsupported (blocking) op. Printed at end of
 * install_trampolines. Pure diagnostic; no effect on what gets emitted. */
static int g_nstats = -1;
static int nstats_enabled(void){
    if (g_nstats < 0){ const char *e = getenv("AVXEMU_NATIVE_STATS"); g_nstats = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return g_nstats;
}
static uint64_t g_ns_accept, g_ns_decline, g_ns_single, g_ns_multi, g_ns_placed;
static uint64_t g_ns_block_op[VEX_OP_COUNT];
static void nstats_tally(const tramp_insn *ri, int rn){
    if (rn == 1) g_ns_single++; else g_ns_multi++;
    int blk = -1;
    for (int k = 0; k < rn; k++) if (!native_insn_supported(&ri[k].dec)){ blk = ri[k].dec.op; break; }
    if (blk < 0) g_ns_accept++;
    else { g_ns_decline++; g_ns_block_op[blk]++; }
}

/* Emit the native SSE `block` for a run into the RWX pool. Returns the block
 * entry, or NULL if any instruction is unsupported or the pool is exhausted.
 * Bypasses the AVXEMU_NATIVE gate (raw emitter; the gate is in the thunk
 * builder) so the differential test can always exercise it. */
void *avxemu_emit_native_block(const tramp_insn *insns, int n){
    if (n <= 0) return 0;
    for (int i = 0; i < n; i++) if (!native_insn_supported(&insns[i].dec)) return 0;
    uint8_t buf[MAXRUN * 160];
    uint8_t *p = buf;
    sse_rr(&p, 0x66, 0, 0xEF, NX_ZERO, NX_ZERO, 0, 0);   /* pxor xmm15,xmm15 (zero reg) */
    int prev_dst = -1;
    for (int i = 0; i < n; i++){
        if (!emit_one(&p, &insns[i].dec, insns[i].addr, &prev_dst)) return 0;
        if ((size_t)(p - buf) > sizeof buf - 128) return 0;   /* overflow guard */
    }
    nb(&p, 0xC3);   /* ret */
    size_t sz = (size_t)(p - buf);
    uint8_t *blk = avxemu_pool_alloc(sz);
    if (!blk) return 0;
    memcpy(blk, buf, sz);
    return blk;
}

/* Native-SSE thunk: a tt-template thunk whose dispatch pointer is the emitted
 * native block instead of avxemu_tramp_dispatch. The spill/reload/resume is the
 * unchanged tt template. Returns NULL (caller falls back) if disabled, any op is
 * unsupported, or the pool is exhausted. */
void *avxemu_build_thunk_native(const tramp_insn *insns, int n, uint64_t resume){
    if (!native_enabled()) return 0;
    void *block = avxemu_emit_native_block(insns, n);
    if (!block) return 0;
    return build_thunk_t(insns, n, resume, avxemu_tt_start, avxemu_tt_record,
                         avxemu_tt_dispatchptr, avxemu_tt_resumeptr, block);
}

/* ============================================================================
 * Milestone B (Task 2): scalar-GPR native codegen for the trampoline.
 *
 * Trampolined LZCNT currently runs through the tt2 (GPR+flags spill) thunk ->
 * avxemu_tramp_dispatch_bmi -> per-instruction C (bmi_exec). This emits a native
 * straight-line block(run_record* rdi, avxemu_regfile* rf=rsi) that operates on
 * the SPILLED rf->gpr[] slots (rsi+512+reg*8) and rf->rflags slot (rsi+640) — the
 * exact same memory image the tt2 thunk saved — then returns. The tt2 thunk
 * reloads every GPR + popf's rflags from those slots afterward, so the block may
 * clobber any caller-saved GPR; it only must preserve rsi (its base) and rsp.
 *
 * Host is no-BMI/no-LZCNT Ivy Bridge: the lowering uses ONLY base-ISA GPR ops
 * (bsr + fixups, NEVER lzcnt — which would #UD). This file stays VEX-clean: we
 * only WRITE machine-code bytes (data); the compiled C emits no VEX and (for this
 * GPR-domain block) no SSE either, so it is safe under the no-vector-save tt2.
 *
 * LZCNT semantics, bit-exact to bmi_exec/set_flags:
 *   result = (src==0) ? opsize : (opsize-1 - bsr(src))
 *   CF = (src==0);  ZF = (result==0);  SF = 0;  OF = 0;  PF/AF preserved.
 *   opsize 32 zero-extends the dst slot's full 64 bits.
 * Only the owned flag bits (CF|ZF|SF|OF) of the rf->rflags slot are rewritten
 * (read-modify-write with mask ~0x8C1), preserving PF/AF and every other bit.
 * ========================================================================== */
#define RF_FLAGS_OFF 640   /* byte offset of rflags within avxemu_regfile */
/* fixed scratch CPU regs for the block (all caller-saved & reloaded by tt2; none
 * is rsi=6, the rf base): S=rax, D=rcx, T1=rdx, T2=r8, F=r9. The decoded dst/src
 * are SLOT indices (memory), independent of these. */
enum { GB_S = 0, GB_D = 1, GB_T1 = 2, GB_T2 = 8, GB_F = 9 };

static void gb_rex(uint8_t **p, int w, int reg, int rm){
    uint8_t r = (uint8_t)((w?8:0) | ((reg>=8)?4:0) | ((rm>=8)?1:0));
    if (r) nb(p, (uint8_t)(0x40 | r));
}
/* mov reg, [rsi+disp32]   (w=1: 64-bit; w=0: 32-bit, zero-extends to 64) */
static void gb_load(uint8_t **p, int reg, int32_t disp, int w){
    gb_rex(p, w, reg, NB_RSI);
    nb(p, 0x8B); nb(p, (uint8_t)(0x80 | ((reg&7)<<3) | NB_RSI)); nb32(p, (uint32_t)disp);
}
/* mov [rsi+disp32], reg   (always 64-bit: writes the full slot) */
static void gb_store(uint8_t **p, int reg, int32_t disp){
    gb_rex(p, 1, reg, NB_RSI);
    nb(p, 0x89); nb(p, (uint8_t)(0x80 | ((reg&7)<<3) | NB_RSI)); nb32(p, (uint32_t)disp);
}
static void gb_alu_rr(uint8_t **p, int w, uint8_t opc, int reg, int rm){
    gb_rex(p, w, reg, rm); nb(p, opc); nb(p, (uint8_t)(0xC0 | ((reg&7)<<3) | (rm&7)));
}
static void gb_alu0f_rr(uint8_t **p, int w, uint8_t opc, int reg, int rm){
    gb_rex(p, w, reg, rm); nb(p, 0x0F); nb(p, opc); nb(p, (uint8_t)(0xC0 | ((reg&7)<<3) | (rm&7)));
}
static void gb_xor  (uint8_t **p,int w,int d,int s){ gb_alu_rr(p,w,0x31,s,d); }  /* d ^= s   */
static void gb_or   (uint8_t **p,int w,int d,int s){ gb_alu_rr(p,w,0x09,s,d); }  /* d |= s   */
static void gb_sub  (uint8_t **p,int w,int d,int s){ gb_alu_rr(p,w,0x29,s,d); }  /* d -= s   */
static void gb_test (uint8_t **p,int w,int a,int b){ gb_alu_rr(p,w,0x85,a,b); }  /* test a,b */
static void gb_bsr  (uint8_t **p,int w,int d,int s){ gb_alu0f_rr(p,w,0xBD,d,s); }
static void gb_bsf  (uint8_t **p,int w,int d,int s){ gb_alu0f_rr(p,w,0xBC,d,s); }
static void gb_cmovz(uint8_t **p,int w,int d,int s){ gb_alu0f_rr(p,w,0x44,d,s); }
/* setz r8l. Force a REX so r8..r15 (and r4..r7) select the proper low byte. */
static void gb_setz (uint8_t **p,int r){ nb(p,(uint8_t)(0x40|((r>=8)?1:0))); nb(p,0x0F); nb(p,0x94); nb(p,(uint8_t)(0xC0|(r&7))); }
static void gb_shl_imm(uint8_t **p,int w,int r,uint8_t i){ gb_rex(p,w,0,r); nb(p,0xC1); nb(p,(uint8_t)(0xC0|(4<<3)|(r&7))); nb(p,i); }
static void gb_mov_imm32(uint8_t **p,int r,uint32_t imm){ if(r>=8) nb(p,0x41); nb(p,(uint8_t)(0xB8|(r&7))); nb32(p,imm); } /* zero-extends to 64 */
static void gb_and_imm32(uint8_t **p,int w,int r,uint32_t imm){ gb_rex(p,w,0,r); nb(p,0x81); nb(p,(uint8_t)(0xC0|(4<<3)|(r&7))); nb32(p,imm); } /* sign-extends imm32 */
/* ---- live-register helpers for the minimal-spill thunk (push/pop, flag image,
 * jmp). All position-correct so long as the emit cursor is the FINAL location. */
static void gb_push  (uint8_t **p,int r){ if(r>=8) nb(p,0x41); nb(p,(uint8_t)(0x50|(r&7))); }
static void gb_pop   (uint8_t **p,int r){ if(r>=8) nb(p,0x41); nb(p,(uint8_t)(0x58|(r&7))); }
static void gb_pushfq(uint8_t **p){ nb(p,0x9C); }
static void gb_popfq (uint8_t **p){ nb(p,0x9D); }
/* and qword [rsp], imm32  (48 81 /4, modrm 00/4/100 + SIB rsp) — sign-extends imm32 */
static void gb_and_rsp_imm32(uint8_t **p,uint32_t imm){ nb(p,0x48); nb(p,0x81); nb(p,0x24); nb(p,0x24); nb32(p,imm); }
/* or qword [rsp], reg  (REX.W[.R] 09 /r, modrm 00/reg/100 + SIB rsp) */
static void gb_or_rsp_reg(uint8_t **p,int reg){ nb(p,(uint8_t)(0x48|((reg>=8)?4:0))); nb(p,0x09); nb(p,(uint8_t)(0x04|((reg&7)<<3))); nb(p,0x24); }
/* jmp rel32 to absolute `target` from the current (final) emit point */
static int gb_jmp_abs(uint8_t **p, uint64_t target) {
    int64_t delta = (int64_t)target - (int64_t)(uintptr_t)(*p + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) return 0;
    nb(p, 0xe9); nb32(p, (uint32_t)(int32_t)delta);
    return 1;
}
/* lea rsp, [rsp+disp32]  (48 8D A4 24 disp32) — adjust rsp WITHOUT touching flags */
static void gb_lea_rsp(uint8_t **p,int32_t disp){ nb(p,0x48); nb(p,0x8D); nb(p,0xA4); nb(p,0x24); nb32(p,(uint32_t)disp); }
static void gb_mov   (uint8_t **p,int w,int d,int s){ gb_alu_rr(p,w,0x89,s,d); } /* d = s (mov r/m,r) */
/* shl r, cl  (D3 /4) — count masked to 5 (w=0) / 6 (w=1) bits, matching shlx */
static void gb_shl_cl(uint8_t **p,int w,int r){ gb_rex(p,w,0,r); nb(p,0xD3); nb(p,(uint8_t)(0xC0|(4<<3)|(r&7))); }
/* shr r, cl  (D3 /5) — logical right shift by CL, matching shrx */
static void gb_shr_cl(uint8_t **p,int w,int r){ gb_rex(p,w,0,r); nb(p,0xD3); nb(p,(uint8_t)(0xC0|(5<<3)|(r&7))); }
/* sar r, cl  (D3 /7) — arithmetic right shift by CL, matching sarx */
static void gb_sar_cl(uint8_t **p,int w,int r){ gb_rex(p,w,0,r); nb(p,0xD3); nb(p,(uint8_t)(0xC0|(7<<3)|(r&7))); }
/* ror r, imm8  (C1 /1 ib) — rotate right by immediate, matching rorx (base-ISA, no BMI2) */
static void gb_ror_imm(uint8_t **p,int w,int r,uint8_t imm){ gb_rex(p,w,0,r); nb(p,0xC1); nb(p,(uint8_t)(0xC0|(1<<3)|(r&7))); nb(p,imm); }
/* mul r  (F7 /4): (r)dx:(r)ax = (r)ax * r  (unsigned widening; clobbers rdx:rax + flags) */
static void gb_mul_reg(uint8_t **p,int w,int r){ gb_rex(p,w,0,r); nb(p,0xF7); nb(p,(uint8_t)(0xC0|(4<<3)|(r&7))); }
/* and r/m, r  (21 /r) — d &= s, same encoding pattern as xor/or */
static void gb_and(uint8_t **p,int w,int d,int s){ gb_alu_rr(p,w,0x21,s,d); }
/* dec r  (FF /1) — r-- ; modifies flags (CF undefined; ZF/SF/OF/PF/AF defined) */
static void gb_dec(uint8_t **p,int w,int r){ gb_rex(p,w,0,r); nb(p,0xFF); nb(p,(uint8_t)(0xC8|(r&7))); }
/* setnz r/m8  (0F 95 /0) — set byte if ZF==0 (not zero / not equal) */
static void gb_setnz(uint8_t **p,int r){ nb(p,(uint8_t)(0x40|((r>=8)?1:0))); nb(p,0x0F); nb(p,0x95); nb(p,(uint8_t)(0xC0|(r&7))); }
/* sets r/m8   (0F 98 /0) — set byte if SF==1 (sign flag set) */
static void gb_sets (uint8_t **p,int r){ nb(p,(uint8_t)(0x40|((r>=8)?1:0))); nb(p,0x0F); nb(p,0x98); nb(p,(uint8_t)(0xC0|(r&7))); }
/* neg r  (F7 /3) — r = 0 - r ; clobbers CF/ZF/SF/OF/PF/AF */
static void gb_neg  (uint8_t **p,int w,int r){ gb_rex(p,w,0,r); nb(p,0xF7); nb(p,(uint8_t)(0xD8|(r&7))); }
/* not r  (F7 /2) — r = ~r ; does NOT touch flags (unlike NEG) */
static void gb_not  (uint8_t **p,int w,int r){ gb_rex(p,w,0,r); nb(p,0xF7); nb(p,(uint8_t)(0xD0|(r&7))); }

/* Emit one LZCNT into the scalar block. Mirrors reloc.c's LZCNT lowering for the
 * value+flag arithmetic, but reads src and writes dst/flags via the rf MEMORY
 * slots (not live registers), and patches the rf->rflags slot instead of a saved
 * pushf image. dst==src is safe: src is loaded into a register first, and the dst
 * slot is written only at the very end. */
static int gb_emit_lzcnt(uint8_t **p, const decoded *d){
    const int w = (d->opsize == 64);
    const uint32_t n = d->opsize;
    const uint32_t CLR = 0xFFFFF73Eu;   /* ~(CF|ZF|SF|OF) = ~0x8C1, sign-extended to 64 in AND */
    const int S = GB_S, D = GB_D, T1 = GB_T1, T2 = GB_T2, F = GB_F;

    gb_load(p, S, RF_GPR_OFF + d->a_src * 8, w);   /* S = rf->gpr[src] (masked to opsize) */
    gb_xor (p, 1, T1, T1);                          /* T1 = 0 */
    gb_test(p, w, S, S);                            /* ZF = (src==0)  [src not clobbered below] */
    gb_setz(p, T1);                                 /* T1 = (src==0) -> CF */
    gb_bsr (p, w, D, S);                            /* D = bsr(src)  (undef if src==0) */
    gb_mov_imm32(p, T2, n - 1);                     /* T2 = opsize-1 */
    gb_sub (p, 1, T2, D);                           /* T2 = (opsize-1) - bsr = lzcnt for src!=0 */
    gb_test(p, 1, T1, T1);                          /* ZF = (T1==0) = (src!=0) */
    gb_mov_imm32(p, D, n);                          /* D = opsize (zero-input result) */
    gb_cmovz(p, 1, D, T2);                          /* src!=0 -> D = T2 */
    /* ZF = (result==0) -> bit6; SF=0, OF=0 */
    gb_xor (p, 1, T2, T2);
    gb_test(p, w, D, D);
    gb_setz(p, T2);
    gb_shl_imm(p, 1, T2, 6);
    gb_or  (p, 1, T1, T2);                          /* owned = CF | ZF */
    /* patch rf->rflags: clear owned bits, OR in computed, preserve PF/AF/rest */
    gb_load(p, F, RF_FLAGS_OFF, 1);
    gb_and_imm32(p, 1, F, CLR);
    gb_or  (p, 1, F, T1);
    gb_store(p, F, RF_FLAGS_OFF);
    gb_store(p, D, RF_GPR_OFF + d->dst * 8);        /* result (upper 32 zero for opsize 32) */
    return 1;
}

/* Emit one MULX into the scalar block. Semantics (bmi_exec BMI_MULX): s1=rf->gpr[rdx]
 * (implicit), s2=rf->gpr[b_src]; low product -> dst slot, high -> dst2 slot; NO flags.
 * Lowering uses base-ISA MUL ((r)dx:(r)ax = (r)ax * T2). Both sources are loaded into
 * registers BEFORE any dst slot is written, so dst/src slot aliasing is safe. The
 * rflags slot is left untouched (MULX affects no flags), even though MUL clobbers the
 * live CPU flags — tt2 reloads rflags from the slot, so the live clobber is discarded.
 * opsize 32: 32-bit MUL zero-extends eax/edx to the full 64-bit slot (matches
 * (u32)p / (u32)(p>>32)); opsize 64: rdx:rax is the 128-bit product. */
static int gb_emit_mulx(uint8_t **p, const decoded *d){
    const int w = (d->opsize == 64);
    gb_load(p, GB_S,  RF_GPR_OFF + 2 * 8, w);            /* rax = s1 = gpr[rdx] */
    gb_load(p, GB_T2, RF_GPR_OFF + d->b_src * 8, w);     /* T2  = s2 = gpr[b_src] */
    gb_mul_reg(p, w, GB_T2);                             /* rdx:rax = rax * T2 */
    gb_store(p, GB_S,  RF_GPR_OFF + d->dst      * 8);    /* dst  = low  (rax) */
    gb_store(p, GB_T1, RF_GPR_OFF + d->bmi_dst2 * 8);    /* dst2 = high (rdx == GB_T1) */
    return 1;
}

/* native-BLOCK op support (LZCNT + MULX). SEPARATE from bmi_native_insn_supported
 * below (which the minspill path shares, LZCNT/shlx-only) so broadening the block
 * path here cannot change minspill selection. */
static int bmi_block_op_supported(const decoded *d){
    if (!d->is_bmi) return 0;
    if (d->dst_kind != DST_GPR) return 0;
    if (!(d->opsize == 32 || d->opsize == 64)) return 0;
    if (d->seg != 0) return 0;
    if (d->a_src == OPND_MEM || d->b_src == OPND_MEM || d->dst_kind == DST_MEM) return 0;
    if (d->dst < 0 || d->dst > 15) return 0;
    switch (d->op){
    case BMI_LZCNT:
        if (d->a_src < 0 || d->a_src > 15) return 0;
        if (d->bmi_dst2 != OPND_NONE || d->bmi_s1_rdx) return 0;
        return 1;
    case BMI_MULX:
        if (!d->bmi_s1_rdx) return 0;                       /* s1 must be implicit rdx */
        if (d->bmi_dst2 < 0 || d->bmi_dst2 > 15) return 0;  /* high dest */
        if (d->b_src   < 0 || d->b_src   > 15) return 0;    /* s2 = gpr[b_src] */
        return 1;
    default: return 0;
    }
}

/* Dispatch one supported scalar-BMI op to its emitter. */
static int gb_emit_bmi(uint8_t **p, const decoded *d){
    switch (d->op){
    case BMI_LZCNT: return gb_emit_lzcnt(p, d);
    case BMI_MULX:  return gb_emit_mulx(p, d);
    default: return 0;
    }
}

/* Is this insn a scalar-GPR op the native scalar block supports? (LZCNT only for
 * now; register operands, no memory/segment, opsize 32/64.) */
static int bmi_native_insn_supported(const decoded *d){
    if (!d->is_bmi || d->op != BMI_LZCNT) return 0;
    if (d->dst_kind != DST_GPR) return 0;
    if (!(d->opsize == 32 || d->opsize == 64)) return 0;
    if (d->seg != 0) return 0;
    if (d->a_src == OPND_MEM || d->b_src == OPND_MEM || d->dst_kind == DST_MEM) return 0;
    if (d->dst < 0 || d->dst > 15) return 0;
    if (d->a_src < 0 || d->a_src > 15) return 0;
    if (d->bmi_dst2 != OPND_NONE || d->bmi_s1_rdx) return 0;
    return 1;
}

/* ==========================================================================
 * Minimal-spill, LIVE-REGISTER lzcnt thunk (Phase 1a — the hypothesis gate).
 *
 * Unlike the tt2 path (spill all 16 GPR + flags to the regfile, run against
 * slots, reload all), this emits a per-op thunk that runs lzcnt on the LIVE
 * program registers in place, saving only the 2 scratch GPRs its lowering
 * borrows + the original flags image. Entered by `jmp` from the patched site;
 * ends with `jmp resume`.
 *
 * Reuses gb_emit_lzcnt's exact math (CF=(src==0) captured before bsr for dst==src
 * safety; result = (src==0)?opsize:(opsize-1-bsr); ZF=(result==0); SF=OF=0; owned
 * mask CLR=~(CF|ZF|SF|OF)), but: operands are live dst/src registers, and flags
 * are the program's via pushfq/popfq. CRITICAL: pushfq runs BEFORE the arithmetic
 * so the saved image holds the ORIGINAL PF/AF (the math clobbers them); we then
 * patch only the owned bits in that saved image. (The plan sketched pushfq AFTER
 * the math — that would write back arithmetic-clobbered PF/AF, breaking the
 * "PF/AF unchanged" floor.)
 * ========================================================================== */
/* Like bmi_native_insn_supported, but ALSO declines rsp (4) as dst or src: the
 * live-register thunk uses rsp as its working stack (push/pushfq), so an rsp
 * operand would read/write the moved stack pointer. Such ops fall back to the
 * slot-based path (which handles rsp correctly via the regfile). */
static int minspill_lzcnt_supported(const decoded *d){
    if (!bmi_native_insn_supported(d)) return 0;
    if (d->dst == 4 || d->a_src == 4) return 0;   /* rsp operand -> fall back */
    return 1;
}

/* Emit the minimal-spill live-register lzcnt thunk at *p (the FINAL location, so
 * the trailing jmp rel32 is correct). Returns 1, or 0 if d is unsupported. */
static int emit_minspill_lzcnt(uint8_t **p, const decoded *d, uint64_t resume){
    if (!minspill_lzcnt_supported(d)) return 0;
    const int w = (d->opsize == 64);
    const uint32_t n = d->opsize;
    const uint32_t CLR = 0xFFFFF73Eu;          /* ~(CF|ZF|SF|OF); sign-extends, preserves PF/AF */
    const int D = d->dst, S = d->a_src;
    /* Two scratch GPRs, deterministically the first two ids that are not dst, src,
     * or rsp(4). 16 regs minus at most 3 exclusions => always available. */
    int T1 = -1, T2 = -1;
    for (int r = 0; r < 16 && T2 < 0; r++){
        if (r == D || r == S || r == 4) continue;
        if (T1 < 0) T1 = r; else T2 = r;
    }
    if (T1 < 0 || T2 < 0) return 0;

    /* Step BELOW the interrupted function's 128-byte red zone before using the
     * stack (push/pushfq), then restore — matching tramp.S's FRAME discipline.
     * lea (not sub) so flags are untouched: pushfq below still sees ORIGINAL flags. */
    gb_lea_rsp(p, -128);
    gb_push(p, T1);
    gb_push(p, T2);
    gb_pushfq(p);                 /* [rsp] = ORIGINAL program flags (PF/AF source) */

    /* CF = (src==0), captured BEFORE bsr (src read first => dst==src safe) */
    gb_xor (p, 1, T1, T1);        /* T1 = 0 */
    gb_test(p, w, S, S);          /* ZF = (src==0) */
    gb_setz(p, T1);               /* T1 = (src==0) */
    /* result into D */
    gb_bsr (p, w, D, S);          /* D = bsr(src) (undef if src==0; w=0 zero-extends D) */
    gb_mov_imm32(p, T2, n - 1);   /* T2 = opsize-1 */
    gb_sub (p, 1, T2, D);         /* T2 = (opsize-1) - bsr */
    gb_test(p, 1, T1, T1);        /* ZF = (T1==0) = (src!=0) */
    gb_mov_imm32(p, D, n);        /* D = opsize (zero-input result; mov: no flag effect) */
    gb_cmovz(p, 1, D, T2);        /* src!=0 -> D = T2 */
    /* owned = CF (T1 bit0) | ZF(result==0)<<6 ; SF=OF=0 -> value in T1 */
    gb_xor (p, 1, T2, T2);
    gb_test(p, w, D, D);
    gb_setz(p, T2);
    gb_shl_imm(p, 1, T2, 6);
    gb_or  (p, 1, T1, T2);        /* T1 = owned value */
    /* patch saved flags image: clear owned, OR computed; PF/AF + rest preserved */
    gb_and_rsp_imm32(p, CLR);
    gb_or_rsp_reg(p, T1);
    gb_popfq(p);                  /* live flags = patched original */
    gb_pop (p, T2);
    gb_pop (p, T1);
    gb_lea_rsp(p, 128);           /* restore rsp above the red zone (flags untouched) */
    if (!gb_jmp_abs(p, resume)) return 0;
    return 1;
}

/* ==========================================================================
 * Minimal-spill, LIVE-REGISTER shlx thunk (re-aimed Phase-1a gate op).
 *
 * shlx dst, value, count : dst = value << (count & (opsize-1)); NO flags affected.
 * (decode: dst=reg, a_src=value[r/m], b_src=count[reg].) The base-ISA shift
 * `shl r,cl` needs the count in CL and DOES write flags, so the thunk: steps past
 * the red zone, saves ALL flags (pushfq) + rcx + one scratch T, computes
 * value<<count into T via cl, writes dst, restores T/rcx/flags. Declines rsp
 * operands (we move rsp), a memory value (falls back), and dst==rcx (we restore
 * rcx, which would clobber the result).
 * ========================================================================== */
static int minspill_shlx_supported(const decoded *d){
    if (!d->is_bmi || d->op != BMI_SHLX) return 0;
    if (d->dst_kind != DST_GPR) return 0;
    if (!(d->opsize == 32 || d->opsize == 64)) return 0;
    if (d->seg != 0) return 0;
    if (d->a_src == OPND_MEM || d->dst_kind == DST_MEM) return 0;   /* reg value only */
    if (d->dst < 0 || d->dst > 15) return 0;
    if (d->a_src < 0 || d->a_src > 15) return 0;                    /* value reg */
    if (d->b_src < 0 || d->b_src > 15) return 0;                    /* count reg */
    if (d->bmi_dst2 != OPND_NONE || d->bmi_s1_rdx) return 0;
    if (d->dst == 4 || d->a_src == 4 || d->b_src == 4) return 0;    /* rsp operand -> fall back */
    if (d->dst == 1) return 0;                                      /* dst==rcx -> fall back (rcx is our CL scratch) */
    return 1;
}

/* Emit the minimal-spill live-register shlx thunk at *p (the FINAL location). */
static int emit_minspill_shlx(uint8_t **p, const decoded *d, uint64_t resume){
    if (!minspill_shlx_supported(d)) return 0;
    const int w = (d->opsize == 64);
    const int D = d->dst, V = d->a_src, C = d->b_src;
    const int RCX = 1;
    /* one scratch T, not dst/value/count/rcx/rsp */
    int T = -1;
    for (int r = 0; r < 16; r++){ if (r==D||r==V||r==C||r==RCX||r==4) continue; T = r; break; }
    if (T < 0) return 0;

    gb_lea_rsp(p, -128);          /* step below the red zone (no flag effect) */
    gb_pushfq(p);                 /* save ALL flags (shl clobbers; shlx preserves) */
    gb_push(p, RCX);              /* save program rcx (we need CL) */
    gb_push(p, T);                /* save scratch T */
    gb_mov(p, w, T, V);           /* T = value (read V before rcx is clobbered) */
    gb_mov(p, 1, RCX, C);         /* rcx = count (64-bit; shl uses CL, masks to size-1) */
    gb_shl_cl(p, w, T);           /* T = value << (count & mask) */
    gb_mov(p, 1, D, T);           /* dst = result (D != rcx, so the pop below won't clobber;
                                     w=0 left T zero-extended, so 64-bit mov is correct) */
    gb_pop(p, T);
    gb_pop(p, RCX);               /* restore program rcx */
    gb_popfq(p);                  /* restore flags unchanged (shlx defines none) */
    gb_lea_rsp(p, 128);
    if (!gb_jmp_abs(p, resume)) return 0;
    return 1;
}

/* ==========================================================================
 * Minimal-spill, LIVE-REGISTER tzcnt thunk.
 *
 * TZCNT is the BSF mirror of LZCNT: result = (src==0) ? opsize : bsf(src).
 * Flags: CF=(src==0); ZF=(result==0); SF=0; OF=0; PF/AF preserved.
 * Base-ISA lowering: BSF (0F BC) gives the trailing-zero count for src!=0;
 * src==0 is special-cased to opsize with CF=1, using the same
 * test+setz+cmovz pattern as emit_minspill_lzcnt.
 *
 * CRITICAL dst==src safety: BSF reads src into scratch T2 (NOT D) before D
 * is overwritten with the opsize default, so the original src is preserved
 * even when dst and src are the same register.
 *
 * pushfq runs BEFORE the arithmetic (same reason as lzcnt: arithmetic
 * clobbers PF/AF, so the saved image must hold the original PF/AF).
 * ========================================================================== */
static int minspill_tzcnt_supported(const decoded *d){
    if (!d->is_bmi || d->op != BMI_TZCNT) return 0;
    if (d->dst_kind != DST_GPR) return 0;
    if (!(d->opsize == 32 || d->opsize == 64)) return 0;
    if (d->seg != 0) return 0;
    if (d->a_src == OPND_MEM || d->b_src == OPND_MEM || d->dst_kind == DST_MEM) return 0;
    if (d->dst < 0 || d->dst > 15) return 0;
    if (d->a_src < 0 || d->a_src > 15) return 0;
    if (d->bmi_dst2 != OPND_NONE || d->bmi_s1_rdx) return 0;
    if (d->dst == 4 || d->a_src == 4) return 0;   /* rsp operand -> fall back */
    return 1;
}

/* Emit the minimal-spill live-register tzcnt thunk at *p (the FINAL location). */
static int emit_minspill_tzcnt(uint8_t **p, const decoded *d, uint64_t resume){
    if (!minspill_tzcnt_supported(d)) return 0;
    const int w = (d->opsize == 64);
    const uint32_t n = d->opsize;
    const uint32_t CLR = 0xFFFFF73Eu;          /* ~(CF|ZF|SF|OF); sign-extends, preserves PF/AF */
    const int D = d->dst, S = d->a_src;
    /* Two scratch GPRs, deterministically the first two ids not dst, src, or rsp(4). */
    int T1 = -1, T2 = -1;
    for (int r = 0; r < 16 && T2 < 0; r++){
        if (r == D || r == S || r == 4) continue;
        if (T1 < 0) T1 = r; else T2 = r;
    }
    if (T1 < 0 || T2 < 0) return 0;

    gb_lea_rsp(p, -128);           /* step below red zone (no flag effect) */
    gb_push(p, T1);
    gb_push(p, T2);
    gb_pushfq(p);                  /* [rsp] = ORIGINAL program flags (PF/AF source) */

    /* CF = (src==0), captured BEFORE bsf (src read first => dst==src safe) */
    gb_xor (p, 1, T1, T1);        /* T1 = 0 */
    gb_test(p, w, S, S);           /* ZF = (src==0) */
    gb_setz(p, T1);                /* T1 = (src==0) -> CF */
    /* BSF into T2 (not D), so dst==src is safe: D not touched yet */
    gb_bsf (p, w, T2, S);         /* T2 = bsf(src) = tzcnt for src!=0 (undef if src==0) */
    gb_mov_imm32(p, D, n);        /* D = opsize (zero-input result; mov: no flag effect) */
    gb_test(p, 1, T1, T1);        /* ZF = (T1==0) = (src!=0) */
    gb_cmovz(p, 1, D, T2);        /* src!=0 -> D = T2 = bsf result */
    /* ZF = (result==0) -> bit6; SF=0, OF=0 */
    gb_xor (p, 1, T2, T2);
    gb_test(p, w, D, D);
    gb_setz(p, T2);
    gb_shl_imm(p, 1, T2, 6);
    gb_or  (p, 1, T1, T2);        /* T1 = CF | ZF (owned value) */
    /* patch saved flags image: clear owned bits, OR in computed, preserve PF/AF/rest */
    gb_and_rsp_imm32(p, CLR);
    gb_or_rsp_reg(p, T1);
    gb_popfq(p);                   /* live flags = patched original */
    gb_pop (p, T2);
    gb_pop (p, T1);
    gb_lea_rsp(p, 128);            /* restore rsp above the red zone (flags untouched) */
    if (!gb_jmp_abs(p, resume)) return 0;
    return 1;
}

/* ==========================================================================
 * Minimal-spill, LIVE-REGISTER shrx thunk.
 *
 * shrx dst, value, count : dst = value >> (count & (opsize-1)); NO flags.
 * Logical right shift: upper bits of result are zero (unlike sarx).
 * Mirrors emit_minspill_shlx exactly but uses SHR (D3 /5) instead of SHL.
 * Same scratch/aliasing discipline: value read into T before rcx is clobbered
 * by the count, so dst==value and dst==count aliasing are both safe.
 * ========================================================================== */
static int minspill_shrx_supported(const decoded *d){
    if (!d->is_bmi || d->op != BMI_SHRX) return 0;
    if (d->dst_kind != DST_GPR) return 0;
    if (!(d->opsize == 32 || d->opsize == 64)) return 0;
    if (d->seg != 0) return 0;
    if (d->a_src == OPND_MEM || d->dst_kind == DST_MEM) return 0;
    if (d->dst < 0 || d->dst > 15) return 0;
    if (d->a_src < 0 || d->a_src > 15) return 0;
    if (d->b_src < 0 || d->b_src > 15) return 0;
    if (d->bmi_dst2 != OPND_NONE || d->bmi_s1_rdx) return 0;
    if (d->dst == 4 || d->a_src == 4 || d->b_src == 4) return 0;   /* rsp operand -> fall back */
    if (d->dst == 1) return 0;                                      /* dst==rcx -> fall back (rcx is our CL scratch) */
    return 1;
}

static int emit_minspill_shrx(uint8_t **p, const decoded *d, uint64_t resume){
    if (!minspill_shrx_supported(d)) return 0;
    const int w = (d->opsize == 64);
    const int D = d->dst, V = d->a_src, C = d->b_src;
    const int RCX = 1;
    /* one scratch T, not dst/value/count/rcx/rsp */
    int T = -1;
    for (int r = 0; r < 16; r++){ if (r==D||r==V||r==C||r==RCX||r==4) continue; T = r; break; }
    if (T < 0) return 0;

    gb_lea_rsp(p, -128);          /* step below the red zone (no flag effect) */
    gb_pushfq(p);                 /* save ALL flags (shr clobbers; shrx preserves) */
    gb_push(p, RCX);              /* save program rcx (we need CL) */
    gb_push(p, T);                /* save scratch T */
    gb_mov(p, w, T, V);           /* T = value (read V before rcx is clobbered) */
    gb_mov(p, 1, RCX, C);         /* rcx = count (64-bit; shr uses CL, masks to size-1) */
    gb_shr_cl(p, w, T);           /* T = value >> (count & mask) */
    gb_mov(p, 1, D, T);           /* dst = result (D != rcx; pop below won't clobber) */
    gb_pop(p, T);
    gb_pop(p, RCX);               /* restore program rcx */
    gb_popfq(p);                  /* restore flags unchanged (shrx defines none) */
    gb_lea_rsp(p, 128);
    if (!gb_jmp_abs(p, resume)) return 0;
    return 1;
}

/* ==========================================================================
 * Minimal-spill, LIVE-REGISTER sarx thunk.
 *
 * sarx dst, value, count : dst = value sar (count & (opsize-1)); NO flags.
 * Arithmetic right shift: the sign bit is replicated (unlike shrx which fills
 * with zeros). Mirrors emit_minspill_shrx exactly but uses SAR (D3 /7).
 * For opsize=32 the base-ISA SAR r32,CL sign-extends within 32 bits and the
 * result is zero-extended to 64 (via the 32-bit register write), matching
 * bmi_exec: (uint64_t)(uint32_t)((int32_t)(uint32_t)s1 >> n).
 * ========================================================================== */
static int minspill_sarx_supported(const decoded *d){
    if (!d->is_bmi || d->op != BMI_SARX) return 0;
    if (d->dst_kind != DST_GPR) return 0;
    if (!(d->opsize == 32 || d->opsize == 64)) return 0;
    if (d->seg != 0) return 0;
    if (d->a_src == OPND_MEM || d->dst_kind == DST_MEM) return 0;
    if (d->dst < 0 || d->dst > 15) return 0;
    if (d->a_src < 0 || d->a_src > 15) return 0;
    if (d->b_src < 0 || d->b_src > 15) return 0;
    if (d->bmi_dst2 != OPND_NONE || d->bmi_s1_rdx) return 0;
    if (d->dst == 4 || d->a_src == 4 || d->b_src == 4) return 0;   /* rsp operand -> fall back */
    if (d->dst == 1) return 0;                                      /* dst==rcx -> fall back */
    return 1;
}

static int emit_minspill_sarx(uint8_t **p, const decoded *d, uint64_t resume){
    if (!minspill_sarx_supported(d)) return 0;
    const int w = (d->opsize == 64);
    const int D = d->dst, V = d->a_src, C = d->b_src;
    const int RCX = 1;
    int T = -1;
    for (int r = 0; r < 16; r++){ if (r==D||r==V||r==C||r==RCX||r==4) continue; T = r; break; }
    if (T < 0) return 0;

    gb_lea_rsp(p, -128);
    gb_pushfq(p);                 /* save ALL flags (sar clobbers; sarx preserves) */
    gb_push(p, RCX);
    gb_push(p, T);
    gb_mov(p, w, T, V);           /* T = value (read before rcx clobbered) */
    gb_mov(p, 1, RCX, C);         /* rcx = count */
    gb_sar_cl(p, w, T);           /* T = value sar (count & mask) */
    gb_mov(p, 1, D, T);           /* dst = result */
    gb_pop(p, T);
    gb_pop(p, RCX);
    gb_popfq(p);
    gb_lea_rsp(p, 128);
    if (!gb_jmp_abs(p, resume)) return 0;
    return 1;
}

/* ==========================================================================
 * Minimal-spill, LIVE-REGISTER rorx thunk.
 *
 * rorx dst, value, imm : dst = ror(value, imm & (opsize-1)); NO flags.
 * The count is an IMMEDIATE baked into the instruction (d->imm), not a
 * register — so no CL/rcx needed at all. Simpler than shrx/sarx: only one
 * scratch T (for the value copy, so dst==value aliasing is safe). pushfq/
 * popfq preserve the program flags that the base-ISA ROR would otherwise
 * clobber. No dst==rcx decline (rcx is never borrowed). Declines rsp.
 *
 * The immediate is masked to opsize-1 bits before encoding, matching bmi_exec.
 * ROR r, 0 (after masking) is a well-defined no-op on x86: flags are not
 * affected and the destination is unchanged.
 * ========================================================================== */
static int minspill_rorx_supported(const decoded *d){
    if (!d->is_bmi || d->op != BMI_RORX) return 0;
    if (d->dst_kind != DST_GPR) return 0;
    if (!(d->opsize == 32 || d->opsize == 64)) return 0;
    if (d->seg != 0) return 0;
    if (d->a_src == OPND_MEM || d->dst_kind == DST_MEM) return 0;
    if (d->dst < 0 || d->dst > 15) return 0;
    if (d->a_src < 0 || d->a_src > 15) return 0;
    if (d->bmi_dst2 != OPND_NONE || d->bmi_s1_rdx) return 0;
    if (d->dst == 4 || d->a_src == 4) return 0;   /* rsp operand -> fall back */
    /* No dst==rcx constraint: rorx count is an immediate, CL not needed */
    return 1;
}

static int emit_minspill_rorx(uint8_t **p, const decoded *d, uint64_t resume){
    if (!minspill_rorx_supported(d)) return 0;
    const int w = (d->opsize == 64);
    const int D = d->dst, V = d->a_src;
    /* mask to opsize-1 bits, matching bmi_exec: n = d->imm & (opsize-1) */
    const uint8_t n = (uint8_t)((uint32_t)d->imm & (uint32_t)(d->opsize - 1));
    /* one scratch T: not dst/value/rsp (no rcx constraint — not borrowing CL) */
    int T = -1;
    for (int r = 0; r < 16; r++){ if (r==D||r==V||r==4) continue; T = r; break; }
    if (T < 0) return 0;

    gb_lea_rsp(p, -128);
    gb_pushfq(p);                 /* save ALL flags (ror clobbers; rorx preserves) */
    gb_push(p, T);                /* save scratch T */
    gb_mov(p, w, T, V);           /* T = value (dst==value aliasing: read before write) */
    gb_ror_imm(p, w, T, n);       /* T = ror(value, n) using base-ISA C1 /1 ib (NOT rorx) */
    gb_mov(p, 1, D, T);           /* dst = result */
    gb_pop(p, T);                 /* restore scratch T */
    gb_popfq(p);                  /* restore flags unchanged (rorx defines none) */
    gb_lea_rsp(p, 128);
    if (!gb_jmp_abs(p, resume)) return 0;
    return 1;
}

/* ==========================================================================
 * Minimal-spill, LIVE-REGISTER bzhi thunk.
 *
 * BZHI dst, value(a_src/rm), index(b_src/vvvv):
 *   n = index & 0xFF
 *   result = (n >= opsize) ? value : value & ((1<<n) - 1)
 *   CF = (n > opsize-1);  ZF = (result==0);  SF = bit(opsize-1) of result;  OF = 0
 * Flags: CF and SF are REAL (not forced to 0). PF/AF preserved.
 *
 * Lowering (base ISA only — no BZHI, no BMI):
 *   - Save T1, T2, T3, rcx on stack; pushfq BEFORE math (preserve PF/AF).
 *   - Read both sources into T1/T2 BEFORE writing dst (aliasing-safe for dst==value,
 *     dst==index, value==index).
 *   - CF via overflow bits of n (n & 0xE0 for opsize=32, n & 0xC0 for opsize=64).
 *   - Mask via (1<<n)-1 using shl/dec; cmovz selects between masked and original
 *     value (the n>=opsize candidate), making the wrapping shl result irrelevant.
 *   - ZF and SF captured via test dst,dst + setz/sets BEFORE any flag-clobbering op.
 *   - Patch the saved flags image (CLR = ~(CF|ZF|SF|OF) = 0xFFFFF73E).
 *
 * Declines: rsp(4) as any operand (we move rsp); dst==rcx(1) (pop rcx after write
 * clobbers the result).
 * ========================================================================== */
static int minspill_bzhi_supported(const decoded *d){
    if (!d->is_bmi || d->op != BMI_BZHI) return 0;
    if (d->dst_kind != DST_GPR) return 0;
    if (!(d->opsize == 32 || d->opsize == 64)) return 0;
    if (d->seg != 0) return 0;
    if (d->a_src == OPND_MEM || d->b_src == OPND_MEM || d->dst_kind == DST_MEM) return 0;
    if (d->dst  < 0 || d->dst  > 15) return 0;
    if (d->a_src < 0 || d->a_src > 15) return 0;
    if (d->b_src < 0 || d->b_src > 15) return 0;
    if (d->bmi_dst2 != OPND_NONE || d->bmi_s1_rdx) return 0;
    if (d->dst == 4 || d->a_src == 4 || d->b_src == 4) return 0;  /* rsp -> fall back */
    if (d->dst == 1) return 0;   /* dst==rcx: pop(rcx) after result write clobbers it */
    return 1;
}

static int emit_minspill_bzhi(uint8_t **p, const decoded *d, uint64_t resume){
    if (!minspill_bzhi_supported(d)) return 0;
    const int w = (d->opsize == 64);
    const uint32_t CLR = 0xFFFFF73Eu;   /* ~(CF|ZF|SF|OF); sign-extends, preserves PF/AF */
    const int D = d->dst, V = d->a_src, IX = d->b_src;
    const int RCX = 1;   /* always borrowed for the variable shift (shl_cl uses CL) */
    /* Three scratch GPRs: first three ids not in {D, V, IX, RCX=1, rsp=4}. At least
     * 10 candidates remain after at most 5 distinct exclusions — always possible. */
    int T1 = -1, T2 = -1, T3 = -1;
    for (int r = 0; r < 16 && T3 < 0; r++){
        if (r == D || r == V || r == IX || r == RCX || r == 4) continue;
        if (T1 < 0) T1 = r; else if (T2 < 0) T2 = r; else T3 = r;
    }
    if (T1 < 0 || T2 < 0 || T3 < 0) return 0;

    /* Overflow mask: bits of n that indicate n >= opsize.
     * opsize=32: n >= 32 iff n & 0xE0 != 0 (bits 5,6,7; n in [0,255]).
     * opsize=64: n >= 64 iff n & 0xC0 != 0 (bits 6,7). */
    const uint32_t ovf = (d->opsize == 32) ? 0xE0u : 0xC0u;

    /* Step below the red zone before using the stack (lea: no flag effect).
     * pushfq BEFORE arithmetic so the saved image holds ORIGINAL PF/AF. */
    gb_lea_rsp(p, -128);
    gb_push(p, T1); gb_push(p, T2); gb_push(p, T3); gb_push(p, RCX);
    gb_pushfq(p);          /* [rsp] = ORIGINAL program flags */

    /* Read both sources into scratch BEFORE any write to D (dst==value, dst==index safe). */
    gb_mov(p, 1, T1, IX);              /* T1 = raw index (full 64-bit) */
    gb_and_imm32(p, 0, T1, 0xFF);     /* T1 = n = index & 0xFF (zero-extended to 64) */
    gb_mov(p, w, T2, V);              /* T2 = value (masked to opsize by 32/64-bit move) */

    /* Copy n into T3 (for overflow test) and RCX (for shift). T1 preserved as n. */
    gb_mov(p, 1, T3, T1);             /* T3 = n */
    gb_mov(p, 1, RCX, T1);            /* RCX = n (CL will hold n for shl_cl) */

    /* CF = (n >= opsize): AND overflow bits of n into T3, normalize to 0/1 via setnz. */
    gb_and_imm32(p, 0, T3, ovf);      /* T3 = n & ovf (0 if n < opsize, nonzero if n >= opsize) */
    gb_test(p, 1, T3, T3);            /* ZF = (T3==0) = (n < opsize) */
    gb_setnz(p, T3);                   /* T3 = CF bit (1 if n >= opsize, 0 if n < opsize) */

    /* Build mask: T1 = (1 << n) - 1.  For n >= opsize the shl wraps (base-ISA masks
     * count to opsize-1 bits), yielding a garbage mask — cmov below discards it. */
    gb_mov_imm32(p, T1, 1);           /* T1 = 1 (zero-extended to 64) */
    gb_shl_cl(p, w, T1);              /* T1 = 1 << (n & (opsize-1)) */
    gb_dec(p, w, T1);                  /* T1 = (1<<n) - 1 [mask for n < opsize] */

    /* T2 = value & mask (CF=0 candidate result). */
    gb_and(p, w, T2, T1);             /* T2 = value & mask */

    /* Initialize D = value (CF=1 candidate: value unchanged for n >= opsize).
     * If D==V this is mov D,D (a no-op); if D==IX the original index was already saved
     * to T1 above and V still holds value untouched. */
    gb_mov(p, w, D, V);               /* D = value */

    /* Select result: if n < opsize (ZF=1 from test T3,T3): D = T2 (masked);
     * if n >= opsize (ZF=0): D stays = value. cmovz uses the ZF from the test below. */
    gb_test(p, 1, T3, T3);            /* ZF = (T3==0) = CF==0 = n < opsize */
    gb_cmovz(p, w, D, T2);            /* n < opsize: D = masked result */

    /* Compute owned flag bits from the final result in D.
     * test D,D sets ZF=(D==0) and SF=sign-bit-of-D simultaneously.
     * setz/sets read those bits WITHOUT modifying rflags; shifts after are fine. */
    gb_test(p, w, D, D);              /* ZF=(result==0), SF=bit(opsize-1) of result */
    /* SETcc writes only a byte; discard the earlier mask/value before it
     * becomes a full-width flags operand. MOV preserves the TEST flags. */
    gb_mov_imm32(p, T1, 0);
    gb_mov_imm32(p, T2, 0);
    gb_setz(p, T1);                    /* T1 = ZF (result==0) */
    gb_sets(p, T2);                    /* T2 = SF (sign bit of result) */
    gb_shl_imm(p, 1, T1, 6);         /* T1 = ZF<<6  (bit position of ZF in rflags) */
    gb_shl_imm(p, 1, T2, 7);         /* T2 = SF<<7  (bit position of SF in rflags) */
    gb_or(p, 1, T3, T1);             /* T3 |= ZF<<6 */
    gb_or(p, 1, T3, T2);             /* T3 |= SF<<7 */
    /* T3 = CF | (ZF<<6) | (SF<<7) ; OF=0 (BZHI always clears OF) */

    /* Patch saved flags image: clear owned bits; OR in computed; preserve PF/AF/rest. */
    gb_and_rsp_imm32(p, CLR);
    gb_or_rsp_reg(p, T3);
    gb_popfq(p);                      /* live flags = patched original */
    gb_pop(p, RCX);                   /* restore program rcx */
    gb_pop(p, T3); gb_pop(p, T2); gb_pop(p, T1);
    gb_lea_rsp(p, 128);               /* restore rsp above the red zone (flags untouched) */
    if (!gb_jmp_abs(p, resume)) return 0;
    return 1;
}

/* ==========================================================================
 * Minimal-spill, LIVE-REGISTER BLSR/BLSI/BLSMSK thunks.
 *
 * These are single-source BMI1 ops (one input GPR, one output GPR):
 *   BLSR  dst, src: result = src & (src - 1)   CF=(src==0)
 *   BLSI  dst, src: result = src & (0 - src)   CF=(src!=0)
 *   BLSMSK dst, src: result = src ^ (src - 1)  CF=(src==0)
 * All three: ZF=(result==0); SF=sign bit of result; OF=0; PF/AF preserved.
 *
 * decode layout: d->dst = VEX.vvvv (destination); d->a_src = ModRM.rm (source);
 * d->b_src = OPND_NONE (no second source); d->bmi_s1_rdx = 0; d->bmi_dst2 = OPND_NONE.
 *
 * Lowering (base ISA only — no blsr/blsi/blsmsk):
 *   - pushfq BEFORE math (preserve PF/AF in saved image).
 *   - Read src into T2 BEFORE any write to dst (dst==src aliasing safe).
 *   - Capture CF condition (test src,src + setz/setnz into T1) BEFORE arithmetic.
 *   - Compute result into T2: BLSR: dec+and; BLSI: neg+and; BLSMSK: dec+xor.
 *   - mov dst, T2  (result; opsize=32 zero-extends to 64 via 32-bit mov).
 *   - Capture ZF and SF from result: test dst,dst; setz T2; sets T3; shift+or.
 *   - Patch saved flags (CLR = ~(CF|ZF|SF|OF) = 0xFFFFF73E), popfq.
 *
 * No rcx/dst==rcx constraint: these ops never need CL (no variable shift).
 * Decline only rsp(4) as dst or src (we move rsp for the stack frame).
 * Three scratch GPRs needed: T1=CF accumulator, T2=computation scratch/ZF,
 * T3=SF byte. Picked as first three not in {D, S, rsp=4}.
 * ========================================================================== */
static int minspill_blsr_supported(const decoded *d){
    if (!d->is_bmi || d->op != BMI_BLSR) return 0;
    if (d->dst_kind != DST_GPR) return 0;
    if (!(d->opsize == 32 || d->opsize == 64)) return 0;
    if (d->seg != 0) return 0;
    if (d->a_src == OPND_MEM || d->dst_kind == DST_MEM) return 0;
    if (d->dst  < 0 || d->dst  > 15) return 0;
    if (d->a_src < 0 || d->a_src > 15) return 0;
    if (d->bmi_dst2 != OPND_NONE || d->bmi_s1_rdx) return 0;
    if (d->dst == 4 || d->a_src == 4) return 0;   /* rsp operand -> fall back */
    return 1;
}

/* Shared scratch selector for single-source ops: picks first three ids not in
 * {D, S, rsp=4}.  With at most 3 distinct exclusions, 13+ candidates remain. */
static int pick3(int D, int S, int *T1, int *T2, int *T3){
    *T1 = *T2 = *T3 = -1;
    for (int r = 0; r < 16 && *T3 < 0; r++){
        if (r == D || r == S || r == 4) continue;
        if (*T1 < 0) *T1 = r; else if (*T2 < 0) *T2 = r; else *T3 = r;
    }
    return (*T1 >= 0 && *T2 >= 0 && *T3 >= 0);
}

/* Emit the owned CF|ZF|SF bits into T1 after the result is in D:
 *   T1 already holds CF bit (set before math).
 *   test D,D  -> setz T2 (ZF) + sets T3 (SF); shift+or into T1.
 *   T1 = CF | (ZF<<6) | (SF<<7) ; OF=0 (always 0 for BLSR/BLSI/BLSMSK). */
static void emit_zfsf_bits(uint8_t **p, int w, int D, int T1, int T2, int T3){
    gb_test(p, w, D, D);         /* ZF=(D==0), SF=bit(opsize-1) of D; CF=OF=0 */
    /* Scratch still contains the result and arbitrary incoming high bits. */
    gb_mov_imm32(p, T2, 0);
    gb_mov_imm32(p, T3, 0);
    gb_setz(p, T2);               /* T2 = ZF bit (0 or 1)  [setz doesn't touch flags] */
    gb_sets(p, T3);               /* T3 = SF bit (0 or 1)  [sets  doesn't touch flags] */
    gb_shl_imm(p, 1, T2, 6);    /* T2 = ZF<<6  (bit-6 position in rflags) */
    gb_shl_imm(p, 1, T3, 7);    /* T3 = SF<<7  (bit-7 position in rflags) */
    gb_or(p, 1, T1, T2);         /* T1 |= ZF<<6 */
    gb_or(p, 1, T1, T3);         /* T1 |= SF<<7 */
    /* T1 = CF | (ZF<<6) | (SF<<7) ; OF bit not set (BLSR/BLSI/BLSMSK always clear OF) */
}

static int emit_minspill_blsr(uint8_t **p, const decoded *d, uint64_t resume){
    if (!minspill_blsr_supported(d)) return 0;
    const int w = (d->opsize == 64);
    const uint32_t CLR = 0xFFFFF73Eu;   /* ~(CF|ZF|SF|OF); sign-extends, preserves PF/AF */
    const int D = d->dst, S = d->a_src;
    int T1, T2, T3;
    if (!pick3(D, S, &T1, &T2, &T3)) return 0;

    gb_lea_rsp(p, -128);                    /* step past red zone; no flag effect */
    gb_push(p, T1); gb_push(p, T2); gb_push(p, T3);
    gb_pushfq(p);                           /* [rsp] = ORIGINAL program flags (PF/AF source) */

    /* CF = (S == 0): capture BEFORE any arithmetic that clobbers flags */
    gb_xor(p, 1, T1, T1);                  /* T1 = 0 */
    gb_test(p, w, S, S);                    /* ZF = (S==0) [S not yet clobbered] */
    gb_setz(p, T1);                          /* T1 = CF_blsr = (S==0) */

    /* Compute result: T2 = S; T2 = (T2-1) & S  [reads original S; D not yet written] */
    gb_mov(p, w, T2, S);                    /* T2 = S (dst==src aliasing: src read first) */
    gb_dec(p, w, T2);                        /* T2 = S - 1 */
    gb_and(p, w, T2, S);                    /* T2 = (S-1) & S */

    /* Write result to D (may alias S; T2 has the result so S is no longer needed) */
    gb_mov(p, w, D, T2);                    /* D = result; opsize32 zero-extends to 64 */

    emit_zfsf_bits(p, w, D, T1, T2, T3);   /* T1 = CF|(ZF<<6)|(SF<<7) */

    gb_and_rsp_imm32(p, CLR);              /* clear CF|ZF|SF|OF in saved flags */
    gb_or_rsp_reg(p, T1);                  /* OR in computed owned bits */
    gb_popfq(p);                            /* live flags = patched original */
    gb_pop(p, T3); gb_pop(p, T2); gb_pop(p, T1);
    gb_lea_rsp(p, 128);                     /* restore rsp above red zone; no flag effect */
    if (!gb_jmp_abs(p, resume)) return 0;
    return 1;
}

static int minspill_blsi_supported(const decoded *d){
    if (!d->is_bmi || d->op != BMI_BLSI) return 0;
    if (d->dst_kind != DST_GPR) return 0;
    if (!(d->opsize == 32 || d->opsize == 64)) return 0;
    if (d->seg != 0) return 0;
    if (d->a_src == OPND_MEM || d->dst_kind == DST_MEM) return 0;
    if (d->dst  < 0 || d->dst  > 15) return 0;
    if (d->a_src < 0 || d->a_src > 15) return 0;
    if (d->bmi_dst2 != OPND_NONE || d->bmi_s1_rdx) return 0;
    if (d->dst == 4 || d->a_src == 4) return 0;
    return 1;
}

static int emit_minspill_blsi(uint8_t **p, const decoded *d, uint64_t resume){
    if (!minspill_blsi_supported(d)) return 0;
    const int w = (d->opsize == 64);
    const uint32_t CLR = 0xFFFFF73Eu;
    const int D = d->dst, S = d->a_src;
    int T1, T2, T3;
    if (!pick3(D, S, &T1, &T2, &T3)) return 0;

    gb_lea_rsp(p, -128);
    gb_push(p, T1); gb_push(p, T2); gb_push(p, T3);
    gb_pushfq(p);

    /* CF = (S != 0): BLSI sets CF when source is non-zero */
    gb_xor(p, 1, T1, T1);                  /* T1 = 0 */
    gb_test(p, w, S, S);                    /* ZF = (S==0) */
    gb_setnz(p, T1);                         /* T1 = CF_blsi = (S!=0) */

    /* Compute result: T2 = S; neg T2; and T2, S */
    gb_mov(p, w, T2, S);                    /* T2 = S (aliasing safety: read before write to D) */
    gb_neg(p, w, T2);                        /* T2 = 0 - T2 = -S */
    gb_and(p, w, T2, S);                    /* T2 = (-S) & S (reads original S; D not yet written) */

    gb_mov(p, w, D, T2);                    /* D = result */

    emit_zfsf_bits(p, w, D, T1, T2, T3);

    gb_and_rsp_imm32(p, CLR);
    gb_or_rsp_reg(p, T1);
    gb_popfq(p);
    gb_pop(p, T3); gb_pop(p, T2); gb_pop(p, T1);
    gb_lea_rsp(p, 128);
    if (!gb_jmp_abs(p, resume)) return 0;
    return 1;
}

static int minspill_blsmsk_supported(const decoded *d){
    if (!d->is_bmi || d->op != BMI_BLSMSK) return 0;
    if (d->dst_kind != DST_GPR) return 0;
    if (!(d->opsize == 32 || d->opsize == 64)) return 0;
    if (d->seg != 0) return 0;
    if (d->a_src == OPND_MEM || d->dst_kind == DST_MEM) return 0;
    if (d->dst  < 0 || d->dst  > 15) return 0;
    if (d->a_src < 0 || d->a_src > 15) return 0;
    if (d->bmi_dst2 != OPND_NONE || d->bmi_s1_rdx) return 0;
    if (d->dst == 4 || d->a_src == 4) return 0;
    return 1;
}

static int emit_minspill_blsmsk(uint8_t **p, const decoded *d, uint64_t resume){
    if (!minspill_blsmsk_supported(d)) return 0;
    const int w = (d->opsize == 64);
    const uint32_t CLR = 0xFFFFF73Eu;
    const int D = d->dst, S = d->a_src;
    int T1, T2, T3;
    if (!pick3(D, S, &T1, &T2, &T3)) return 0;

    gb_lea_rsp(p, -128);
    gb_push(p, T1); gb_push(p, T2); gb_push(p, T3);
    gb_pushfq(p);

    /* CF = (S == 0): BLSMSK sets CF when source is zero (same as BLSR) */
    gb_xor(p, 1, T1, T1);                  /* T1 = 0 */
    gb_test(p, w, S, S);                    /* ZF = (S==0) */
    gb_setz(p, T1);                          /* T1 = CF_blsmsk = (S==0) */

    /* Compute result: T2 = S; dec T2; xor T2, S */
    gb_mov(p, w, T2, S);                    /* T2 = S (aliasing safety) */
    gb_dec(p, w, T2);                        /* T2 = S - 1 */
    gb_xor(p, w, T2, S);                    /* T2 = (S-1) ^ S (reads original S; D not yet written) */

    gb_mov(p, w, D, T2);                    /* D = result */

    emit_zfsf_bits(p, w, D, T1, T2, T3);

    gb_and_rsp_imm32(p, CLR);
    gb_or_rsp_reg(p, T1);
    gb_popfq(p);
    gb_pop(p, T3); gb_pop(p, T2); gb_pop(p, T1);
    gb_lea_rsp(p, 128);
    if (!gb_jmp_abs(p, resume)) return 0;
    return 1;
}

/* ==========================================================================
 * Minimal-spill, LIVE-REGISTER ANDN thunk.
 *
 * ANDN dst, src1, src2:  dst = (~src1) & src2
 *   CF=0; ZF=(result==0); SF=sign bit of result; OF=0; PF/AF preserved.
 *
 * decode layout: d->a_src=src1(VEX.vvvv), d->b_src=src2(ModRM.rm), d->dst=reg.
 * This is a TWO-source op (unlike BLSR/BLSI/BLSMSK which are single-source).
 * bmi_exec: s1=rf->gpr[a_src], s2=rf->gpr[b_src]; result = maskz(~s1 & s2, opsize).
 *
 * Lowering (base ISA only — no ANDN, no BMI1):
 *   - pushfq BEFORE math (preserve PF/AF in saved image; NOT does not affect flags
 *     at all, but AND and XOR do clobber PF/AF, so we save early).
 *   - Read src1 into T1 BEFORE writing dst (dst==src1 and dst==src2 aliasing safe).
 *   - NOT T1 → T1 = ~src1  (NOT does NOT touch flags — safe before pushfq's image).
 *   - AND T1, src2(live) → T1 = (~src1) & src2  (src2 read before D written).
 *   - MOV D, T1  → dst = result (32-bit zero-extends to 64).
 *   - T1 = 0  (CF=0 for ANDN; this clobbers live flags but emit_zfsf_bits re-sets them).
 *   - emit_zfsf_bits: test D,D + setz T2 (ZF) + sets T3 (SF) → T1 = ZF<<6 | SF<<7.
 *   - Patch saved flags: CLR = ~(CF|ZF|SF|OF); OR in T1 (CF=OF=0, ZF/SF from result).
 *
 * Three scratch GPRs: first three ids not in {D, S1, S2, rsp=4}. With at most 4
 * distinct exclusions, 12+ candidates remain — always at least 3. No rcx/dst==rcx
 * constraint (no variable shift needed; NOT and AND are not CL-dependent).
 * Decline: any of dst, src1, src2 == rsp(4) (we move rsp for the stack frame).
 * ========================================================================== */
static int minspill_andn_supported(const decoded *d){
    if (!d->is_bmi || d->op != BMI_ANDN) return 0;
    if (d->dst_kind != DST_GPR) return 0;
    if (!(d->opsize == 32 || d->opsize == 64)) return 0;
    if (d->seg != 0) return 0;
    if (d->a_src == OPND_MEM || d->b_src == OPND_MEM || d->dst_kind == DST_MEM) return 0;
    if (d->dst  < 0 || d->dst  > 15) return 0;
    if (d->a_src < 0 || d->a_src > 15) return 0;
    if (d->b_src < 0 || d->b_src > 15) return 0;
    if (d->bmi_dst2 != OPND_NONE || d->bmi_s1_rdx) return 0;
    if (d->dst == 4 || d->a_src == 4 || d->b_src == 4) return 0;  /* rsp -> fall back */
    return 1;
}

static int emit_minspill_andn(uint8_t **p, const decoded *d, uint64_t resume){
    if (!minspill_andn_supported(d)) return 0;
    const int w = (d->opsize == 64);
    const uint32_t CLR = 0xFFFFF73Eu;   /* ~(CF|ZF|SF|OF); sign-extends, preserves PF/AF */
    const int D = d->dst, S1 = d->a_src, S2 = d->b_src;
    /* Three scratch GPRs: first three ids not in {D, S1, S2, rsp=4}. At most 4 distinct
     * exclusions from 16 regs leaves 12+ candidates — always at least 3 available. */
    int T1 = -1, T2 = -1, T3 = -1;
    for (int r = 0; r < 16 && T3 < 0; r++){
        if (r == D || r == S1 || r == S2 || r == 4) continue;
        if (T1 < 0) T1 = r; else if (T2 < 0) T2 = r; else T3 = r;
    }
    if (T1 < 0 || T2 < 0 || T3 < 0) return 0;

    gb_lea_rsp(p, -128);                 /* step past red zone; no flag effect */
    gb_push(p, T1); gb_push(p, T2); gb_push(p, T3);
    gb_pushfq(p);                        /* [rsp] = ORIGINAL program flags (PF/AF source) */

    /* Compute result into T1 BEFORE writing D (aliasing-safe: D may equal S1 or S2).
     * NOT does not affect flags, so the pushfq image remains the original; AND clobbers
     * flags but the image was already saved. */
    gb_mov(p, w, T1, S1);               /* T1 = src1 (read before D is written) */
    gb_not(p, w, T1);                    /* T1 = ~src1 (does NOT touch flags) */
    gb_and(p, w, T1, S2);              /* T1 = (~src1) & src2 (S2 not yet overwritten; D not written) */
    gb_mov(p, w, D, T1);               /* D = result; opsize32 zero-extends to 64 */

    /* CF=0 for ANDN: zero T1 so emit_zfsf_bits starts with CF contribution = 0.
     * This xor clobbers live flags, but emit_zfsf_bits begins with test D,D which
     * re-establishes ZF and SF from the result; the live flags after xor are discarded. */
    gb_xor(p, 1, T1, T1);              /* T1 = 0 (CF=0 ; OF=0 is not OR'd in) */
    emit_zfsf_bits(p, w, D, T1, T2, T3);  /* T1 = (ZF<<6)|(SF<<7) ; CF/OF stay 0 */

    /* Patch saved flags image: clear CF|ZF|SF|OF, OR in computed; preserve PF/AF/rest.
     * Since CF and OF were never set in T1, they remain 0 after the OR. */
    gb_and_rsp_imm32(p, CLR);
    gb_or_rsp_reg(p, T1);
    gb_popfq(p);                         /* live flags = patched original */
    gb_pop(p, T3); gb_pop(p, T2); gb_pop(p, T1);
    gb_lea_rsp(p, 128);                  /* restore rsp above red zone; no flag effect */
    if (!gb_jmp_abs(p, resume)) return 0;
    return 1;
}

/* ==========================================================================
 * Minimal-spill, LIVE-REGISTER MULX thunk (the hardest op in the set).
 *
 * MULX dlo, dhi, src : unsigned widening multiply. s1 = IMPLICIT rdx (gpr[2]);
 * s2 = src (rm register). Product = s1*s2 (opsize32 -> 64-bit; opsize64 -> 128-bit).
 * dlo (= d->dst) = LOW half; dhi (= d->bmi_dst2) = HIGH half. MULX affects NO flags.
 *
 * Base-ISA lowering uses `MUL T` ((r)dx:(r)ax = (r)ax * T), which clobbers rax, rdx
 * AND flags — but MULX must leave every register except dlo/dhi unchanged and touch
 * no flags. So the thunk:
 *   - pushfq/popfq around everything (MUL sets CF/OF; MULX defines none).
 *   - saves rax and rdx to the stack IF they are not in {dlo,dhi} (they legitimately
 *     receive the product when they ARE dests); restores in LIFO order.
 *   - reads s2 (src) into scratch T BEFORE clobbering rax (src may itself be rax/rdx).
 *   - moves the live rdx (s1) into rax, then MUL T -> rdx:rax = s1*s2.
 *   - shuffles low(rax)/high(rdx) into dlo/dhi using T as a temp so NO product half is
 *     lost under ANY {dlo,dhi} x {rax,rdx} aliasing (incl. the swapped dlo==rdx &
 *     dhi==rax): T=high; dlo=low; dhi=T. T is chosen distinct from rax/rdx/dlo/dhi,
 *     so all three moves are safe regardless of aliasing. dhi is written LAST so
 *     the dlo==dhi form keeps the HIGH half (hardware writes DEST2=lo then DEST1=hi).
 *   - opsize32: 32-bit MUL zero-extends eax/edx to the full 64-bit regs (matches
 *     bmi_exec's (u32)p / (u32)(p>>32)); opsize64: 64-bit MUL.
 * Declines rsp (4) as dlo/dhi/src (the thunk uses rsp as its working stack).
 * ========================================================================== */
static int minspill_mulx_supported(const decoded *d){
    if (!d->is_bmi || d->op != BMI_MULX) return 0;
    if (!d->bmi_s1_rdx) return 0;                    /* s1 must be implicit rdx */
    if (d->dst_kind != DST_GPR) return 0;
    if (!(d->opsize == 32 || d->opsize == 64)) return 0;
    if (d->seg != 0) return 0;
    if (d->b_src == OPND_MEM || d->dst_kind == DST_MEM) return 0;  /* src reg only */
    if (d->dst      < 0 || d->dst      > 15) return 0;  /* dlo */
    if (d->bmi_dst2 < 0 || d->bmi_dst2 > 15) return 0;  /* dhi */
    if (d->b_src    < 0 || d->b_src    > 15) return 0;  /* src (s2) */
    if (d->dst == 4 || d->bmi_dst2 == 4 || d->b_src == 4) return 0;  /* rsp -> fall back */
    return 1;
}

static int emit_minspill_mulx(uint8_t **p, const decoded *d, uint64_t resume){
    if (!minspill_mulx_supported(d)) return 0;
    const int w = (d->opsize == 64);
    const int RAX = 0, RDX = 2;
    const int DLO = d->dst, DHI = d->bmi_dst2, SRC = d->b_src;
    /* One scratch T: first id not in {rax, rdx, dlo, dhi, rsp}. At most 5 distinct
     * exclusions from 16 regs => always available. T may equal SRC (src is only read,
     * and if T==SRC it is not a dest, so the pop below restores it). */
    int T = -1;
    for (int r = 0; r < 16; r++){ if (r==RAX||r==RDX||r==DLO||r==DHI||r==4) continue; T = r; break; }
    if (T < 0) return 0;
    const int save_rax = (RAX != DLO && RAX != DHI);   /* preserve rax if not a dest */
    const int save_rdx = (RDX != DLO && RDX != DHI);   /* preserve rdx if not a dest */

    gb_lea_rsp(p, -128);              /* step past red zone; lea has no flag effect */
    gb_pushfq(p);                     /* save ALL flags (MUL clobbers; MULX defines none) */
    if (save_rax) gb_push(p, RAX);
    if (save_rdx) gb_push(p, RDX);
    gb_push(p, T);

    gb_mov(p, w, T, SRC);             /* T = s2 (read src BEFORE rax is clobbered; src may be rax/rdx) */
    gb_mov(p, w, RAX, RDX);           /* rax = s1 = live rdx (rdx still holds its value) */
    gb_mul_reg(p, w, T);              /* rdx:rax = s1 * s2 (rax=low, rdx=high; clobbers flags) */
    /* Place low/high into dlo/dhi via T so no half is lost under any aliasing
     * (incl. swapped dlo==rdx & dhi==rax). T != rax/rdx/dlo/dhi by construction.
     * ORDER MATTERS: low first, high LAST — hardware writes DEST2(lo) then
     * DEST1(hi), so dlo==dhi keeps the HIGH half (real 2.1.185 code uses
     * `mulx rax,rax,rax`-style forms to take just the high half). */
    gb_mov(p, 1, T,   RDX);           /* T   = high (rdx may be dlo — save it first) */
    gb_mov(p, 1, DLO, RAX);           /* dlo = low  (rax already zero-extended for opsize32) */
    gb_mov(p, 1, DHI, T);             /* dhi = high (written LAST: dlo==dhi -> high wins) */

    gb_pop(p, T);
    if (save_rdx) gb_pop(p, RDX);     /* reverse LIFO order */
    if (save_rax) gb_pop(p, RAX);
    gb_popfq(p);                      /* restore original flags unchanged (MULX defines none) */
    gb_lea_rsp(p, 128);
    if (!gb_jmp_abs(p, resume)) return 0;
    return 1;
}

/* Dispatch: emit whichever supported minimal-spill op `d` is. Returns 1 on emit. */
static int emit_minspill_op(uint8_t **p, const decoded *d, uint64_t resume){
    if (minspill_lzcnt_supported(d))  return emit_minspill_lzcnt(p, d, resume);
    if (minspill_shlx_supported(d))   return emit_minspill_shlx(p, d, resume);
    if (minspill_tzcnt_supported(d))  return emit_minspill_tzcnt(p, d, resume);
    if (minspill_shrx_supported(d))   return emit_minspill_shrx(p, d, resume);
    if (minspill_sarx_supported(d))   return emit_minspill_sarx(p, d, resume);
    if (minspill_rorx_supported(d))   return emit_minspill_rorx(p, d, resume);
    if (minspill_bzhi_supported(d))   return emit_minspill_bzhi(p, d, resume);
    if (minspill_blsr_supported(d))   return emit_minspill_blsr(p, d, resume);
    if (minspill_blsi_supported(d))   return emit_minspill_blsi(p, d, resume);
    if (minspill_blsmsk_supported(d)) return emit_minspill_blsmsk(p, d, resume);
    if (minspill_andn_supported(d))   return emit_minspill_andn(p, d, resume);
    if (minspill_mulx_supported(d))   return emit_minspill_mulx(p, d, resume);
    return 0;
}
static int minspill_supported(const decoded *d){
    return minspill_lzcnt_supported(d)  || minspill_shlx_supported(d)  || minspill_tzcnt_supported(d)
        || minspill_shrx_supported(d)   || minspill_sarx_supported(d)  || minspill_rorx_supported(d)
        || minspill_bzhi_supported(d)   || minspill_blsr_supported(d)
        || minspill_blsi_supported(d)   || minspill_blsmsk_supported(d)
        || minspill_andn_supported(d)   || minspill_mulx_supported(d);
}

/* Emit a minimal-spill thunk for any supported op into the RWX pool; return its
 * entry (the jmp site target), or NULL if unsupported / pool exhausted. Emits
 * DIRECTLY into the pool (the trailing jmp rel32 is position-dependent). Non-static
 * so the differential test can exercise the emitter in isolation. */
void *avxemu_emit_minspill_block(const decoded *d, uint64_t resume){
    if (!minspill_supported(d)) return 0;
    uint8_t *blk = avxemu_pool_alloc(256);     /* thunk is < 256 bytes (bzhi uses ~170) */
    if (!blk) return 0;
    uint8_t *p = blk;
    if (!emit_minspill_op(&p, d, resume)) return 0;
    return blk;
}

/* AVXEMU_MINSPILL: on by default; AVXEMU_MINSPILL=0 turns it off, falling back to
 * the selection below it (native, then C dispatch). */
static int minspill_enabled(void){ const char *e = getenv("AVXEMU_MINSPILL"); return !(e && e[0] == '0'); }

/* Minimal-spill thunk for a single supported op (lzcnt / shlx). Returns the entry,
 * or 0 (caller falls back to the existing full-spill selection — no regression).
 * Single-op only for now: 78% of trampolined runs are single-instruction;
 * multi-op minimal runs are a later op-set task. */
void *avxemu_build_thunk_minspill(const tramp_insn *insns, int n, uint64_t resume){
    if (n != 1) return 0;
    if (!minspill_supported(&insns[0].dec)) return 0;
    return avxemu_emit_minspill_block(&insns[0].dec, resume);
}

/* Emit the native scalar-GPR `block` for a run into the RWX pool. Returns the
 * block entry, or NULL if any instruction is unsupported or the pool is
 * exhausted. Bypasses the AVXEMU_NATIVE gate (the gate is in the thunk builder)
 * so the differential test can always exercise it. */
void *avxemu_emit_native_block_bmi(const tramp_insn *insns, int n){
    if (n <= 0) return 0;
    for (int i = 0; i < n; i++) if (!bmi_block_op_supported(&insns[i].dec)) return 0;
    uint8_t buf[MAXRUN * 160];
    uint8_t *p = buf;
    for (int i = 0; i < n; i++){
        if (!gb_emit_bmi(&p, &insns[i].dec)) return 0;
        if ((size_t)(p - buf) > sizeof buf - 128) return 0;   /* overflow guard */
    }
    nb(&p, 0xC3);   /* ret */
    size_t sz = (size_t)(p - buf);
    uint8_t *blk = avxemu_pool_alloc(sz);
    if (!blk) return 0;
    memcpy(blk, buf, sz);
    return blk;
}

/* True iff every instruction in the run is a scalar-GPR op the native block
 * supports (currently: register-operand LZCNT). */
static int run_is_native_bmi(const tramp_insn *insns, int n){
    for (int i = 0; i < n; i++) if (!bmi_block_op_supported(&insns[i].dec)) return 0;
    return n > 0;
}

/* Native scalar-GPR thunk: a tt2-template thunk (GPR+flags spill, NO vector save
 * — correct since LZCNT touches no vector reg) whose dispatch pointer is the
 * emitted scalar block. Returns NULL (caller falls back) if disabled, any op is
 * unsupported, or the pool is exhausted. */
void *avxemu_build_thunk_native_bmi(const tramp_insn *insns, int n, uint64_t resume){
    if (!native_enabled()) return 0;
    void *block = avxemu_emit_native_block_bmi(insns, n);
    if (!block) return 0;
    return build_thunk_t(insns, n, resume, avxemu_tt2_start, avxemu_tt2_record,
                         avxemu_tt2_dispatchptr, avxemu_tt2_resumeptr, block);
}

/* Scan one cleanly-decoding function and trampoline its faulting runs. Returns
 * the number of jmps written. Patches go to the live (already-writable) text. */
/* Place the trampoline for one gathered run of faulting instructions: pick the
 * first start whose 5-byte jmp can't corrupt a branch target, build a thunk for
 * [s,ni), and overwrite that start with `jmp thunk`. Returns 1 if patched. */
/* Immutable metadata used by recording and runtime patch-safety checks. */
static const avxemu_image *g_patch_image;
static int place_run(uint8_t *text, const tramp_insn *ri, int rn, size_t site_off, size_t re);
static long emit_run(uint8_t *text, size_t fstart, size_t fend,
                     const tramp_insn *insns, const size_t *offs, int ni,
                     size_t re, const uint8_t *tgt, int has_indirect) {
    int s = 0;
    while (s < ni) {
        size_t soff = offs[s]; int slen = insns[s].dec.len;
        if (re - soff < 5) { s = ni; break; }            /* can't fit a 5-byte jmp -> rest traps */
        if (slen >= 5) break;                            /* jmp lands inside insn[s]: always safe */
        size_t nb = soff + 4;                            /* slen==4: next insn start is in the jmp */
        if (!has_indirect && (nb >= fend || !tgt[nb - fstart])) break;
        s++;                                              /* unsafe 4-byte start -> it traps; try next */
    }
    if (s < ni && re - offs[s] >= 5) {
        const tramp_insn *ri = insns + s; int rn = ni - s;
        if (g_nstats) nstats_tally(ri, rn);   /* AVXEMU_NATIVE_STATS diagnostic */
        if (place_run(text, ri, rn, offs[s], re)) {
            uint32_t range; size_t off;
            if (avxemu_image_find(g_patch_image, (uintptr_t)(text + offs[s]), 5, &range, &off)) {
                lc_site site = {range, 0, off}, resume = {range, 0, re};
                avxemu_lc_rec_run(site, (uint32_t)rn, resume);
            } else avxemu_lc_rec_fail();
            return 1;
        }
    }
    return 0;
}

/* Pick the thunk tier for the run ri[0..rn), which resumes at res, and build it in
 * the pool. NULL if none could be built. Non-static so minspilltest can check which
 * tier each AVXEMU_MINSPILL setting selects. */
void *avxemu_select_thunk(const tramp_insn *ri, int rn, uint64_t res) {
    /* AVXEMU_MINSPILL (default ON): a minimal-spill live-register thunk for a single
     * supported register-operand BMI op -- highest priority. On NULL (gate off, not a
     * single supported op, an rsp operand, or pool exhausted) fall through to the
     * selection below. AVXEMU_MINSPILL=0 gives exactly that selection. */
    void *thunk = (minspill_enabled() && !g_force_full)
                      ? avxemu_build_thunk_minspill(ri, rn, res) : 0;
    /* Milestone B: register-resident native-SSE thunk (gated by AVXEMU_NATIVE,
     * default ON). NULL -> fall back to the existing C-dispatch selection. */
    if (!thunk) thunk = g_force_full ? 0 : avxemu_build_thunk_native(ri, rn, res);
    /* Task 2: scalar-GPR native (register-only LZCNT, MULX) via the tt2 thunk, also
     * gated by AVXEMU_NATIVE. The vector native attempt above declines BMI ops, so
     * this runs next. NULL (gate off, op outside the scalar set, or pool exhausted)
     * -> existing C thunk selection. No correctness risk. */
    if (!thunk && !g_force_full && run_is_native_bmi(ri, rn))
        thunk = avxemu_build_thunk_native_bmi(ri, rn, res);
    if (!thunk)
        thunk = g_force_full              ? avxemu_build_thunk(ri, rn, res)
              : run_is_regonly_bmi(ri, rn) ? build_thunk_bmi(ri, rn, res)
              : run_is_gpr_only(ri, rn)    ? build_thunk_gpr(ri, rn, res)
              :                              avxemu_build_thunk(ri, rn, res);
    return thunk;
}

/* Build the thunk for the run ri[0..rn), which starts at site_off and resumes at
 * re (both relative to text), and point the site at it. Returns 1 if patched.
 * The analysis (emit_run) and the lcache replay (replay_runs) both end here, so
 * a replayed trampoline is the one the analysis would have built. The site was
 * already chosen branch-target-safe by the caller. */
static int place_run(uint8_t *text, const tramp_insn *ri, int rn, size_t site_off, size_t re) {
    uint32_t range; size_t offset;
    if(re<site_off || !avxemu_image_find(g_patch_image,(uintptr_t)(text+site_off),re-site_off,&range,&offset) ||
        avxemu_image_data_overlap(g_patch_image,range,offset,re-site_off)) return 0;
    {
        uint64_t res = (uint64_t)(text + re);
        void *thunk = avxemu_select_thunk(ri, rn, res);
        if (thunk) {
            uint8_t *site = text + site_off;
            int64_t rel = (int64_t)((uint8_t *)thunk - (site + 5));
            if (rel >= INT32_MIN && rel <= INT32_MAX) {
                site[0] = 0xE9; int32_t r32 = (int32_t)rel; memcpy(site + 1, &r32, 4);
                g_ns_placed++;
                return 1;
            }
        }
    }
    return 0;
}

/* Gather the maximal run of physically-consecutive faulting instructions at q
 * (each must also be reachable code when `code` is given), fill insns/offs, and
 * return the run-end offset. Faulting ops never branch, so the run always falls
 * through to real code at the returned offset. */
static size_t gather_run(uint8_t *text, size_t fstart, size_t fend, size_t q,
                         const uint8_t *code, tramp_insn *insns, size_t *offs, int *pni) {
    int ni = 0; size_t p = q;
    while (p < fend && ni < MAXRUN && (!code || code[p - fstart])) {
        int z2, oo; int l2 = x86_len(text + p, text + fend, &z2, &oo);
        if (l2 <= 0) break;
        decoded d2; int dl2 = decode(text + p, &d2);
        if (!(dl2 > 0 && d2.op && tramp_faults(&d2))) break;
        offs[ni] = p; insns[ni].addr = (uint64_t)(text + p); insns[ni].dec = d2;
        ni++; p += l2;
    }
    *pni = ni;
    return p;
}

/* ---- bounded jump-table resolution (fault-storm fix) ------------------------
 * The dominant repeat-faulting sites live in functions whose only indirect jmps
 * are LLVM switch dispatches:
 *     cmp  rI, imm          ; unsigned bound: valid index 0..imm
 *     ja   <default>        ; forward, past the dispatch
 *     lea  rB, [rip+d32]    ; table base (rel32-entry table, usually inline)
 *     movsxd rD, [rB+rI*4]  ; load rel32 entry
 *     add  rD, rB           ; absolute target
 *     jmp  *rD
 * That target set is STATICALLY ENUMERABLE: entries e_i at [tbl, tbl+4*(imm+1)),
 * targets tbl+e_i. resolve_jump_table() matches the pattern against the last few
 * decoded instructions ending at the `jmp *rD` and, on an EXACT match, marks
 * every in-function target in tgt[] and records the inline table bytes as data
 * (so the linear walk skips them instead of desyncing). ANY mismatch returns 0
 * and the caller keeps the blanket has_indirect decline — the safe floor.
 * Index bound: cmp+ja when present; else a `movzx rI, r/m8` defining the index
 * within the window bounds it to 256. No bound => no resolution. ------------- */
#define JT_RING 8            /* lookback: cmp,ja,lea,movsxd,add precede the jmp */
#define JT_MAX_ENTRIES 16384
#define JT_MAX_TABLES 16
typedef struct { size_t lo, hi; } jt_range;

/* modrm/rex field helpers over a single-instruction byte view */
static int jt_rex(const uint8_t *p, int *rex){   /* optional single REX; no other prefixes */
    if ((p[0] & 0xF0) == 0x40) { *rex = p[0]; return 1; }
    *rex = 0; return 0;
}
static int resolve_jump_table(uint8_t *text, size_t fstart, size_t fend,
                              const size_t *ring, int nring, size_t jq, int jlen,
                              uint8_t *tgt, jt_range *tab, int *ntab) {
    if (nring < 3) return 0;
    /* jmp *rD : [rex] FF /4 mod=11 */
    { const uint8_t *p = text + jq; int rex, k = jt_rex(p, &rex);
      if (p[k] != 0xFF || ((p[k+1] >> 3) & 7) != 4 || (p[k+1] >> 6) != 3) return 0;
      if (k + 2 != jlen) return 0;
      int rD = (p[k+1] & 7) | ((rex & 1) << 3);

      /* prev1: add rD, rB  (01 /r: dst=rm ; 03 /r: dst=reg) */
      const uint8_t *a = text + ring[nring-1]; int arex, ak = jt_rex(a, &arex);
      int rB;
      if (a[ak] == 0x01 && (a[ak+1] >> 6) == 3) {
          int rm  = (a[ak+1] & 7)        | ((arex & 1) << 3);
          int reg = ((a[ak+1] >> 3) & 7) | ((arex & 4) << 1);
          if (rm != rD) return 0;
          rB = reg;
      } else if (a[ak] == 0x03 && (a[ak+1] >> 6) == 3) {
          int reg = ((a[ak+1] >> 3) & 7) | ((arex & 4) << 1);
          int rm  = (a[ak+1] & 7)        | ((arex & 1) << 3);
          if (reg != rD) return 0;
          rB = rm;
      } else return 0;

      /* prev2: movsxd rD, [rB + rI*4]  (63 /r mod=00 rm=100, SIB scale=2) */
      const uint8_t *m = text + ring[nring-2]; int mrex, mk = jt_rex(m, &mrex);
      if (m[mk] != 0x63 || (m[mk+1] >> 6) != 0 || (m[mk+1] & 7) != 4) return 0;
      { int reg = ((m[mk+1] >> 3) & 7) | ((mrex & 4) << 1);
        if (reg != rD) return 0; }
      if ((m[mk+2] >> 6) != 2) return 0;                       /* scale = *4 */
      int rI = ((m[mk+2] >> 3) & 7) | ((mrex & 2) << 2);
      { int base = (m[mk+2] & 7) | ((mrex & 1) << 3);
        if (base != rB || (m[mk+2] & 7) == 5) return 0; }      /* base==101 needs disp */

      /* prev3: lea rB, [rip+d32]  (8D /r mod=00 rm=101) -> table offset */
      const uint8_t *l = text + ring[nring-3]; int lrex, lk = jt_rex(l, &lrex);
      if (l[lk] != 0x8D || (l[lk+1] >> 6) != 0 || (l[lk+1] & 7) != 5) return 0;
      { int reg = ((l[lk+1] >> 3) & 7) | ((lrex & 4) << 1);
        if (reg != rB) return 0; }
      if (rB == rI || rB == rD) return 0;   /* aliased base would corrupt the pattern */
      int32_t d32; memcpy(&d32, l + lk + 2, 4);
      size_t lea_end = ring[nring-3] + (size_t)lk + 6;
      long tbl_l = (long)lea_end + d32;
      if (tbl_l < (long)fstart || (size_t)tbl_l >= fend) return 0;   /* table must be in-function */
      size_t tbl = (size_t)tbl_l;

      /* Index bound: `cmp rI,imm` guarded by a forward ja past the jmp, or a
       * `movzx rI, r/m8` (bounds the index to 0..255). SOUNDNESS: every ring
       * instruction between the bounding insn and the movsxd must be one we
       * fully understand as not writing rI — only the guarding ja and the lea
       * (which writes rB != rI) qualify. Anything else => unproven => fail. */
      long n_ent = -1; int ja_seen = 0;
      for (int h = nring - 4; h >= 0; h--) {
          const uint8_t *c = text + ring[h]; int crex, ck = jt_rex(c, &crex);
          /* guarding ja: part of the gap; note whether it jumps past the dispatch */
          if (c[0] == 0x77) {
              if ((long)ring[h] + 2 + (int8_t)c[1] > (long)jq) ja_seen = 1;
              continue;
          }
          if (c[0] == 0x0F && c[1] == 0x87) {
              int32_t v; memcpy(&v, c + 2, 4);
              if ((long)ring[h] + 6 + v > (long)jq) ja_seen = 1;
              continue;
          }
          long imm = -1;
          if (c[ck] == 0x83 && (c[ck+1] >> 6) == 3 && ((c[ck+1] >> 3) & 7) == 7
              && ((c[ck+1] & 7) | ((crex & 1) << 3)) == rI)
              imm = (int8_t)c[ck+2];
          else if (c[ck] == 0x81 && (c[ck+1] >> 6) == 3 && ((c[ck+1] >> 3) & 7) == 7
                   && ((c[ck+1] & 7) | ((crex & 1) << 3)) == rI)
              { int32_t v; memcpy(&v, c + ck + 2, 4); imm = v; }
          else if (c[ck] == 0x3D && rI == 0)
              { int32_t v; memcpy(&v, c + ck + 1, 4); imm = v; }
          else if (c[ck] == 0x0F && c[ck+1] == 0xB6
                   && (((c[ck+2] >> 3) & 7) | ((crex & 4) << 1)) == rI)
              { n_ent = 256; break; }        /* movzx 8-bit index: bound needs no ja */
          if (imm < 0) return 0;             /* unknown insn in the gap -> unproven */
          if (!ja_seen || imm < 0) return 0; /* cmp without a guarding ja -> unproven */
          n_ent = imm + 1;
          break;
      }
      if (n_ent <= 0 || n_ent > JT_MAX_ENTRIES) return 0;
      if (tbl + 4u * (size_t)n_ent > fend) return 0;           /* table must fit in-function */
      if (*ntab >= JT_MAX_TABLES) return 0;

      /* mark every in-function target; record the table as data to skip */
      for (long i = 0; i < n_ent; i++) {
          int32_t e; memcpy(&e, text + tbl + 4 * (size_t)i, 4);
          long t = (long)tbl + e;
          if (t >= (long)fstart && t < (long)fend) tgt[t - fstart] = 1;
      }
      tab[*ntab].lo = tbl; tab[*ntab].hi = tbl + 4u * (size_t)n_ent; (*ntab)++;
      return 1;
    }
}

/* Pass-1 branch-target collection over one function [fstart,fend) (offsets into
 * text). Marks tgt[off-fstart]=1 at every in-function direct branch target, and
 * sets *has_indirect if any UNRESOLVED indirect jmp is seen (bounded jump-table
 * dispatches are resolved: their targets are marked and their inline table bytes
 * skipped as data). Returns 1 if the function decoded cleanly to its boundary, 0
 * on a length-decode desync (caller treats a 0 conservatively). Shared by the
 * eager scanner (scan_function) and the runtime patch-safety check
 * (avxemu_patch_safe). tgt[] must be caller-allocated, length >= fend-fstart and
 * pre-zeroed. */
static int collect_branch_targets(uint8_t *text, size_t fstart, size_t fend,
                                  uint8_t *tgt, int *has_indirect) {
    *has_indirect = 0;
    size_t ring[JT_RING]; int nring = 0;
    jt_range tab[JT_MAX_TABLES]; int ntab = 0;
    size_t q = fstart;
    while (q < fend) {
        int skipped = 0;
        for (int i = 0; i < ntab; i++)
            if (q >= tab[i].lo && q < tab[i].hi) { q = tab[i].hi; skipped = 1; break; }
        if (skipped) { nring = 0; continue; }              /* table data: not code */
        if (fend - q <= 15) { int pad = 1; for (size_t r = q; r < fend; r++) if (text[r]&&text[r]!=0xCC&&text[r]!=0x90){pad=0;break;} if (pad) break; }
        int zk, o2; int len = x86_len(text + q, text + fend, &zk, &o2);
        if (len <= 0) return 0;
        int term; long t; int ind;
        lde_cflow(text + q, text + fend, len, (long)q, &term, &t, &ind);
        if (ind && !resolve_jump_table(text, fstart, fend, ring, nring, q, len, tgt, tab, &ntab))
            *has_indirect = 1;
        if (t >= (long)fstart && t < (long)fend) tgt[t - fstart] = 1;
        if (nring == JT_RING) { memmove(ring, ring + 1, (JT_RING - 1) * sizeof ring[0]); nring--; }
        ring[nring++] = q;
        q += len;
    }
    return 1;
}

/* Branch-target scratch for the patch-safety scan. avxemu_patch_safe now runs
 * INSIDE the SIGILL handler (on_sigill -> avxemu_relocate_block -> avxemu_patch_safe),
 * so it must NOT call malloc/free: those are not async-signal-safe and the fault
 * could have interrupted the allocator mid-update, risking deadlock. A fixed static
 * map sized to a generous max function span replaces the heap allocation. If a
 * containing function's span exceeds the cap we DECLINE (return 0, the safe floor).
 * Runtime callers hold handler.c's shared relocation lock while using this
 * scratch map and the pool cursor. */
#define PATCHSAFE_MAP_MAX (256u * 1024)
static uint8_t g_patchsafe_map[PATCHSAFE_MAP_MAX];

/* Is it safe to write a `jmplen`-byte jmp at `site`? Decline unless we can PROVE
 * no branch in the containing function targets the open interval (site, site+jmplen).
 * Conservative: decline if the layout is unknown, the site is outside scanned text,
 * the function won't decode cleanly, or it contains an unresolved indirect jmp
 * (whose target could land anywhere, including inside the jmp footprint). */
int avxemu_patch_safe(uint8_t *site, int jmplen) {
    if (jmplen <= 0) return 0;
    const avxemu_function *f = avxemu_image_function(g_patch_image, (uintptr_t)site);
    if (!f) return 0;
    const avxemu_code_range *range = &g_patch_image->ranges[f->range];
    size_t offset = (size_t)((uintptr_t)site - (uintptr_t)range->base);
    if ((size_t)jmplen > f->end - offset ||
        avxemu_image_data_overlap(g_patch_image,f->range,offset,(size_t)jmplen)) return 0;
    size_t fstart = f->begin, fend = f->end;
    size_t n = fend - fstart;
    if (n > PATCHSAFE_MAP_MAX) return 0;               /* span exceeds scratch cap -> decline (safe floor) */
    uint8_t *tgt = g_patchsafe_map;                    /* static scratch: no malloc in the handler */
    memset(tgt, 0, n);                                 /* collect_branch_targets needs a pre-zeroed map */
    int has_indirect = 0;
    int clean = collect_branch_targets(range->base, fstart, fend, tgt, &has_indirect);
    int safe = 1;
    if (!clean || has_indirect) {
        safe = 0;                                      /* can't fully account for targets */
    } else {
        size_t soff = offset - fstart;  /* site offset within function */
        for (int k = 1; k < jmplen; k++) {               /* open interval (site, site+jmplen) */
            size_t off = soff + (size_t)k;
            if (off < n && tgt[off]) { safe = 0; break; }
        }
    }
    return safe;
}

/* Explicit metadata hook for synthetic multi-range contract fixtures. */
void avxemu_patch_safe_test_image(const avxemu_image *image) { g_patch_image = image; }

/* Bounds used before relocation decodes or copies a legal tail. */
const uint8_t *avxemu_patch_limit(const uint8_t *site) {
    const avxemu_function *f = avxemu_image_function(g_patch_image, (uintptr_t)site);
    if(!f) return 0;
    const avxemu_code_range *r=&g_patch_image->ranges[f->range];
    size_t off=(size_t)(site-r->base), end=f->end;
    for(size_t i=0;i<g_patch_image->ndata;i++) {
        const avxemu_data_span *d=&g_patch_image->data[i];
        if(d->range==f->range && d->begin>off && d->begin<end) end=d->begin;
    }
    return r->base+end;
}

/* Test hook (reloctest): register a single synthetic function [base, base+size)
 * as the cached layout so avxemu_patch_safe can run against a unit-test buffer not
 * in the main image's __text. Production NEVER calls this — install_trampolines
 * uses the immutable main image metadata. */
void avxemu_patch_safe_test_region(uint8_t *base, size_t size) {
    static avxemu_code_range range;
    static avxemu_function function;
    static avxemu_image image;
    range = (avxemu_code_range){base, (uintptr_t)base, size, size, AVXEMU_PROT_R | AVXEMU_PROT_X};
    function = (avxemu_function){0, 0, size};
    image = (avxemu_image){&range, 1, &function, 1, 0, 0, 0, AVXEMU_SCAN_STRICT};
    g_patch_image = &image;
}

/*
 * Trampoline one function's faulting runs. A function that decodes cleanly
 * linearly is walked straight through; one that doesn't (embedded jump tables)
 * is mapped by recursive descent so its reachable code is covered too instead of
 * being skipped wholesale. Faulting (AVX2/BMI/FMA) instructions never branch, so
 * a run of physically-consecutive ones always falls through to real code — the
 * resume point is safe in both modes.
 */
static long scan_function(const avxemu_image *image, size_t fi, const avxemu_scan *scan) {
    const avxemu_function *f=&image->functions[fi];
    const avxemu_code_range *range=&image->ranges[f->range];
    uint8_t *text=range->base;
    size_t fstart=f->begin,fend=f->end,readable=range->readable;
    int recursive=image->strategy==AVXEMU_SCAN_STRICT;
    size_t n = fend - fstart;
    uint8_t *tgt = calloc(n, 1);
    if (!tgt) return 0;
    long patched = 0;

    /* pass 1: clean linear decode? collect direct-branch targets + indirect flag */
    int has_indirect = 0;
    int clean = !recursive && collect_branch_targets(text, fstart, fend, tgt, &has_indirect);

    if (clean) {
        /* pass 2: walk linearly; trampoline each maximal faulting run */
        size_t q = fstart;
        while (q < fend) {
            if (fend - q <= 15) { int pad = 1; for (size_t r = q; r < fend; r++) if (text[r]&&text[r]!=0xCC&&text[r]!=0x90){pad=0;break;} if (pad) break; }
            int zk, o2; int len = x86_len(text + q, text + fend, &zk, &o2);
            if (len <= 0) break;
            decoded d; int dl = decode(text + q, &d);
            if (!(dl > 0 && d.op && tramp_faults(&d))) { q += len; continue; }
            tramp_insn insns[MAXRUN]; size_t offs[MAXRUN]; int ni;
            size_t re = gather_run(text, fstart, fend, q, 0, insns, offs, &ni);
            patched += emit_run(text, fstart, fend, insns, offs, ni, re, tgt, has_indirect);
            q = re;
        }
    } else {
        /* dirty: recursive-descent reachability map, then trampoline faulting runs
         * inside reachable code only (jump-table data is never visited or patched). */
        uint8_t *code = calloc(n, 1);
        memset(tgt, 0, n);                               /* linear targets past the desync are bogus */
        int mapped=0;
        if(code && recursive) {
            const avxemu_scan_map *map=avxemu_scan_function(scan,fi);
            memcpy(code,map->code,n);memcpy(tgt,map->targets,n);
            has_indirect=map->indirect;mapped=1;
        } else if(code) mapped=lde_rd_map_traps(text,fstart,fend,readable,code,tgt,
            &has_indirect,avxemu_cpuid_trap);
        if (mapped) {
            size_t off = 0;
            while (off < n) {
                if (!code[off]) { off++; continue; }
                size_t qq = fstart + off;
                int zk, o2; int len = x86_len(text + qq, text + fend, &zk, &o2);
                decoded d; int dl = (len > 0) ? decode(text + qq, &d) : 0;
                if (!(dl > 0 && d.op && tramp_faults(&d))) { off++; continue; }
                tramp_insn insns[MAXRUN]; size_t offs[MAXRUN]; int ni;
                size_t re = gather_run(text, fstart, fend, qq, code, insns, offs, &ni);
                patched += emit_run(text, fstart, fend, insns, offs, ni, re, tgt, has_indirect);
                off = re - fstart;
            }
        }
        free(code);
    }
    free(tgt);
    return patched;
}

/* lcache replay: one recorded run's instructions, decoded afresh. It must still be
 * a contiguous run of faulting instructions that ends exactly at its resume
 * point, or the recording does not describe this program. out may be NULL. */
static int replay_decode(const avxemu_image *image, const lc_run *r, tramp_insn *out) {
    uint8_t *start = avxemu_lc_resolve(image, r->site, 5);
    uint8_t *resume = avxemu_lc_resolve(image, r->resume, 0);
    if (!start || !resume || r->site.range != r->resume.range || r->n == 0 ||
        r->n > MAXRUN || r->resume.off < r->site.off || r->resume.off - r->site.off < 5) return 0;
    if(avxemu_image_data_overlap(image,r->site.range,(size_t)r->site.off,
        (size_t)(r->resume.off-r->site.off))) return 0;
    uint8_t *p = start;
    for (uint32_t k = 0; k < r->n; k++) {
        if (p >= resume) return 0;
        int zk, oo; int l = x86_len(p, resume, &zk, &oo);
        if (l <= 0 || l > 15) return 0;
        /* Decode only a bounded instruction, with zero padding for the
         * emulator decoder's speculative ModRM/displacement reads. */
        uint8_t bytes[32] = {0};
        memcpy(bytes, p, (size_t)l);
        decoded d; int dl = decode(bytes, &d);
        if (dl != l || !d.op || !tramp_faults(&d)) return 0;
        if (out) { out[k].addr = (uint64_t)p; out[k].dec = d; }
        p += (size_t)l;
    }
    return p == resume;
}

/* Validate the complete stage across all ranges before placing its first jmp. */
static int replay_runs_valid(const avxemu_image *image, const avxemu_scan *scan) {
    const lc_run *r; uint32_t n = avxemu_lc_runs(&r);
    for (uint32_t i = 0; i < n; i++) {
        if (!replay_decode(image, &r[i], 0)) return 0;
        if(scan && !avxemu_scan_span(scan,(uintptr_t)avxemu_lc_resolve(image,r[i].site,5),
            (size_t)(r[i].resume.off-r[i].site.off))) return 0;
    }
    return 1;
}
static long replay_runs(const avxemu_image *image, uint32_t range) {
    const lc_run *r; uint32_t n = avxemu_lc_runs(&r);
    long total = 0;
    for (uint32_t i = 0; i < n; i++) if (r[i].site.range == range) {
        tramp_insn insns[MAXRUN];
        if (!replay_decode(image, &r[i], insns)) continue;
        if (g_nstats) nstats_tally(insns, (int)r[i].n);
        total += place_run(image->ranges[range].base, insns, (int)r[i].n,
                           (size_t)r[i].site.off, (size_t)r[i].resume.off);
    }
    return total;
}

/* Install trampolines over the main executable. Returns jmps written. */
long avxemu_install_trampolines(void) {
    const avxemu_image *image = avxemu_image_main();
    g_patch_image = image;
    detect_features();
    if (!g_lack_avx2 && !g_lack_fma && !g_lack_bmi && !g_lack_f16c) return 0;  /* nothing faults here */

    /* Diagnostics (env-gated): resolve flags now so the hot path sees them, and
     * arm the SIGUSR2 / atexit histogram dump if AVXEMU_OPHIST is set. */
    (void)nstats_enabled();
    if (ophist_enabled()){
        struct sigaction sa; memset(&sa, 0, sizeof sa);
        sa.sa_handler = ophist_sigusr2; sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR2, &sa, 0);
        atexit(ophist_dump);
    }

    /* per-thread side-stack key, created here (single-threaded) before any thunk runs */
    if (!g_side_key_ok && pthread_key_create(&g_side_key, 0) == 0) g_side_key_ok = 1;

    if (!image || !image->nranges) return 0;
    const avxemu_code_range *first = &image->ranges[0];
    uintptr_t hint = (uintptr_t)first->base + first->size;
    size_t page = avxemu_page_size();
    if (!page || hint > UINTPTR_MAX - 0x100000) return 0;
    hint = (hint + 0x100000) / page * page;
    if (!g_pool && !avxemu_pool_init((void *)hint, 96u << 20)) {
        avxemu_lc_rec_fail(); return 0;
    }
    avxemu_scan *scan=image->strategy==AVXEMU_SCAN_STRICT ?
        avxemu_scan_prepare(image,avxemu_cpuid_trap,AVXEMU_SCAN_WORK_LIMIT):0;
    if(image->strategy==AVXEMU_SCAN_STRICT && !scan) {
        if(avxemu_lc_mode==LC_REPLAY) avxemu_lc_invalid();
        avxemu_lc_rec_fail();return 0;
    }
    if (avxemu_lc_mode == LC_REPLAY && !replay_runs_valid(image,scan)) avxemu_lc_invalid();
    long total = 0;
    for (size_t ri = 0; ri < image->nranges; ri++) {
        const avxemu_code_range *range = &image->ranges[ri];
        size_t nsaved;
        avxemu_mapping *saved = avxemu_patch_write_begin((uintptr_t)range->base, range->size, &nsaved);
        if (!saved) {
            avxemu_lc_rec_fail(); continue;
        }
        if (avxemu_lc_mode == LC_REPLAY) {
            total += replay_runs(image, (uint32_t)ri);
        } else {
            for (size_t fi = 0; fi < image->nfunctions; fi++) {
                const avxemu_function *f = &image->functions[fi];
                if (f->range != ri || f->begin >= f->end || f->end > range->size) continue;
                total += scan_function(image,fi,scan);
            }
        }
        avxemu_patch_write_end(saved, nsaved);
    }

    avxemu_scan_free(scan);
    if (g_nstats){   /* AVXEMU_NATIVE_STATS run-composition report (stderr) */
        emit("=== AVXEMU_NATIVE_STATS (install-time run composition) ===\n");
        emit("native-accept runs\t"); as_u64(2, g_ns_accept); emit("\n");
        emit("native-decline runs\t"); as_u64(2, g_ns_decline); emit("\n");
        emit("single-insn runs\t"); as_u64(2, g_ns_single); emit("\n");
        emit("multi-insn runs\t"); as_u64(2, g_ns_multi); emit("\n");
        /* the runs above were built; this many jmps actually landed (a pool out
         * of rel32 reach of __text, as on macOS 15, places none) */
        emit("placed runs\t"); as_u64(2, g_ns_placed); emit("\n");
        int idx[VEX_OP_COUNT]; int m = 0;
        for (int i = 0; i < VEX_OP_COUNT; i++) if (g_ns_block_op[i]) idx[m++] = i;
        for (int i = 0; i < m; i++){ int best = i;
            for (int j = i + 1; j < m; j++) if (g_ns_block_op[idx[j]] > g_ns_block_op[idx[best]]) best = j;
            int t = idx[i]; idx[i] = idx[best]; idx[best] = t; }
        emit("-- first-blocking op in declined runs (desc) --\n");
        for (int i = 0; i < m; i++){ const char *nm = vex_op_name((vex_op)idx[i]);
            emit(nm); emit("\t"); as_u64(2, g_ns_block_op[idx[i]]); emit("\n"); }
    }
    return total;
}
