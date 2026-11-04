/* Manifest loader and instruction encoder shared by the spec, decoder and semantics tests. */
#ifndef AVXEMU_SPEC_H
#define AVXEMU_SPEC_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    char id[128], iclass[32], isa[12], ops[64], name[40], status[12], reason[160], memrw[4];
    int vex, map, pp;   /* pp: VEX 0 NP 1 66 2 F3 3 F2; legacy 0 NP 1 66 2 F2 3 F3 4 66+F3 5 66+F2 */
    int opc, ext;       /* ext: modrm.reg opcode extension, -1 when it is a register */
    int mem;            /* 0 register form, 1 memory form */
    int W, L;           /* -1: either value is the same form; L -2: not applicable (legacy) */
    int imm8, opsz, memw;
} specform;

/* Loads the manifest. Returns the form count or -1. *out is malloc'd. */
int spec_load(const char *path, specform **out);

typedef struct {
    int reg, vvvv, rm;          /* register numbers for modrm.reg, VEX.vvvv, and modrm.rm (register form) */
    int base, index, scale;     /* memory form; -1 = none; scale 1/2/4/8 */
    int32_t disp;
    int rip;                    /* [rip+disp] */
    int disp_size;              /* 0 smallest that works, 8, 32 */
    int W, L;                   /* used when the form leaves them open (-1) */
    int imm;
    int pad67, vex2;            /* leading 0x67 padding count; use C5 when the bits allow */
    int segpfx;                 /* 0, 0x64 or 0x65 */
} specenc;

int spec_uses_vvvv(const specform *f);

/* Encode one instance of f. Returns length (<= 15 plus padding). */
int spec_encode(const specform *f, const specenc *e, uint8_t *out);

uint64_t spec_rng(uint64_t *s);
#endif
