#include <stdint.h>
#include <string.h>
#include "decode.h"
#include "regs.h"
#include "regfile.h"
#include "platform/memory.h"

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
static void mem_write(uint64_t ea, const void *src, int n, int seg){
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
    if (d->a_src == OPND_MEM) { uint64_t a=ea_rf(d,rf,rip_next); MEMRD(A.b, a, d->mem_bytes); }
    else if (d->a_src >= 0)   memcpy(A.b, rf->ymm[d->a_src], 32);
    if (d->b_src == OPND_MEM) { uint64_t a=ea_rf(d,rf,rip_next); MEMRD(B.b, a, d->mem_bytes); }
    else if (d->b_src >= 0)   memcpy(B.b, rf->ymm[d->b_src], 32);
    if (d->c_src >= 0)        memcpy(C.b, rf->ymm[d->c_src], 32);
    if (d->shift_imm) { memset(B.b,0,32); B.b[0] = d->imm; }

    if (!vec_exec(d->op, d->type, &A, &B, &C, d->imm, &OUT, &gpr_out)) return 0;

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
