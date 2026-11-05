/*
 * semantics: every `tested`/`failing` manifest form is executed through the real decoder and
 * avxemu_emulate(), from the same inputs as the real CPU, and the whole machine state is compared:
 * all 16 ymm, 16 GPRs, flags, MXCSR status, the memory arena, and whether (and where) it faulted.
 *
 *   semtest self   [smoke|full]            emulator only: decodes, runs, is deterministic
 *   semtest check  REF [smoke|full]        emulator only, digests against REF (recorded on silicon)
 *   semtest record REF                     needs the instruction set: real CPU vs emulator, writes REF
 *   semtest fault                          the fault handlers catch a write to a guard page
 *   semtest live   [smoke|full]            real CPU vs emulator, no file (silicon, or QEMU TCG as a stand-in)
 * common options: MANIFEST KNOWN as the first two arguments.
 *
 * A "runner" executes one case; the emulator runner here is the only one the suite needs, so a
 * different execution mechanism only has to provide the same function.
 */
#define _GNU_SOURCE
#include "spec.h"
#include "known.h"
#include "regfile.h"
#include "refdigest.h"
#include "platform/memory.h"
#include <fnmatch.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <xmmintrin.h>

typedef struct { uint8_t ymm[16][32]; uint64_t gpr[16]; uint64_t rflags; uint32_t mxcsr, pad; } sem_state;
extern void sem_hw_call(sem_state *st, void *code);
extern void sem_hw_after(void);

enum { K_OK, K_SEGV, K_FPE, K_NODECODE, K_REFUSED, K_BADLEN };
#define FLAGS_ALL 0x8D5u
static uint64_t FLAGS_MASK = FLAGS_ALL;      /* per form: only the flags the SDM defines (AF/PF etc. are undefined for BMI) */
#define PAGE 4096
#define ARENA 0x30000000ul

static uint8_t *g_gl, *g_data, *g_gh, *g_code, *g_stk;     /* guard, 2 data pages, guard, code, scratch stack */

typedef struct {
    int kind; uint64_t fault_page;
    sem_state st; uint64_t mem_hash;
} sem_out;

typedef struct {
    const specform *f; uint8_t code[32]; int len; sem_state in;
    uint8_t mem[2 * PAGE]; int idx, place; uint64_t ea; int vvvv, reg, rm;
} sem_case;

typedef void (*sem_runner)(const sem_case *c, sem_out *o);

static sigjmp_buf g_jb; static volatile int g_sig; static volatile uintptr_t g_addr;
static void on_fault(int sig, siginfo_t *si, void *uc) { (void)uc; g_sig = sig; g_addr = (uintptr_t)si->si_addr; siglongjmp(g_jb, 1); }

static void install_handlers(void) {
    static uint8_t stack[65536];
    stack_t ss = {.ss_sp = stack, .ss_size = sizeof stack}; sigaltstack(&ss, 0);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGFPE, &sa, 0);
}

static int map_arena(void) {
    size_t total = 8 * PAGE;
    uint8_t *p = mmap((void *)ARENA, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) p = mmap(0, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1;
    g_gl = p; g_data = p + PAGE; g_gh = p + 3 * PAGE; g_code = p + 4 * PAGE; g_stk = p + 5 * PAGE;
    mprotect(g_data, 2 * PAGE, PROT_READ | PROT_WRITE);
    mprotect(g_code, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC);
    mprotect(g_stk, 2 * PAGE, PROT_READ | PROT_WRITE);
    return 0;
}

static uint64_t fnv(const void *p, size_t n, uint64_t h) { return ref_fnv1a(h, p, n); }

static void collect(sem_out *o) {
    o->mem_hash = fnv(g_data, 2 * PAGE, REF_FNV_OFFSET);
}

/* debug (SEM_DUMP=file): one line per case of what the CPU did, to name the inputs behind a digest
 * that differs between two machines. Rflags are raw, so undefined flags show here but not in digests. */
static void dump_hex(FILE *d, const uint8_t *p, int n) { for (int b = n - 1; b >= 0; b--) fprintf(d, "%02x", p[b]); }
static void dump_case(FILE *d, const specform *f, const sem_case *c, const sem_out *o) {
    fprintf(d, "%s\t%d\t", f->name, c->idx);
    for (int b = 0; b < c->len; b++) fprintf(d, "%02x", c->code[b]);
    fprintf(d, "\t%x\t%llx\tdst=%d,vvvv=%d,rm=%d", c->in.mxcsr, (unsigned long long)c->in.rflags, c->reg, c->vvvv, c->rm);
    int rg[3] = {c->reg, c->vvvv, c->rm};
    for (int q = 0; q < 3; q++) { fprintf(d, ",y%d=", rg[q] & 15); dump_hex(d, c->in.ymm[rg[q] & 15], 32); }
    long off = f->mem ? (long)(c->ea - (uintptr_t)g_data) : -1;
    int mn = off < 0 || off >= 2 * PAGE ? 0 : off + 32 <= 2 * PAGE ? 32 : 2 * PAGE - off;
    if (mn) { fprintf(d, ",mem="); dump_hex(d, c->mem + off, mn); }
    fprintf(d, "\t%d\t%llx\t%llx\t%llx\t%x\t%llx\t", o->kind, (unsigned long long)o->fault_page, (unsigned long long)(o->st.rflags & FLAGS_MASK),
            (unsigned long long)o->st.rflags, o->st.mxcsr, (unsigned long long)o->mem_hash);
    if (mn && memcmp(g_data + off, c->mem + off, mn)) { fprintf(d, "mem="); dump_hex(d, g_data + off, mn); fputc(' ', d); }
    if (o->kind == K_OK) {
        for (int r = 0; r < 16; r++) if (memcmp(o->st.ymm[r], c->in.ymm[r], 32)) { fprintf(d, "y%d=", r); dump_hex(d, o->st.ymm[r], 32); fputc(' ', d); }
        for (int r = 0; r < 16; r++) if (o->st.gpr[r] != c->in.gpr[r]) fprintf(d, "g%d=%llx ", r, (unsigned long long)o->st.gpr[r]);
    }
    fputc('\n', d);
}

/* ---------------------------------------------------------------- runners */
static void run_hw(const sem_case *c, sem_out *o) {
    memset(o, 0, sizeof *o);
    memcpy(g_data, c->mem, sizeof c->mem);
    memcpy(g_code, c->code, c->len);
    g_code[c->len] = 0xFF; g_code[c->len + 1] = 0x25; memset(g_code + c->len + 2, 0, 4);
    uintptr_t after = (uintptr_t)sem_hw_after; memcpy(g_code + c->len + 6, &after, 8);
    o->st = c->in;
    g_sig = 0;
    if (!sigsetjmp(g_jb, 1)) sem_hw_call(&o->st, g_code);
    else { o->kind = g_sig == SIGFPE ? K_FPE : K_SEGV; o->fault_page = g_sig == SIGFPE ? 0 : (uint64_t)(g_addr & ~(uintptr_t)(PAGE - 1)); memset(&o->st, 0, sizeof o->st); }
    _mm_setcsr(0x1F80);
    collect(o);
}

static void run_emu(const sem_case *c, sem_out *o) {
    memset(o, 0, sizeof *o);
    memcpy(g_data, c->mem, sizeof c->mem);
    memcpy(g_code, c->code, c->len);
    g_code[c->len] = 0x90;
    decoded d; int n = decode(g_code, &d);
    if (n == 0) { o->kind = K_NODECODE; collect(o); return; }
    if (n != c->len) { o->kind = K_BADLEN; collect(o); return; }
    avxemu_regfile rf; memset(&rf, 0, sizeof rf);
    memcpy(rf.ymm, c->in.ymm, sizeof rf.ymm); memcpy(rf.gpr, c->in.gpr, sizeof rf.gpr);
    rf.rflags = c->in.rflags; rf.rip = (uintptr_t)g_code;
    int ok = 0; g_sig = 0;
    if (!sigsetjmp(g_jb, 1)) {
        _mm_setcsr(c->in.mxcsr);
        __asm__ volatile("" ::: "memory");
        ok = avxemu_emulate(&d, &rf);
        __asm__ volatile("" ::: "memory");
        o->st.mxcsr = _mm_getcsr();
        _mm_setcsr(0x1F80);
        if (!ok) { o->kind = K_REFUSED; collect(o); return; }
        memcpy(o->st.ymm, rf.ymm, sizeof rf.ymm); memcpy(o->st.gpr, rf.gpr, sizeof rf.gpr); o->st.rflags = rf.rflags;
    } else {
        _mm_setcsr(0x1F80);
        o->kind = g_sig == SIGFPE ? K_FPE : K_SEGV; o->fault_page = g_sig == SIGFPE ? 0 : (uint64_t)(g_addr & ~(uintptr_t)(PAGE - 1));
    }
    collect(o);
}

/* What is compared and digested: nothing the architecture leaves undefined or the harness owns. */
static size_t pack(const sem_out *o, uint8_t *buf) {
    size_t n = 0;
    buf[n++] = (uint8_t)o->kind;
    memcpy(buf + n, &o->fault_page, 8); n += 8;
    memcpy(buf + n, &o->mem_hash, 8); n += 8;
    if (o->kind == K_OK) {
        memcpy(buf + n, o->st.ymm, 512); n += 512;
        memcpy(buf + n, o->st.gpr, 128); n += 128;
        uint64_t fl = o->st.rflags & FLAGS_MASK; memcpy(buf + n, &fl, 8); n += 8;
        uint32_t mx = o->st.mxcsr & 0x3F; memcpy(buf + n, &mx, 4); n += 4;
    }
    return n;
}

static void describe_diff(const sem_out *h, const sem_out *e, char *out, size_t cap) {
    out[0] = 0;
    if (h->kind != e->kind) { snprintf(out, cap, "outcome hw=%d emu=%d (0 ok,1 segv,2 fpe,3 nodecode,4 refused,5 badlen)", h->kind, e->kind); return; }
    if (h->kind != K_OK) { snprintf(out, cap, "fault page hw=%llx emu=%llx", (unsigned long long)h->fault_page, (unsigned long long)e->fault_page); if (h->mem_hash != e->mem_hash) strncat(out, " memory differs", cap - strlen(out) - 1); return; }
    for (int r = 0; r < 16; r++) for (int b = 0; b < 32; b++)
        if (h->st.ymm[r][b] != e->st.ymm[r][b]) { snprintf(out, cap, "ymm%d byte %d hw=%02x emu=%02x", r, b, h->st.ymm[r][b], e->st.ymm[r][b]); return; }
    for (int r = 0; r < 16; r++)
        if (h->st.gpr[r] != e->st.gpr[r]) { snprintf(out, cap, "gpr%d hw=%llx emu=%llx", r, (unsigned long long)h->st.gpr[r], (unsigned long long)e->st.gpr[r]); return; }
    if ((h->st.rflags ^ e->st.rflags) & FLAGS_MASK) { snprintf(out, cap, "rflags hw=%llx emu=%llx", (unsigned long long)(h->st.rflags & FLAGS_MASK), (unsigned long long)(e->st.rflags & FLAGS_MASK)); return; }
    if ((h->st.mxcsr ^ e->st.mxcsr) & 0x3F) { snprintf(out, cap, "mxcsr flags hw=%x emu=%x", h->st.mxcsr & 0x3F, e->st.mxcsr & 0x3F); return; }
    if (h->mem_hash != e->mem_hash) snprintf(out, cap, "memory differs");
}

/* ---------------------------------------------------------------- inputs */
static uint64_t g_rng;
static uint64_t rnd(void) { return spec_rng(&g_rng); }

static uint64_t edge_elem(int ew, uint64_t r) {
    int bits = ew * 8; uint64_t mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
    uint64_t sign = 1ull << (bits - 1);
    switch (r % 12) {
    case 0: return 0; case 1: return mask; case 2: return sign; case 3: return sign - 1; case 4: return 1;
    case 5: return 1ull << ((r >> 8) % bits); case 6: return ~(1ull << ((r >> 8) % bits)) & mask;
    case 7: return 0x5555555555555555ull & mask; case 8: return 0xAAAAAAAAAAAAAAAAull & mask;
    default: return rnd() & mask;
    }
}
static uint64_t fp_elem(int ew, uint64_t r) {   /* ew: 2 half, 4 single, 8 double */
    int mb = ew == 2 ? 10 : ew == 4 ? 23 : 52, eb = ew == 2 ? 5 : ew == 4 ? 8 : 11;
    uint64_t em = (1ull << eb) - 1, mm = (1ull << mb) - 1, sg = 1ull << (mb + eb), x = 0;
    uint64_t s = (r >> 40) & 1 ? sg : 0, bias = em >> 1;
    switch (r % 16) {
    case 0: x = 0; break; case 1: x = bias << mb; break;                    /* +-0, +-1 */
    case 2: x = em << mb; break; case 3: x = (em << mb) | (1ull << (mb - 1)); break;   /* inf, qNaN */
    case 4: x = (em << mb) | 1; break; case 5: x = (em << mb) | (1ull << (mb - 1)) | (rnd() & mm); break;  /* sNaN, NaN payload */
    case 6: x = 1; break; case 7: x = mm; break;                            /* smallest, largest subnormal */
    case 8: x = 1ull << mb; break; case 9: x = ((em - 1) << mb) | mm; break;   /* min normal, max finite */
    case 10: x = (bias - 1) << mb; break; case 11: x = ((bias + 1 + ((r >> 8) % 5)) << mb) | (rnd() & mm); break;
    case 12: x = (((rnd() % (em - 1)) + 1) << mb) | (rnd() & mm); break;      /* random normal */
    default: x = rnd() & (sg - 1); break;
    }
    return x | s;
}

static int fp_width(const specform *f) {
    const char *c = f->iclass;
    if (!strcmp(c, "VCVTPH2PS") || !strcmp(c, "VCVTPS2PH")) return 2;
    if (strncmp(c, "VF", 2) == 0 && (strstr(c, "MADD") || strstr(c, "MSUB"))) {
        size_t n = strlen(c); return c[n - 1] == 'D' && c[n - 2] == 'S' ? 8 : c[n - 1] == 'D' ? 8 : 4;
    }
    return 0;
}

static void fill_bytes(uint8_t *p, size_t n, int fpw, int ew) {
    uint64_t kind = rnd();
    for (size_t i = 0; i < n; i += ew) {
        uint64_t r = rnd(), v;
        if (fpw) v = fp_elem(fpw, r); else if ((kind & 7) == 0) v = rnd(); else v = edge_elem(ew, r);
        if ((kind & 7) == 1 && i >= (size_t)ew) { memcpy(&v, p + i - ew, ew); }     /* repeat the previous element */
        memcpy(p + i, &v, ew);
    }
}

static const int regv[][3] = {{1, 2, 3}, {9, 10, 11}, {15, 0, 8}, {3, 3, 3}, {0, 0, 1}, {7, 12, 5}, {2, 9, 2}, {4, 5, 6}};

static int is_shiftcount(const specform *f) {   /* vpsll/srl/sra with an xmm or m128 count, and variable shifts */
    const char *c = f->iclass;
    return !strncmp(c, "VPS", 3) && (strstr(c, "LL") || strstr(c, "RL") || strstr(c, "RA")) ;
}

/* Build case #idx of form f. The same (form, idx) always yields the same case. */
static int gen_case(const specform *f, int idx, sem_case *c) {
    uint64_t h = REF_FNV_OFFSET; h = fnv(f->name, strlen(f->name), h);
    g_rng = h ^ ((uint64_t)(idx + 1) * 0x9E3779B97F4A7C15ull); rnd(); rnd();
    memset(c, 0, sizeof *c); c->f = f; c->idx = idx;
    int fpw = fp_width(f);
    int rv = (int)(rnd() % (sizeof regv / sizeof regv[0]));
    specenc e = {.reg = regv[rv][0], .vvvv = regv[rv][1], .rm = regv[rv][2], .base = -1, .index = -1, .scale = 1,
                 .W = (int)(rnd() & 1), .L = (int)(rnd() & 1)};
    /* imm forms run every imm8, three data sets of 256 each */
    int dataseed = f->imm8 ? idx >> 8 : idx;
    e.imm = f->imm8 ? (idx & 255) : 0;
    uint64_t rimm = rnd();
    (void)rimm;
    if (f->imm8) { g_rng = h ^ ((uint64_t)(dataseed + 1) * 0xD1B54A32D192ED03ull); rnd(); rnd(); }
    sem_state *s = &c->in; memset(s, 0, sizeof *s);
    for (int r = 0; r < 16; r++) {
        int ew = 1 << (rnd() & 3);
        fill_bytes(s->ymm[r], 32, fpw, fpw ? fpw : ew);
    }
    if (is_shiftcount(f)) {    /* counts around and past the element width, in the count operand and lanes */
        static const uint64_t cnt[] = {0, 1, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 255, 256, 0x100000001ull, ~0ull, 0x80000000ull};
        for (int r = 0; r < 16; r++) if (rnd() & 1) {
            uint64_t *q = (uint64_t *)s->ymm[r];
            if (strchr(f->iclass, 'V') && f->iclass[3] != 'D') { }
            for (int i = 0; i < 4; i++) if (rnd() & 1) q[i] = cnt[rnd() % 20];
            if (rnd() & 1) for (int i = 0; i < 8; i++) ((uint32_t *)s->ymm[r])[i] = (uint32_t)cnt[rnd() % 20];
        }
    }
    for (int r = 0; r < 16; r++) {
        static const uint64_t gv[] = {0, 1, ~0ull, 0x80000000ull, 0x7fffffffull, 0x8000000000000000ull, 63, 64, 31, 32, 0xffffffffull, 0xffff, 0x8000, 255, 256, 0x0101010101010101ull};
        uint64_t r1 = rnd(); s->gpr[r] = r1 & 3 ? (r1 & 4 ? rnd() : gv[(r1 >> 8) % 16]) : (rnd() % 130);
    }
    s->rflags = (rnd() & FLAGS_ALL) | 0x202;
    s->mxcsr = 0x1F80;
    if (fpw && !getenv("SEM_FPDEFAULT")) {     /* SEM_FPDEFAULT=1: default MXCSR only (debug knob) */
        uint64_t m = rnd() % 16;
        s->mxcsr = 0x1F80 | ((uint32_t)(rnd() & 3) << 13);                 /* rounding mode */
        if (m & 1) s->mxcsr |= 0x8000;                                  /* FTZ */
        if (m & 2) s->mxcsr |= 0x0040;                                  /* DAZ */
        if (m & 4) s->mxcsr |= (uint32_t)(rnd() & 0x3F);                /* sticky flags already set */
        if ((m & 15) == 8) s->mxcsr &= ~(uint32_t)(1 << (7 + rnd() % 6));   /* one exception unmasked */
    }
    /* memory operand */
    fill_bytes(c->mem, sizeof c->mem, fpw, fpw ? fpw : 1 << (rnd() & 3));
    uint64_t ea = 0; c->ea = 0; int mw = f->memw > 0 ? f->memw : 1;
    if (f->mem) {
        static const int places[] = {0, 1, 2, 5, 5, 0, 1, 2, 3, 4};
        c->place = places[idx % 10];   /* every placement in the first ten cases, so smoke has them all */
        uint8_t *de = g_data + 2 * PAGE;
        switch (c->place) {
        case 0: ea = (uintptr_t)(de - mw); break; case 1: ea = (uintptr_t)g_data; break;
        case 2: ea = (uintptr_t)(g_data + PAGE - mw / 2 - (mw > 1)); break; case 3: ea = (uintptr_t)(de - mw + 1); break;
        case 4: ea = (uintptr_t)(g_data - 1); break; default: ea = (uintptr_t)(g_data + 100 + (rnd() & 255)); break;
        }
        static const struct { int base, index, scale, rip, dsz; } mv[] = {
            {0, -1, 1, 0, 0}, {3, -1, 1, 0, 0}, {5, -1, 1, 0, 0}, {4, -1, 1, 0, 0}, {12, -1, 1, 0, 0}, {13, -1, 1, 0, 0},
            {9, 10, 4, 0, 0}, {-1, 1, 8, 0, 0}, {-1, -1, 1, 0, 0}, {7, 12, 2, 0, 0}, {-1, -1, 1, 1, 0}, {1, 2, 1, 0, 8}, {3, 15, 8, 0, 32}};
        int m = (int)(rnd() % 13);
        e.base = mv[m].base; e.index = mv[m].index; e.scale = mv[m].scale; e.rip = mv[m].rip; e.disp_size = mv[m].dsz;
        int32_t disp = (int32_t)((rnd() & 1) ? 0 : (int32_t)(rnd() % 0x2000) - 0x1000);
        if (e.base < 0 && !e.rip) {
            /* [index*scale+disp32] / [disp32] must be reachable with a signed 32-bit displacement */
            if (e.index >= 0) { s->gpr[e.index] = 16; e.disp = (int32_t)(ea - 16 * (uint64_t)e.scale); }
            else e.disp = (int32_t)ea;
        } else if (e.rip) {
            e.disp = 0;
        } else {
            e.disp = disp;
            uint64_t ix = 0;
            if (e.index >= 0) { ix = (rnd() % 64); s->gpr[e.index] = ix; }
            s->gpr[e.base] = ea - (uint64_t)(int64_t)e.disp - ix * (uint64_t)e.scale;
            if (e.index == e.base) { s->gpr[e.base] = ea - (uint64_t)(int64_t)e.disp; }   /* same register twice */
        }
    }
    e.imm = f->imm8 ? e.imm : 0;
    c->ea = ea; c->vvvv = e.vvvv; c->reg = e.reg; c->rm = e.rm;
    int n = spec_encode(f, &e, c->code);
    if (f->mem && e.rip) { e.disp = (int32_t)(ea - ((uintptr_t)g_code + n)); n = spec_encode(f, &e, c->code); }
    c->len = n;
    if (f->mem && e.index >= 0 && e.base >= 0 && e.index == e.base) { /* ea = base*(1+scale) + disp */ }
    /* masked moves: lanes that would touch the guard are masked off in most cases (fault suppression) */
    if (!strncmp(f->iclass, "VPMASKMOV", 9) && f->mem) {
        int lsz = (f->iclass[9] == 'D') ? 4 : 8;
        uint8_t *mask = c->in.ymm[e.vvvv & 15];
        int lanes = (f->L == -1 ? e.L : f->L) ? 32 / lsz : 16 / lsz;
        if (rnd() % 5) for (int j = 0; j < lanes; j++) {
            uintptr_t a = ea + (uintptr_t)j * lsz;
            int inside = a >= (uintptr_t)g_data && a + lsz <= (uintptr_t)(g_data + 2 * PAGE);
            if (!inside) memset(mask + j * lsz, 0x00, lsz);
            else if (rnd() & 1) mask[j * lsz + lsz - 1] |= 0x80; else mask[j * lsz + lsz - 1] &= 0x7F;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- driver */
static int have_hw(void) {
    unsigned a, b, cc, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(cc), "=d"(d) : "a"(7), "c"(0));
    int avx2 = (b >> 5) & 1, bmi1 = (b >> 3) & 1, bmi2 = (b >> 8) & 1;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(cc), "=d"(d) : "a"(1), "c"(0));
    int fma = (cc >> 12) & 1, movbe = (cc >> 22) & 1, f16c = (cc >> 29) & 1;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(cc), "=d"(d) : "a"(0x80000001), "c"(0));
    return avx2 && bmi1 && bmi2 && fma && movbe && f16c && ((cc >> 5) & 1);
}

/* A reference must come from silicon: QEMU TCG passes have_hw() but mishandles NaNs and FMA flags.
 * A KVM guest of QEMU can carry the same name yet runs these instructions on the host CPU. */
static int emulated_cpu(void) {
    char name[49] = "";
    for (unsigned i = 0; i < 3; i++) {
        unsigned r[4];
        __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0x80000002 + i), "c"(0));
        memcpy(name + 16 * i, r, 16);
    }
    unsigned r[4], h[4];
    __asm__ volatile("cpuid" : "=a"(h[0]), "=b"(h[1]), "=c"(h[2]), "=d"(h[3]) : "a"(1), "c"(0));
    char sig[13] = "";
    if ((h[2] >> 31) & 1) { /* hypervisor bit: leaf 0x40000000 is only meaningful then */
        __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0x40000000), "c"(0));
        memcpy(sig, &r[1], 12);
    }
    /* TCG announces itself; the QEMU name only counts as KVM when the hypervisor says so. A TCG guest that
     * hides both (custom model name, KVM signature) cannot be told apart from here. */
    return !strcmp(sig, "TCGTCGTCGTCG") || (strstr(name, "QEMU") && strcmp(sig, "KVMKVMKVM"));
}

static uint64_t flags_mask(const specform *f) {
    static const struct { const char *ic; uint64_t m; } t[] = {
        {"ANDN", 0x8C1}, {"BLSI", 0x8C1}, {"BLSMSK", 0x8C1}, {"BLSR", 0x8C1}, {"BZHI", 0x8C1},
        {"BEXTR", 0x841}, {"LZCNT", 0x41}, {"TZCNT", 0x41}};
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(f->iclass, t[i].ic)) return t[i].m;
    return FLAGS_ALL;
}

static int is_smoke(int idx) {
    uint64_t x = (uint64_t)(idx + 1) * 0x9E3779B97F4A7C15ull;
    return idx < 10 || ((x >> 40) & 7) == 0;
}
static int full_cases(const specform *f) { return f->imm8 ? 768 : 400; }

int main(int argc, char **argv) {
    char *shifted[16];
    if (argc >= 2 && argc < 14 && !strstr(argv[1], ".tsv")) {      /* mode first: manifest and known-failures from the build */
        shifted[0] = argv[0]; shifted[1] = (char *)SEM_MANIFEST; shifted[2] = (char *)SEM_KNOWN;
        for (int i = 1; i < argc; i++) shifted[i + 2] = argv[i];
        argc += 2; argv = shifted;
    }
    if (argc < 4) { fprintf(stderr, "usage: semtest MANIFEST KNOWN self|check|record|live ...\n"); return 2; }
    const char *mode = argv[3], *refpath = NULL; int full = 0;
    int ai = 4;
    if (!strcmp(mode, "check") || !strcmp(mode, "record")) { if (argc < 5) return 2; refpath = argv[ai++]; }
    if (ai < argc && !strcmp(argv[ai], "full")) full = 1;
    if (!strcmp(mode, "record")) full = 1;
    int live = !strcmp(mode, "live") || !strcmp(mode, "record"), record = !strcmp(mode, "record"), check = !strcmp(mode, "check");
    specform *forms; int nf = spec_load(argv[1], &forms);
    if (nf < 0 || known_load(argv[2]) < 0) return 1;
    if (live && !have_hw()) { printf("SKIP: needs AVX2, FMA, BMI1/2, LZCNT, F16C and MOVBE (%s)\n", mode); return 77; }
    if (record && emulated_cpu()) { printf("refusing to record: this CPU is QEMU, not silicon\n"); return 1; }
    if (map_arena() < 0) { perror("arena"); return 1; }
    install_handlers();
    if (!strcmp(mode, "fault")) {     /* the handlers must catch a real fault, also in a static link */
        if (sigsetjmp(g_jb, 1) == 0) { *(volatile uint8_t *)g_gl = 1; puts("fault: no fault raised"); return 1; }
        printf("fault: caught signal %d at %p\n", g_sig, (void *)g_addr);
        return g_sig == SIGSEGV && g_addr == (uintptr_t)g_gl ? 0 : 1;
    }
    avxemu_page_size();
    if (refpath && ref_begin(record ? REF_RECORD : REF_CHECK, "semantics", refpath) != 0) {
        if (check) { printf("SKIP: no reference at %s (record one on AVX2 hardware: .github/workflows/record-references.yml)\n", refpath); return 77; }
        return 1;
    }
    sem_runner emu = run_emu;
    FILE *dump = live && getenv("SEM_DUMP") ? fopen(getenv("SEM_DUMP"), "w") : NULL;
    if (live && getenv("SEM_DUMP") && !dump) { perror("SEM_DUMP"); return 1; }
    long ncases = 0, nforms = 0; int deterministic_bad = 0;
    for (int i = 0; i < nf; i++) {
        const specform *f = &forms[i];
        if (!strcmp(f->status, "refused")) continue;
        if (getenv("SEM_ONLY") && fnmatch(getenv("SEM_ONLY"), f->name, 0)) continue;   /* debug: run only matching forms */
        nforms++;
        int total = full_cases(f);
        FLAGS_MASK = flags_mask(f);
        uint64_t ds = REF_FNV_OFFSET, df = REF_FNV_OFFSET, dh_s = REF_FNV_OFFSET, dh_f = REF_FNV_OFFSET; long cs = 0, cf = 0;
        int bad_hw = 0, bad_dec = 0, bad_ref = 0; int diffs = 0; char repro[600] = "";
        for (int k = 0; k < total; k++) {
            int smoke = is_smoke(k);
            if (!full && !smoke) continue;
            sem_case c; gen_case(f, k, &c);
            sem_out eo, ho; uint8_t pe[1024], ph[1024];
            emu(&c, &eo);
            if (getenv("SEM_TRACE")) printf("%s case %d place %d ea+%lld kind %d\n", f->name, k, c.place, (long long)(c.ea - (uintptr_t)g_data), eo.kind);
            size_t ne = pack(&eo, pe);
            if (eo.kind == K_NODECODE || eo.kind == K_BADLEN || eo.kind == K_REFUSED) {
                if (!bad_dec) { char hx[80] = ""; for (int b = 0; b < c.len; b++) sprintf(hx + 2 * b, "%02x", c.code[b]);
                    printf("  repro semantics/%s/decode: %s (case %d) kind %d\n", f->name, hx, k, eo.kind); }
                bad_dec = 1;
            }
            ds = fnv(pe, ne, ds); if (smoke) { cs++; } cf++;
            if (smoke) df = fnv(pe, ne, df);      /* df = smoke digest, ds = full digest */
            if (live) {
                run_hw(&c, &ho);
                size_t nh = pack(&ho, ph);
                if (dump) dump_case(dump, f, &c, &ho);
                dh_f = fnv(ph, nh, dh_f); if (smoke) dh_s = fnv(ph, nh, dh_s);
                if ((nh != ne || memcmp(ph, pe, ne)) && getenv("SEM_DIFFMAX") && diffs++ < atoi(getenv("SEM_DIFFMAX"))) {
                    /* debug: every differing case with its inputs and both outcomes, beyond the first repro */
                    printf("  diff %s case %d mxcsr_in=%04x kind hw=%d emu=%d mxcsr hw=%x emu=%x\n", f->name, k, c.in.mxcsr, ho.kind, eo.kind, ho.st.mxcsr & 0x3F, eo.st.mxcsr & 0x3F);
                    int rg[3] = {c.reg, c.vvvv, c.rm}; static const char *rn[3] = {"dst", "vvvv", "rm"};
                    for (int q = 0; q < 3; q++) { printf("    %s(in)=", rn[q]); for (int b = 31; b >= 0; b--) printf("%02x", c.in.ymm[rg[q] & 15][b]); putchar('\n'); }
                    if (f->mem) { long off = (long)(c.ea - (uintptr_t)g_data); printf("    mem@%ld=", off); for (int b = 31; b >= 0; b--) printf("%02x", off >= 0 && off + b < 2 * PAGE ? c.mem[off + b] : 0); putchar('\n'); }
                    printf("    hw  dst="); for (int b = 31; b >= 0; b--) printf("%02x", ho.st.ymm[c.reg & 15][b]); putchar('\n');
                    printf("    emu dst="); for (int b = 31; b >= 0; b--) printf("%02x", eo.st.ymm[c.reg & 15][b]); putchar('\n');
                }
                if (nh != ne || memcmp(ph, pe, ne)) {
                    if (!bad_hw) {
                        char why[200], hx[80] = ""; describe_diff(&ho, &eo, why, sizeof why);
                        for (int b = 0; b < c.len; b++) sprintf(hx + 2 * b, "%02x", c.code[b]);
                        int o = 0; o = snprintf(repro, sizeof repro, "  repro semantics/%s/hw: %s (case %d, place %d, ea+%lld, mxcsr %04x): %s", f->name, hx, k, c.place, (long long)(c.ea - (uintptr_t)g_data), c.in.mxcsr, why);
                        if (fp_width(f)) { int rg[3] = {c.reg, c.vvvv, c.rm}; for (int q = 0; q < (f->mem ? 2 : 3); q++) { o += snprintf(repro + o, sizeof repro - o, " r%d=", rg[q]); for (int b = 15; b >= 0; b--) o += snprintf(repro + o, sizeof repro - o, "%02x", c.in.ymm[rg[q] & 15][b]); } }
                        if (!strncmp(f->iclass, "VPMASKMOV", 9)) { o += snprintf(repro + o, sizeof repro - o, " mask="); for (int b = 0; b < 32 && o < 560; b++) o += snprintf(repro + o, sizeof repro - o, "%02x", c.in.ymm[c.vvvv & 15][b]); }
                    }
                    bad_hw = 1;
                }
            }
            ncases++;
        }
        char nm[48];
        if (record) {
            snprintf(nm, sizeof nm, "%s:s", f->name); ref_put(nm, (unsigned long)cs, dh_s);
            snprintf(nm, sizeof nm, "%s:f", f->name); ref_put(nm, (unsigned long)cf, dh_f);
        } else if (check) {
            /* smoke digests (:s) are always checkable; the full one (:f) only after a full run */
            for (int pass = 0; pass < 2; pass++) {
                int isfull = pass == 1;
                snprintf(nm, sizeof nm, "%s:%c", f->name, isfull ? 'f' : 's');
                unsigned long rc; uint64_t rd;
                if (!ref_get(nm, &rc, &rd)) { printf("  repro semantics/%s/digest: %s not in the reference\n", f->name, nm); bad_ref = 1; continue; }
                if (isfull && !full) continue;
                uint64_t mine = isfull ? ds : df; unsigned long mc = isfull ? (unsigned long)cf : (unsigned long)cs;
                if (mc != rc || mine != rd) {
                    printf("  repro semantics/%s/digest: %s emulator %016llx (%lu cases) != recorded %016llx (%lu)\n", f->name, nm, (unsigned long long)mine, mc, (unsigned long long)rd, rc);
                    bad_ref = 1;
                }
            }
        }
        if (!strcmp(mode, "self") || check) {     /* determinism: a second pass must give the same digest */
            uint64_t d2 = REF_FNV_OFFSET;
            for (int k = 0; k < total; k++) {
                if (!is_smoke(k)) continue;
                sem_case c; gen_case(f, k, &c); sem_out o2; uint8_t p2[1024]; emu(&c, &o2); d2 = fnv(p2, pack(&o2, p2), d2);
            }
            if (d2 != df) deterministic_bad = 1;
        }
        char cn[160];
        snprintf(cn, sizeof cn, "semantics/%s/decode", f->name); known_case(cn, !bad_dec);
        if (live) { snprintf(cn, sizeof cn, "semantics/%s/hw", f->name); if (known_case(cn, !bad_hw) || (bad_hw && getenv("KNOWN_VERBOSE"))) puts(repro); }
        if (check) { snprintf(cn, sizeof cn, "semantics/%s/digest", f->name); known_case(cn, !bad_ref); }
    }
    printf("semantics (%s%s): %ld forms, %ld cases\n", mode, full ? ", full" : "", nforms, ncases);
    int rc = known_finish("semantics");
    if (deterministic_bad) { printf("emulator results are not deterministic across two passes\n"); rc++; }
    if (refpath) rc += ref_finish();
    if (dump) fclose(dump);
    return rc != 0;
}
