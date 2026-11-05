/*
 * decoder differential against Intel XED (test-only).
 *   positive  every `tested`/`failing` manifest form with varied registers, REX/VEX bits,
 *             addressing modes and 0x67/segment padding: length, registers, memory operand,
 *             memory width and read/write direction must equal XED's.
 *   negative  the whole VEX/legacy opcode space: whatever XED says is not one of our
 *             instructions (crc32, bsf/bsr, SSE4.2, AVX1, invalid) must be rejected by decode().
 *   soup      prefix orderings around lzcnt/tzcnt/movbe/crc32/bsf/bsr/popcnt.
 *   fuzz      random bytes: decode()'s length must equal XED's.
 * usage: decoder_test MANIFEST KNOWN [positive|negative|fuzz|all] [fuzz-iterations]
 */
#include "spec.h"
#include "known.h"
#include "decode.h"
#include "xed-interface.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int in_scope(xed_isa_set_enum_t s) {
    switch (s) {
    case XED_ISA_SET_AVX2: case XED_ISA_SET_AVX2GATHER: case XED_ISA_SET_FMA: case XED_ISA_SET_BMI1:
    case XED_ISA_SET_BMI2: case XED_ISA_SET_LZCNT: case XED_ISA_SET_F16C: case XED_ISA_SET_MOVBE: return 1;
    default: return 0;
    }
}

typedef struct {
    int ok, len, scope, memw, rd, wr, nreg, reg[8], base, index, scale, rip, seg, imm, zcnt;
    int64_t disp; char iclass[40];
} xr;

static int regnum(xed_reg_enum_t r) {
    xed_reg_class_enum_t c = xed_reg_class(r);
    if (c == XED_REG_CLASS_GPR) { xed_reg_enum_t b = xed_get_largest_enclosing_register(r); return (int)(b - XED_REG_RAX); }
    if (c == XED_REG_CLASS_XMM) return (int)(r - XED_REG_XMM0);
    if (c == XED_REG_CLASS_YMM) return (int)(r - XED_REG_YMM0);
    return -1;
}

static void xdecode(const uint8_t *b, int n, xr *o) {
    xed_decoded_inst_t x; xed_state_t st; xed_state_zero(&st);
    st.mmode = XED_MACHINE_MODE_LONG_64; st.stack_addr_width = XED_ADDRESS_WIDTH_64b;
    xed_decoded_inst_zero_set_mode(&x, &st);
    xed_decoded_inst_set_input_chip(&x, XED_CHIP_ALL);
    memset(o, 0, sizeof *o); o->base = o->index = -1;
    if (xed_decode(&x, b, n) != XED_ERROR_NONE) return;
    o->ok = 1; o->len = (int)xed_decoded_inst_get_length(&x);
    xed_isa_set_enum_t s = xed_decoded_inst_get_isa_set(&x);
    o->scope = in_scope(s);
    snprintf(o->iclass, sizeof o->iclass, "%s", xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(&x)));
    o->zcnt = !strcmp(o->iclass, "LZCNT") || !strcmp(o->iclass, "TZCNT");
    const xed_inst_t *in = xed_decoded_inst_inst(&x);
    for (unsigned i = 0; i < xed_inst_noperands(in); i++) {
        const xed_operand_t *op = xed_inst_operand(in, i);
        if (xed_operand_operand_visibility(op) != XED_OPVIS_EXPLICIT) continue;
        xed_operand_enum_t nm = xed_operand_name(op);
        if (nm >= XED_OPERAND_REG0 && nm <= XED_OPERAND_REG9) {
            int r = regnum(xed_decoded_inst_get_reg(&x, nm)), k;
            for (k = 0; k < o->nreg; k++) if (o->reg[k] == r) break;
            if (k == o->nreg && o->nreg < 8) o->reg[o->nreg++] = r;
        }
    }
    for (int i = 0; i < o->nreg; i++) for (int j = i + 1; j < o->nreg; j++)
        if (o->reg[j] < o->reg[i]) { int t = o->reg[i]; o->reg[i] = o->reg[j]; o->reg[j] = t; }
    if (xed_decoded_inst_number_of_memory_operands(&x)) {
        o->memw = (int)xed_decoded_inst_get_memory_operand_length(&x, 0);
        o->rd = xed_decoded_inst_mem_read(&x, 0); o->wr = xed_decoded_inst_mem_written(&x, 0);
        xed_reg_enum_t bs = xed_decoded_inst_get_base_reg(&x, 0), ix = xed_decoded_inst_get_index_reg(&x, 0);
        if (bs == XED_REG_RIP) o->rip = 1; else if (bs != XED_REG_INVALID) o->base = regnum(bs);
        if (ix != XED_REG_INVALID) o->index = regnum(ix);
        o->scale = (int)xed_decoded_inst_get_scale(&x, 0);
        o->disp = xed_decoded_inst_get_memory_displacement(&x, 0);
        xed_reg_enum_t sg = xed_decoded_inst_get_seg_reg(&x, 0);
        o->seg = sg == XED_REG_FS ? 2 : sg == XED_REG_GS ? 1 : 0;
    }
    if (xed_decoded_inst_get_immediate_width(&x)) o->imm = (int)xed_decoded_inst_get_unsigned_immediate(&x) & 0xff;
}

static char hexbuf[8][64];
static const char *hex(const uint8_t *b, int n) {
    static int k; char *s = hexbuf[k++ & 7]; if (n > 20) n = 20;
    for (int i = 0; i < n; i++) sprintf(s + 2 * i, "%02x", b[i]);
    return s;
}

static char cname[256];
static void result(const char *suite, const char *form, const char *what, int ok, const uint8_t *b, int n, const char *why) {
    snprintf(cname, sizeof cname, "%s/%s/%s", suite, form, what);
    int unexpected = known_case(cname, ok);
    if (unexpected || (!ok && getenv("KNOWN_VERBOSE"))) printf("  repro %s: %s %s\n", cname, hex(b, n), why ? why : "");
}

/* ------------------------------------------------------------ positive */
struct memv { int base, index, scale, disp, rip, dsz; };
static const struct memv memvs[] = {
    {0, -1, 1, 0, 0, 0}, {3, -1, 1, 0x7f, 0, 0}, {5, -1, 1, 0, 0, 0}, {4, -1, 1, 0, 0, 0},
    {12, -1, 1, 0x12345, 0, 0}, {13, -1, 1, -4, 0, 0}, {9, 10, 4, -8, 0, 0}, {-1, 1, 8, 0x1000, 0, 0},
    {-1, -1, 1, 0x2000, 0, 0}, {7, 12, 2, 0, 0, 0}, {-1, -1, 1, 0x100, 1, 0}, {1, 2, 1, 0, 0, 8},
    {3, 15, 8, 0x80, 0, 32}, {14, 13, 1, 0, 0, 0},
};
static const int regv[][3] = {{1, 2, 3}, {9, 10, 11}, {15, 0, 8}, {3, 3, 3}, {0, 0, 1}, {7, 12, 5}, {2, 9, 2}};

static int positive(const specform *f) {
    uint8_t b[40], pad[40]; int bad[8] = {0}, seen = 0;
    static const char *const what[] = {"len", "regs", "mem", "memw", "rw", "imm", "seg", "addr32"};
    uint8_t first[8][24]; int firstn[8]; char why[8][64];
    int gather = !strncmp(f->iclass, "VGATHER", 7) || !strncmp(f->iclass, "VPGATHER", 8);
    int nW = f->W < 0 ? 2 : 1, nL = f->L == -1 ? 2 : 1;
    int nmem = f->mem ? (int)(sizeof memvs / sizeof memvs[0]) : 1;
    for (int w = 0; w < nW; w++) for (int l = 0; l < nL; l++) for (int m = 0; m < nmem; m++)
    for (unsigned r = 0; r < sizeof regv / sizeof regv[0]; r++) for (int v2 = 0; v2 < 2; v2++)
    for (int p67 = 0; p67 < 3; p67++) for (int sg = 0; sg < 3; sg += f->mem ? 1 : 3) {
        specenc e = {.reg = regv[r][0], .vvvv = regv[r][1], .rm = regv[r][2], .base = -1, .index = -1, .scale = 1,
                     .W = w, .L = l, .imm = 0x80 | (m * 7 & 0x7f), .pad67 = p67, .vex2 = v2,
                     .segpfx = sg == 1 ? 0x64 : sg == 2 ? 0x65 : 0};
        if (f->mem) {
            const struct memv *mv = &memvs[m];
            e.base = mv->base; e.index = mv->index; e.scale = mv->scale; e.disp = mv->disp; e.rip = mv->rip; e.disp_size = mv->dsz;
            if (r > 2) continue;
            if (gather) {      /* VSIB: a vector index, no RIP form, dst/mask/index all different */
                if (mv->rip || e.reg == e.vvvv) continue;
                e.index = (m & 1) ? 12 : 4; e.scale = 1 << (m & 3);
                while (e.index == e.reg || e.index == e.vvvv) e.index = (e.index + 1) & 15;
            }
        } else if (v2 || p67 > 1) { if (r > 0) continue; }
        int n = spec_encode(f, &e, b);
        memset(pad, 0x90, sizeof pad); memcpy(pad, b, n);
        xr x; xdecode(pad, 15, &x);
        if (!x.ok || x.len != n) { fprintf(stderr, "generator: %s %s not accepted by XED as %d bytes\n", f->name, hex(b, n), n); continue; }
        decoded d; int got = decode(pad, &d);
        seen++;
        int fail[8] = {0};
        fail[0] = got != n;
        if (got) {
            int dr[8], ndr = 0;
            int c[6] = {d.dst_kind == DST_MEM ? -1 : d.dst, d.bmi_dst2, d.a_src, d.b_src, d.c_src, -1};
            for (int i = 0; i < 5; i++) if (c[i] >= 0) { int k; for (k = 0; k < ndr; k++) if (dr[k] == c[i]) break; if (k == ndr) dr[ndr++] = c[i]; }
            for (int i = 0; i < ndr; i++) for (int j = i + 1; j < ndr; j++) if (dr[j] < dr[i]) { int t = dr[i]; dr[i] = dr[j]; dr[j] = t; }
            fail[1] = ndr != x.nreg || memcmp(dr, x.reg, ndr * sizeof(int)) != 0;
            if (f->mem) {
                int hm = d.has_mem && (gather || d.a_src == OPND_MEM || d.b_src == OPND_MEM || d.dst_kind == DST_MEM);
                fail[7] = p67 && f->mem && !d.addr32;      /* 0x67 truncates the address; decode() must say so */
                if (!p67) fail[2] = !hm || d.base != (x.base < 0 ? OPND_NONE : x.base) ||
                          (gather ? (d.index != OPND_NONE || d.vsib_index != x.index) : d.index != (x.index < 0 ? OPND_NONE : x.index)) ||
                          (x.index >= 0 && d.scale != x.scale) || d.disp != (int32_t)x.disp || d.rip_rel != x.rip;
                fail[3] = d.mem_bytes != x.memw;
                fail[4] = (d.dst_kind == DST_MEM) != x.wr || ((gather || d.a_src == OPND_MEM || d.b_src == OPND_MEM) != x.rd);
                fail[6] = d.seg != x.seg;
            }
            if (f->imm8) fail[5] = d.imm != x.imm && !(d.op == VPBLENDVB);
        }
        for (int k = 0; k < 8; k++) if (fail[k] && !bad[k]) {
            bad[k] = 1; memcpy(first[k], b, n); firstn[k] = n;
            snprintf(why[k], sizeof why[k], "(decode len %d, xed len %d)", got, x.len);
        }
    }
    if (!seen) return 0;
    for (int k = 0; k < 8; k++) result("decoder", f->name, what[k], !bad[k], first[k], firstn[k], why[k]);
    return seen;
}

/* ------------------------------------------------------------ negative sweep */
static long negs;
static void neg_one(const uint8_t *b, int vex, int tag, int map, int pp, int opc, int mod) {
    xr x; xdecode(b, 15, &x);
    decoded d; int got = decode(b, &d);
    char form[96];
    if (x.ok && x.scope) return;                        /* in scope: the positive sweep owns it */
    if (!x.ok) snprintf(form, sizeof form, "invalid.%s.%d.%d.%02x", vex ? "vex" : "leg", pp, map, opc);
    else snprintf(form, sizeof form, "%s", x.iclass);
    for (char *p = form; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    negs++;
    result("decoder-neg", form, vex ? "vex" : "legacy", got == 0, b, 8, "decode() accepted it");
    (void)tag; (void)mod;
}

static void negative(void) {
    static const uint8_t pf[8][3] = {{0}, {0x66}, {0xF2}, {0xF3}, {0x66, 0xF3}, {0x66, 0xF2}, {0xF3, 0xF2}, {0xF2, 0xF3}};
    for (int map = 1; map <= 3; map++) for (int pp = 0; pp < 4; pp++) for (int W = 0; W < 2; W++) for (int L = 0; L < 2; L++)
    for (int opc = 0; opc < 256; opc++) for (int ext = 0; ext < 8; ext += 7) for (int mod = 0; mod < 2; mod++) {
        uint8_t b[16] = {0xC4, (uint8_t)(0xE0 | map), (uint8_t)(W << 7 | 0x78 | L << 2 | pp), (uint8_t)opc,
                         (uint8_t)((mod ? 0x04 : 0xC0) | ext << 3 | (mod ? 0 : 1)), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        neg_one(b, 1, 0, map, pp, opc, mod);
        b[0] = 0xC5; b[1] = (uint8_t)(0x78 | L << 2 | pp); memmove(b + 2, b + 3, 13);   /* 2-byte VEX: map 1 */
        if (map == 1 && !W) { b[3] = (uint8_t)((mod ? 0x04 : 0xC0) | ext << 3 | (mod ? 0 : 1)); neg_one(b, 1, 1, 1, pp, opc, mod); }
    }
    for (int p = 0; p < 8; p++) for (int rw = 0; rw < 2; rw++) for (int map = 1; map <= 3; map++)
    for (int opc = 0; opc < 256; opc++) for (int ext = 0; ext < 8; ext += 7) for (int mod = 0; mod < 2; mod++) {
        if (map == 1 && (opc == 0x38 || opc == 0x3A)) continue;
        uint8_t b[16] = {0}; int n = 0;
        for (int q = 0; q < 3 && pf[p][q]; q++) b[n++] = pf[p][q];
        if (rw) b[n++] = 0x48;
        b[n++] = 0x0F; if (map == 2) b[n++] = 0x38; if (map == 3) b[n++] = 0x3A;
        b[n++] = (uint8_t)opc; b[n++] = (uint8_t)((mod ? 0x04 : 0xC0) | ext << 3 | (mod ? 0 : 1));
        neg_one(b, 0, p, map, p, opc, mod);
    }
}

/* ------------------------------------------------------------ prefix soup */
static void soup(void) {
    static const uint8_t alpha[] = {0x66, 0xF2, 0xF3, 0x48, 0x40, 0xF0, 0x67};
    static const struct { uint8_t op[3]; int n; const char *tag; } ops[] = {
        {{0x0F, 0xBC}, 2, "0fbc"}, {{0x0F, 0xBD}, 2, "0fbd"}, {{0x0F, 0xB8}, 2, "0fb8"},
        {{0x0F, 0x38, 0xF0}, 3, "0f38f0"}, {{0x0F, 0x38, 0xF1}, 3, "0f38f1"}};
    for (unsigned o = 0; o < sizeof ops / sizeof ops[0]; o++) {
        int ok[2] = {1, 1}; uint8_t first[2][16]; int fn[2] = {0, 0}; char why[2][64] = {"", ""};
        for (int len = 0; len <= 4; len++) {
            int total = 1; for (int i = 0; i < len; i++) total *= 7;
            for (int c = 0; c < total; c++) {
                uint8_t b[16] = {0}; int n = 0, cc = c;
                for (int i = 0; i < len; i++) { b[n++] = alpha[cc % 7]; cc /= 7; }
                for (int i = 0; i < ops[o].n; i++) b[n++] = ops[o].op[i];
                b[n++] = 0xC1;   /* register form: reg=0 rm=1 */
                xr x; xdecode(b, 15, &x);
                decoded d; int got = decode(b, &d);
                int want = x.ok && x.scope;
                int bad = want ? (got != x.len) : (got != 0);
                if (bad && ok[0]) { ok[0] = 0; memcpy(first[0], b, n); fn[0] = n; snprintf(why[0], 64, "(xed %s len %d, decode %d)", x.ok ? x.iclass : "invalid", x.len, got); }
                /* width: 66 / REX.W selection must match XED's operand size where both accept */
                if (want && got == x.len && d.is_bmi) {
                    xed_decoded_inst_t xx; xed_state_t st; xed_state_zero(&st);
                    st.mmode = XED_MACHINE_MODE_LONG_64; st.stack_addr_width = XED_ADDRESS_WIDTH_64b;
                    xed_decoded_inst_zero_set_mode(&xx, &st); xed_decoded_inst_set_input_chip(&xx, XED_CHIP_ALL);
                    xed_decode(&xx, b, 15);
                    int bits = (int)xed_decoded_inst_get_operand_width(&xx);
                    if (bits != d.opsize && ok[1]) { ok[1] = 0; memcpy(first[1], b, n); fn[1] = n; snprintf(why[1], 64, "(xed %d bits, decode %d)", bits, d.opsize); }
                }
            }
        }
        result("decoder-soup", ops[o].tag, "accept", ok[0], first[0], fn[0], why[0]);
        result("decoder-soup", ops[o].tag, "opsize", ok[1], first[1], fn[1], why[1]);
    }
}

/* ------------------------------------------------------------ random-bytes fuzz */
/* REX followed by another prefix is ignored by the CPU and never emitted by compilers; D5/D4 start APX. */
static int odd_prefixes(const uint8_t *b) {
    int rex = 0;
    for (int i = 0; i < 14; i++) {
        uint8_t c = b[i];
        int leg = c == 0x66 || c == 0x67 || c == 0xF0 || c == 0xF2 || c == 0xF3 || c == 0x2E || c == 0x36 || c == 0x3E ||
                  c == 0x26 || c == 0x64 || c == 0x65;
        if ((c & 0xF0) == 0x40) { if (rex) return 1; rex = 1; }
        else if (leg) { if (rex) return 1; }
        else return c == 0xD5 || c == 0xD4;
    }
    return 1;
}

/* Byte strings real toolchains emit; decode() must agree with XED on each. */
static void idioms(void) {
    static const struct { const char *name; uint8_t b[16]; } v[] = {
        {"tls-gd-call", {0x66, 0x66, 0x48, 0xE8, 0x10, 0, 0, 0}},
        {"tls-gd-lea", {0x66, 0x48, 0x8D, 0x3D, 0x10, 0, 0, 0}},
        {"nopw-cs", {0x66, 0x2E, 0x0F, 0x1F, 0x84, 0, 0, 0, 0, 0}},
        {"nopw-3x66", {0x66, 0x66, 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0, 0, 0, 0, 0}},
        {"endbr64", {0xF3, 0x0F, 0x1E, 0xFA}}, {"pause", {0xF3, 0x90}},
        {"vzeroupper", {0xC5, 0xF8, 0x77}}, {"vzeroall", {0xC5, 0xFC, 0x77}},
        {"stack-protector", {0x64, 0x48, 0x8B, 0x04, 0x25, 0x28, 0, 0, 0}},
        {"rep-ret", {0xF3, 0xC3}}, {"bnd-jmp", {0xF2, 0xE9, 1, 0, 0, 0}}, {"notrack-jmp", {0x3E, 0xFF, 0xE0}},
        {"movabs", {0x48, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8}}, {"mov-moffs", {0x48, 0xA1, 1, 2, 3, 4, 5, 6, 7, 8}},
        {"popcnt", {0xF3, 0x48, 0x0F, 0xB8, 0xC1}}, {"crc32-r8", {0xF2, 0x0F, 0x38, 0xF0, 0xC1}},
        {"crc32-r64", {0xF2, 0x48, 0x0F, 0x38, 0xF1, 0xC1}}, {"crc32-r16", {0x66, 0xF2, 0x0F, 0x38, 0xF1, 0xC1}},
        {"bsf", {0x0F, 0xBC, 0xC1}}, {"bsr-rexw", {0x48, 0x0F, 0xBD, 0xC1}}, {"rep-bsf", {0xF2, 0x0F, 0xBC, 0xC1}},
        {"xchg-ax", {0x66, 0x90}}, {"lock-add", {0xF0, 0x48, 0x01, 0x07}}, {"xacquire", {0xF2, 0xF0, 0x01, 0x07}},
        {"sse42-pcmpistri", {0x66, 0x0F, 0x3A, 0x63, 0xC1, 0x0C}}, {"vex-vpshufb-ymm", {0xC4, 0xE2, 0x75, 0x00, 0xC2}},
        {"avx1-vaddps", {0xC5, 0xF4, 0x58, 0xC2}}, {"vpermilps-imm", {0xC4, 0xE3, 0x7D, 0x04, 0xC1, 0x1B}},
    };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        uint8_t b[32]; memset(b, 0x90, sizeof b); memcpy(b, v[i].b, 16);
        xr x; xdecode(b, 15, &x);
        decoded d; int dl = decode(b, &d);
        result("decoder-idiom", v[i].name, "decode", x.scope ? dl == x.len : dl == 0, b, 12, "decode() wrong for it");
    }
}

static void fuzz(long iters) {
    static const uint8_t lp[] = {0x66, 0x67, 0xF2, 0xF3, 0xF0, 0x2E, 0x64, 0x65};
    static const uint8_t first[] = {0x0F, 0x0F, 0x0F, 0xC4, 0xC5};
    uint64_t s = 0x1234abcd5678ef01ull; int ok_dec = 1; char w2[64] = "";
        for (long i = 0; i < iters; i++) {
        uint8_t b[16]; int n = 0; uint64_t r = spec_rng(&s);
        for (int k = (int)(r & 3); k > 0; k--) b[n++] = lp[spec_rng(&s) & 7];
        if (spec_rng(&s) & 1) b[n++] = (uint8_t)(0x40 | (spec_rng(&s) & 15));
        uint64_t q = spec_rng(&s);
        if (q & 1) { b[n++] = first[(q >> 4) % 5]; if (b[n - 1] == 0x0F && (q & 6) == 2) b[n++] = (q & 8) ? 0x38 : 0x3A; }
        while (n < 16) b[n++] = (uint8_t)(spec_rng(&s) >> 24);
        if (odd_prefixes(b)) continue;
        xr x; xdecode(b, 15, &x);
        if (!x.ok) continue;
        xed_decoded_inst_t y; xed_state_t st; xed_state_zero(&st);   /* skip what no AVX1-era code contains */
        st.mmode = XED_MACHINE_MODE_LONG_64; st.stack_addr_width = XED_ADDRESS_WIDTH_64b;
        xed_decoded_inst_zero_set_mode(&y, &st); xed_decoded_inst_set_input_chip(&y, XED_CHIP_ALL);
        xed_decode(&y, b, 15);
        const char *ext = xed_extension_enum_t2str(xed_decoded_inst_get_extension(&y));
        if (strstr(ext, "AVX512") || strstr(ext, "APX") || strstr(ext, "AMX") || strstr(ext, "XOP") || strstr(ext, "KNC") ||
            strstr(ext, "AVX_VNNI") || strstr(ext, "MPX")) continue;
        xed_category_enum_t cat = xed_decoded_inst_get_category(&y);
        if (cat == XED_CATEGORY_SYSTEM || cat == XED_CATEGORY_VTX || !strcmp(x.iclass, "INSERTQ") || !strcmp(x.iclass, "EXTRQ")) continue;
        if (!strncmp(x.iclass, "MOV_CR", 6) || !strncmp(x.iclass, "MOV_DR", 6) || !strcmp(x.iclass, "UD0") || !strcmp(x.iclass, "UD1") || !strcmp(x.iclass, "UD2")) continue;
        if (cat == XED_CATEGORY_COND_BR || cat == XED_CATEGORY_UNCOND_BR || cat == XED_CATEGORY_CALL) {
            int has66 = 0; for (int k = 0; k < 14 && !((b[k] & 0xF0) == 0x40 || b[k] == 0x0F || b[k] == 0xE8 || b[k] == 0xE9 || (b[k] & 0xF0) == 0x70); k++) has66 |= b[k] == 0x66;
            if (has66) continue;          /* rel16 vs rel32 under 0x66 differs by vendor */
        }
        decoded d; int dl = decode(b, &d);
        if (dl && dl != x.len && ok_dec) {
            ok_dec = 0; snprintf(w2, 64, "(decode %d, xed %d %s)", dl, x.len, x.iclass);
            result("fuzz-len", "any", "decode", 0, b, x.len, w2);
        }
    }
    if (ok_dec) result("fuzz-len", "any", "decode", 1, 0, 0, 0);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    specform *f; int n = spec_load(argv[1], &f);
    if (n < 0 || known_load(argv[2]) < 0) return 1;
    const char *part = argc > 3 ? argv[3] : "all";
    long iters = argc > 4 ? atol(argv[4]) : 200000;
    xed_tables_init();
    int all = !strcmp(part, "all");
    if (all || !strcmp(part, "positive")) {
        long forms = 0;
        for (int i = 0; i < n; i++) if (strcmp(f[i].status, "refused") && positive(&f[i])) forms++;
        printf("positive: %ld forms\n", forms);
    }
    if (all || !strcmp(part, "negative")) { negative(); printf("negative: %ld encodings\n", negs); soup(); idioms(); }
    if (all || !strcmp(part, "fuzz")) fuzz(iters);
    return known_finish("") ? 1 : 0;
}
