/*
 * xedgen.c -- enumerate every AVX2/FMA3/BMI1/BMI2/LZCNT/F16C/MOVBE instruction form XED
 * knows, by decoding the whole VEX and legacy opcode space with XED's own tables.
 * Test-only; links libxed, never libavxemu. TSV on stdout (see gen-manifest.sh).
 *
 * W, L and modrm.reg collapse to "x" when XED decodes every value to the same iform.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "xed-interface.h"

static int in_scope(xed_isa_set_enum_t s) {
    switch (s) {
    case XED_ISA_SET_AVX2: case XED_ISA_SET_AVX2GATHER: case XED_ISA_SET_FMA:
    case XED_ISA_SET_BMI1: case XED_ISA_SET_BMI2: case XED_ISA_SET_LZCNT:
    case XED_ISA_SET_F16C: case XED_ISA_SET_MOVBE: return 1;
    default: return 0;
    }
}

struct form {
    char iform[96], iclass[32], isa[16], ops[96], rw[16];
    int vex, map, pp, opc, mod, imm, opsz, memw, extmask;
    int have[2][2];     /* decoded OK for [W][L] */
};
static struct form *forms; static int nforms, cap;

static int decode(const uint8_t *b, int n, xed_decoded_inst_t *x) {
    xed_state_t st; xed_state_zero(&st);
    st.mmode = XED_MACHINE_MODE_LONG_64; st.stack_addr_width = XED_ADDRESS_WIDTH_64b;
    xed_decoded_inst_zero_set_mode(x, &st);
    xed_decoded_inst_set_input_chip(x, XED_CHIP_ALL);   /* else F3 0F BD decodes as bsr */
    return xed_decode(x, b, n) == XED_ERROR_NONE;
}

static void opstr(xed_decoded_inst_t *x, char *out, char *rw, int *memw) {
    const xed_inst_t *in = xed_decoded_inst_inst(x);
    int n = xed_inst_noperands(in); out[0] = 0; *memw = 0;
    for (int i = 0; i < n; i++) {
        const xed_operand_t *o = xed_inst_operand(in, i);
        if (xed_operand_operand_visibility(o) != XED_OPVIS_EXPLICIT) continue;
        xed_operand_enum_t nm = xed_operand_name(o);
        char t[48];
        if (nm >= XED_OPERAND_REG0 && nm <= XED_OPERAND_REG9) {
            xed_reg_enum_t r = xed_decoded_inst_get_reg(x, nm);
            char *p; snprintf(t, sizeof t, "%s", xed_reg_class_enum_t2str(xed_reg_class(r)));
            for (p = t; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
        } else if (nm == XED_OPERAND_MEM0) {
            *memw = xed_decoded_inst_get_memory_operand_length(x, 0);
            snprintf(t, sizeof t, "m%d", *memw * 8);
        } else if (nm == XED_OPERAND_IMM0) snprintf(t, sizeof t, "imm8");
        else if (nm == XED_OPERAND_AGEN) snprintf(t, sizeof t, "agen");
        else if (nm == XED_OPERAND_BASE0) continue;
        else snprintf(t, sizeof t, "%s", xed_operand_enum_t2str(nm));
        if (out[0]) strcat(out, ",");
        strcat(out, t);
    }
    int rd = xed_decoded_inst_mem_read(x, 0), wr = xed_decoded_inst_mem_written(x, 0);
    snprintf(rw, 16, "%s%s", rd ? "r" : "", wr ? "w" : "");
    if (!rw[0]) strcpy(rw, "-");
}

static void consider(const uint8_t *b, int vex, int map, int pp, int opc, int ext, int mod,
                     int W, int L, int opsz) {
    xed_decoded_inst_t x;
    if (!decode(b, 16, &x)) return;
    xed_isa_set_enum_t s = xed_decoded_inst_get_isa_set(&x);
    if (!in_scope(s)) return;
    char iform[96]; snprintf(iform, sizeof iform, "%s", xed_iform_enum_t2str(xed_decoded_inst_get_iform_enum(&x)));
    char ops[96], rw[16]; int memw;
    opstr(&x, ops, rw, &memw);
    int k;
    for (k = 0; k < nforms; k++) {
        struct form *f = &forms[k];
        if (!strcmp(f->iform, iform) && f->vex == vex && f->map == map && f->pp == pp && f->opc == opc &&
            f->mod == mod && f->opsz == opsz && !strcmp(f->ops, ops)) break;
    }
    if (k == nforms) {
        if (nforms == cap) { cap = cap ? cap * 2 : 1024; forms = realloc(forms, cap * sizeof *forms); }
        struct form *f = &forms[nforms++]; memset(f, 0, sizeof *f);
        snprintf(f->iform, sizeof f->iform, "%s", iform);
        snprintf(f->iclass, sizeof f->iclass, "%s", xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(&x)));
        snprintf(f->isa, sizeof f->isa, "%s", xed_isa_set_enum_t2str(s));
        snprintf(f->ops, sizeof f->ops, "%s", ops); snprintf(f->rw, sizeof f->rw, "%s", rw);
        f->vex = vex; f->map = map; f->pp = pp; f->opc = opc; f->mod = mod;
        f->opsz = opsz; f->imm = xed_decoded_inst_get_immediate_width(&x) ? 1 : 0; f->memw = memw;
    }
    forms[k].have[W][L] = 1;
    forms[k].extmask |= 1 << ext;
}

static int cmp(const void *a, const void *b) {
    const struct form *x = a, *y = b;
    int c = strcmp(x->iclass, y->iclass); if (c) return c;
    c = strcmp(x->iform, y->iform); if (c) return c;
    return (x->opsz - y->opsz) ?: (x->mod - y->mod) ?: (x->opc - y->opc) ?: (x->pp - y->pp);
}

int main(void) {
    xed_tables_init();
    for (int map = 1; map <= 3; map++) for (int pp = 0; pp < 4; pp++)
    for (int W = 0; W < 2; W++) for (int L = 0; L < 2; L++)
    for (int opc = 0; opc < 256; opc++) for (int ext = 0; ext < 8; ext++) for (int mod = 0; mod < 2; mod++)
    for (int vv = 0; vv < 2; vv++) {   /* vvvv = xmm1, or unused (1111) */
        uint8_t b[16] = {0xC4, (uint8_t)(0xE0 | map), (uint8_t)((W << 7) | ((vv ? 0xE : 0xF) << 3) | (L << 2) | pp),
                         (uint8_t)opc, (uint8_t)((mod ? 0x04 : 0xC0) | (ext << 3) | (mod ? 0 : 1))};   /* mem: SIB, so VSIB gathers decode */
        consider(b, 1, map, pp, opc, ext, mod, W, L, 0);
    }
    static const uint8_t pf[6][2] = {{0}, {0x66}, {0xF2}, {0xF3}, {0x66, 0xF3}, {0x66, 0xF2}};
    for (int p = 0; p < 6; p++) for (int rw = 0; rw < 2; rw++) for (int map = 1; map <= 3; map++)
    for (int opc = 0; opc < 256; opc++) for (int ext = 0; ext < 8; ext++) for (int mod = 0; mod < 2; mod++) {
        if (map == 1 && (opc == 0x38 || opc == 0x3A)) continue;
        uint8_t b[16] = {0}; int n = 0;
        for (int q = 0; q < 2 && pf[p][q]; q++) b[n++] = pf[p][q];
        if (rw) b[n++] = 0x48;
        b[n++] = 0x0F; if (map == 2) b[n++] = 0x38; if (map == 3) b[n++] = 0x3A;
        b[n++] = (uint8_t)opc; b[n++] = (uint8_t)((mod ? 0x04 : 0xC0) | (ext << 3) | (mod ? 0 : 1));
        consider(b, 0, map, p, opc, ext, mod, rw, 0, rw ? 64 : (p == 1 || p >= 4 ? 16 : 32));
    }
    qsort(forms, nforms, sizeof *forms, cmp);
    printf("#id\ticlass\tisa\tvex\tmap\tpp\topc\text\tmod\tW\tL\timm8\topsz\tops\tmemw\tmemrw\n");
    static const char *ppv[4] = {"NP", "66", "F3", "F2"}, *ppl[6] = {"NP", "66", "F2", "F3", "66F3", "66F2"};
    for (int k = 0; k < nforms; k++) {
        struct form *f = &forms[k];
        int w0 = f->have[0][0] | f->have[0][1], w1 = f->have[1][0] | f->have[1][1];
        int l0 = f->have[0][0] | f->have[1][0], l1 = f->have[0][1] | f->have[1][1];
        const char *Ws = w0 && w1 ? "x" : w1 ? "1" : "0";
        const char *Ls = !f->vex ? "-" : l0 && l1 ? "x" : l1 ? "1" : "0";
        char ext[8], enc[160];
        if (f->extmask == 0xFF) strcpy(ext, "x");
        else snprintf(ext, sizeof ext, "%d", __builtin_ctz(f->extmask));
        snprintf(enc, sizeof enc, "%s.%s.%s.%s.W%s.%02X/%s.%s%s",
                 f->vex ? "VEX" : "LEG", f->vex ? (Ls[0] == 'x' ? "LIG" : Ls[0] == '1' ? "256" : "128") : "-",
                 (f->vex ? ppv : ppl)[f->pp], f->map == 1 ? "0F" : f->map == 2 ? "0F38" : "0F3A", Ws, f->opc, ext,
                 f->mod ? "m" : "r", f->vex ? "" : (f->opsz == 16 ? ".o16" : f->opsz == 64 ? ".o64" : ".o32"));
        printf("%s@%s\t%s\t%s\t%d\t%d\t%d\t0x%02X\t%s\t%s\t%s\t%s\t%d\t%d\t%s\t%d\t%s\n",
               f->iform, enc, f->iclass, f->isa, f->vex, f->map, f->pp, f->opc, ext,
               f->mod ? "mem" : "reg", Ws, Ls, f->imm, f->opsz, f->ops, f->memw, f->rw);
    }
    return 0;
}
