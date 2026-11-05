#include <stdint.h>
#include <string.h>
#include <xmmintrin.h>
#include "decode.h"
#include "regs.h"
#include "regfile.h"
#include "platform/memory.h"

/* An unmasked exception is #XM on silicon: no result is written. Provoke the real SSE fault of
 * the same kind (MXCSR still has the application's masks) so the application sees a SIGFPE. */
static void fp_fault(uint32_t t) {
    float x, y;
    if (t & 1)        { x = 0.0f;    y = 0.0f;    __asm__ volatile("divss %1, %0" : "+x"(x) : "x"(y)); }   /* IE */
    else if (t & 2)   { x = 1e-40f;  y = 0.0f;    __asm__ volatile("addss %1, %0" : "+x"(x) : "x"(y)); }   /* DE */
    else if (t & 8)   { x = 3e38f;   y = 3e38f;   __asm__ volatile("mulss %1, %0" : "+x"(x) : "x"(y)); }   /* OE */
    else if (t & 16)  { x = 1e-30f;  y = 1e-30f;  __asm__ volatile("mulss %1, %0" : "+x"(x) : "x"(y)); }   /* UE */
    else              { x = 1.0f;    y = 3.0f;    __asm__ volatile("divss %1, %0" : "+x"(x) : "x"(y)); }   /* ZE, PE */
}

/*
 * Emulated memory access. `seg` selects a segment override (1=gs, 2=fs): the
 * effective address from ea_rf() is the *flat* part (base+index+disp), and the
 * CPU adds the segment base, so we issue a real gs/fs-prefixed access rather
 * than trying to read the (kernel-private) segment base ourselves.
 */
static void mem_read(void *dst, uint64_t ea, int n, int seg){
    if (seg == 1)      { for (int i=0;i<n;i++){ uint8_t v; __asm__ volatile("movb %%gs:(%1),%0":"=r"(v):"r"(ea+(uint64_t)i)); ((uint8_t*)dst)[i]=v; } return; }
    else if (seg == 2) { for (int i=0;i<n;i++){ uint8_t v; __asm__ volatile("movb %%fs:(%1),%0":"=r"(v):"r"(ea+(uint64_t)i)); ((uint8_t*)dst)[i]=v; } return; }
    /*
     * Vector loads over-read: optimized code fetches a full 16/32-byte vector
     * past a buffer's logical end, relying on page granularity. That is fine on
     * hardware when the trailing page is mapped; here the byte-copy would fault.
     * Read the first page normally (a genuinely bad EA still faults, like native),
     * and zero-fill any over-read into an unmapped trailing page — those bytes
     * aren't used by a real over-read, so the result matches.
     */
    uint64_t end = ea + (uint64_t)n;
    size_t page = avxemu_page_size();
    uint64_t fp = ea - ea % page, lp = (end - 1) - (end - 1) % page;
    if (fp == lp) { memcpy(dst, (void*)ea, n); return; }
    uint8_t *d8 = (uint8_t*)dst;
    uint64_t b = fp + page; size_t n0 = (size_t)(b - ea);
    memcpy(d8, (void*)ea, n0);                       /* first page: fault if unmapped (genuine bad EA) */
    /* Only a verified gap gets compatibility zeros. Mapped guards and unknown
     * mappings retain native operand semantics. These are real operand reads,
     * not diagnostic safe reads: a denied self-read syscall must not change
     * a readable operand into zeros or conceal a genuine access fault. */
    size_t tail = (size_t)(end - b);
    avxemu_mapping mapping;
    if (avxemu_memory_query((uintptr_t)b, &mapping) == 0)
        memset(d8 + n0, 0, tail);
    else
        memcpy(d8 + n0, (const void *)(uintptr_t)b, tail);
}
/* One gather element. Unlike mem_read this never tolerates an unmapped trailing page: a masked-in
 * element must fault exactly where the instruction would. */
static void mem_read_exact(void *dst, uint64_t ea, int n, int seg){
    if (seg) { mem_read(dst, ea, n, seg); return; }     /* the segment path is already byte-exact */
    memcpy(dst, (const void*)ea, n);
}
/* Hardware commits nothing when any byte of a store faults, so a store that spans two pages
 * must find out about the second one before writing the first. An atomic or-with-zero faults
 * exactly as the store would and changes nothing, whatever other threads do meanwhile. */
static void probe_w(uint64_t ea, int seg){
    if (seg == 1)      __asm__ volatile("lock orb $0,%%gs:(%0)"::"r"(ea):"memory","cc");
    else if (seg == 2) __asm__ volatile("lock orb $0,%%fs:(%0)"::"r"(ea):"memory","cc");
    else               __asm__ volatile("lock orb $0,(%0)"::"r"(ea):"memory","cc");
}
static void probe_store(uint64_t ea, int n, int seg, int always){
    uint64_t last = ea + (uint64_t)n - 1, page = avxemu_page_size();
    if (!always && !seg && ea / page == last / page) return;   /* one page: the store is all-or-nothing */
    probe_w(ea, seg);
    if (seg || ea / page != last / page) probe_w(last, seg);
}
static void mem_write(uint64_t ea, const void *src, int n, int seg){
    probe_store(ea, n, seg, 0);
    if (seg == 1)      for (int i=0;i<n;i++){ uint8_t v=((const uint8_t*)src)[i]; __asm__ volatile("movb %0,%%gs:(%1)"::"r"(v),"r"(ea+(uint64_t)i)); }
    else if (seg == 2) for (int i=0;i<n;i++){ uint8_t v=((const uint8_t*)src)[i]; __asm__ volatile("movb %0,%%fs:(%1)"::"r"(v),"r"(ea+(uint64_t)i)); }
    else               memcpy((void*)ea, src, n);
}
#define MEMRD(dstp,addr,nn) mem_read((dstp),(addr),(int)(nn),d->seg)
#define MEMWR(addr,srcp,nn) mem_write((addr),(srcp),(int)(nn),d->seg)

static uint64_t ea_rf(const decoded *d, const avxemu_regfile *rf, uint64_t rip_next) {
    uint64_t addr = (uint64_t)(int64_t)d->disp;
    if (d->rip_rel) addr += rip_next;
    if (d->base  != OPND_NONE) addr += rf->gpr[d->base];
    if (d->index != OPND_NONE) addr += rf->gpr[d->index] * d->scale;
    return d->addr32 ? (uint32_t)addr : addr;
}

/*
 * The single emulation core, shared by the SIGILL handler and the trampoline.
 * Reads operands from rf (registers or memory), runs the SSE-only executor, and
 * writes results back into rf. rf->rip must be the instruction's own address so
 * RIP-relative memory operands resolve correctly. Returns 1 on success, 0 if the
 * op is recognised-but-unimplemented or a memory operand is implausibly sized.
 */
int avxemu_emulate(const decoded *d, avxemu_regfile *rf) {
    uint64_t rip_next = rf->rip + d->len;

    if (d->is_bmi) {
        uint64_t s1 = 0, s2 = 0, dst = 0, dst2 = 0, flags = rf->rflags, memv = 0;
        if (d->a_src == OPND_MEM || d->b_src == OPND_MEM) {
            if (d->mem_bytes < 1 || d->mem_bytes > (int)sizeof memv) return 0;
            uint64_t a = ea_rf(d, rf, rip_next); MEMRD(&memv, a, d->mem_bytes);
        }
        if (d->a_src == OPND_MEM) s1 = memv;
        else if (d->a_src >= 0)   s1 = rf->gpr[d->a_src];
        if (d->bmi_s1_rdx) s1 = rf->gpr[2];
        if (d->op == BMI_RORX)         s2 = d->imm;
        else if (d->b_src == OPND_MEM) s2 = memv;
        else if (d->b_src >= 0)        s2 = rf->gpr[d->b_src];
        if (!bmi_exec(d->op, d->opsize, s1, s2, &dst, &dst2, &flags)) return 0;
        if (d->dst_kind == DST_MEM) {
            uint64_t a = ea_rf(d, rf, rip_next); MEMWR(a, &dst, d->mem_bytes);
        } else {
            /* 16-bit ops (66-prefixed lzcnt/tzcnt/movbe) write only the low word
             * of the dst register on real hardware; 32/64-bit writes zero-extend. */
            if (d->dst >= 0)      rf->gpr[d->dst] = (d->opsize == 16)
                                      ? ((rf->gpr[d->dst] & ~0xFFFFull) | (dst & 0xFFFFull))
                                      : dst;
            if (d->bmi_dst2 >= 0) rf->gpr[d->bmi_dst2] = dst2;
            rf->rflags = flags;
        }
        return 1;
    }

    /* AVX2 gather (SDM VPGATHERDD etc.). Written into rf element by element, so a fault leaves exactly
     * the architectural partial state: elements before it are in dst and cleared in the mask, and the
     * instruction can be restarted. Masked-off elements are never read. Each mask element is cleared as
     * it is processed (the rest keep their value); on completion the whole mask register and everything
     * in dst above the gathered elements are zero. */
    if (d->op == VGATHER) {
        int elem = d->mem_bytes, isz = d->vsib_bytes, vec = d->wide ? 32 : 16;
        int count = vec / (elem > isz ? elem : isz);
        if ((elem != 4 && elem != 8) || (isz != 4 && isz != 8) || d->vsib_index < 0) return 0;
        uint8_t *dst = rf->ymm[d->dst], *mask = rf->ymm[d->a_src];
        const uint8_t *ix = rf->ymm[d->vsib_index];
        uint64_t base = ea_rf(d, rf, rip_next);
        for (int i = 0; i < count; i++) {
            if ((mask[i * elem + elem - 1] >> 7) & 1) {
                int64_t v;
                if (isz == 8) memcpy(&v, ix + i * 8, 8);
                else { int32_t v32; memcpy(&v32, ix + i * 4, 4); v = v32; }
                uint64_t a = base + (uint64_t)v * d->scale;
                if (d->addr32) a = (uint32_t)a;
                uint8_t tmp[8];
                mem_read_exact(tmp, a, elem, d->seg);
                memcpy(dst + i * elem, tmp, elem);
            }
            memset(mask + i * elem, 0, elem);
        }
        memset(dst + count * elem, 0, 32 - count * elem);
        memset(mask, 0, 32);
        return 1;
    }

    /* AVX2 masked move: per-lane conditional memory access (unmasked lanes are
     * neither read nor written, so they never fault). Mask = a_src register; the
     * lane's high bit selects. Load: dst<-mem (else 0). Store: mem<-b_src. */
    if (d->op == VPMASKMOVD || d->op == VPMASKMOVQ) {
        int lsz = (d->op == VPMASKMOVQ) ? 8 : 4;
        int lanes = (d->wide ? 32 : 16) / lsz;
        const uint8_t *mask = (const uint8_t *)rf->ymm[d->a_src];
        uint64_t ea = ea_rf(d, rf, rip_next);
        if (d->dst_kind == DST_MEM) {
            const uint8_t *src = (const uint8_t *)rf->ymm[d->b_src];
            /* every selected lane is checked before any is written */
            for (int i = 0; i < lanes; i++) {
                int sign = (lsz == 8) ? (((const int64_t *)mask)[i] < 0) : (((const int32_t *)mask)[i] < 0);
                if (sign) probe_store(ea + (uint64_t)i * lsz, lsz, d->seg, 1);
            }
            for (int i = 0; i < lanes; i++) {
                int sign = (lsz == 8) ? (((const int64_t *)mask)[i] < 0) : (((const int32_t *)mask)[i] < 0);
                if (sign) mem_write(ea + (uint64_t)i * lsz, src + i * lsz, lsz, d->seg);
            }
        } else {
            uint8_t out[32]; memset(out, 0, 32);
            for (int i = 0; i < lanes; i++) {
                int sign = (lsz == 8) ? (((const int64_t *)mask)[i] < 0) : (((const int32_t *)mask)[i] < 0);
                if (sign) mem_read(out + i * lsz, ea + (uint64_t)i * lsz, lsz, d->seg);
            }
            memcpy(rf->ymm[d->dst], out, 16);
            if (d->wide) memcpy(rf->ymm[d->dst] + 16, out + 16, 16);
            else         memset(rf->ymm[d->dst] + 16, 0, 16);
        }
        return 1;
    }

    ymm256 A, B, C, OUT; uint64_t gpr_out = 0;
    memset(&A,0,sizeof A); memset(&B,0,sizeof B); memset(&C,0,sizeof C);
    if ((d->a_src == OPND_MEM || d->b_src == OPND_MEM || d->dst_kind == DST_MEM)
        && (d->mem_bytes < 1 || d->mem_bytes > (int)sizeof A.b)) return 0;
    /* A misaligned ymm vmovntdqa is #GP, which Linux reports as a SIGSEGV for address 0. */
    if (d->op == VMOVNTDQA && !d->seg && (ea_rf(d, rf, rip_next) & 31)) __asm__ volatile("movb (%0), %%al" :: "r"((uintptr_t)0) : "al");
    if (d->a_src == OPND_MEM) { uint64_t a=ea_rf(d,rf,rip_next); MEMRD(A.b, a, d->mem_bytes); }
    else if (d->a_src >= 0)   memcpy(A.b, rf->ymm[d->a_src], 32);
    if (d->b_src == OPND_MEM) { uint64_t a=ea_rf(d,rf,rip_next); MEMRD(B.b, a, d->mem_bytes); }
    else if (d->b_src >= 0)   memcpy(B.b, rf->ymm[d->b_src], 32);
    if (d->c_src >= 0)        memcpy(C.b, rf->ymm[d->c_src], 32);
    if (d->shift_imm) { memset(B.b,0,32); B.b[0] = d->imm; }
    /* A VEX.128 FMA must not compute, or raise flags for, the stale upper lanes. */
    if (!d->wide && ((d->op >= VFMADD132 && d->op <= VFNMSUB231) || (d->op >= VFMADDSUB132 && d->op <= VFMSUBADD231))) {
        memset(A.b + 16, 0, 16); memset(B.b + 16, 0, 16); memset(C.b + 16, 0, 16);
    }
    if (!d->wide && d->op == VCVTPH2PS) memset(B.b + 8, 0, 24);   /* xmm form: 4 halves only */

    /* The helpers never fault: they take MXCSR by value and report an unmasked exception as
     * FP_PENDING. Run them with the sticky bits clear so `raised` is exactly this op's flags. */
    uint32_t csr = _mm_getcsr(), mx = csr & ~0x3Fu;
    int ran = vec_exec(d->op, d->type, &A, &B, &C, d->imm, &OUT, &gpr_out, &mx);
    if (!ran) return 0;
    uint32_t raised = mx & 0x3Fu;
    if (raised) _mm_setcsr(csr | raised);
    if (ran == AVXEMU_FP_PENDING) { fp_fault(raised & ~(csr >> 7)); return 0; }

    if (d->dst_kind == DST_GPR) {
        /* VPMOVMSKB r32, xmm (VEX.128) yields a 16-bit mask with the upper bits
         * zeroed; vec_exec always packs 32 bits (lo | hi<<16), where the hi half
         * is the stale upper 128 of the source ymm. Drop it for the 128-bit form
         * (same upper-zeroing the vector-dst path does when !wide). Without this,
         * scan loops (vpcmpeqb; vpmovmskb; bsf/test) see phantom high bits and
         * never terminate — the same failure family as the 16-bit lzcnt bug. */
        if (d->op == VPMOVMSKB && !d->wide) gpr_out &= 0xFFFFu;
        rf->gpr[d->dst] = gpr_out;
    } else if (d->dst_kind == DST_MEM) {
        uint64_t a = ea_rf(d, rf, rip_next); MEMWR(a, OUT.b, d->mem_bytes);
    } else {
        memcpy(rf->ymm[d->dst], OUT.b, 16);
        if (d->wide) memcpy(rf->ymm[d->dst] + 16, OUT.b + 16, 16);
        else         memset(rf->ymm[d->dst] + 16, 0, 16);   /* VEX.128 zeroes upper */
    }
    return 1;
}
