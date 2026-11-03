#include "spec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int spec_load(const char *path, specform **out) {
    FILE *fp = fopen(path, "r");
    if (!fp) { perror(path); return -1; }
    char line[1024]; int n = 0, cap = 0; specform *v = NULL;
    while (fgets(line, sizeof line, fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        line[strcspn(line, "\n")] = 0;
        char *col[24]; int nc = 0;
        for (char *p = line;; ) { col[nc++] = p; p = strchr(p, '\t'); if (!p || nc == 24) break; *p++ = 0; }
        if (nc != 19) { fprintf(stderr, "%s: bad row (%d columns): %.60s\n", path, nc, line); return -1; }
        if (n == cap) { cap = cap ? cap * 2 : 256; v = realloc(v, cap * sizeof *v); }
        specform *f = &v[n++]; memset(f, 0, sizeof *f);
        snprintf(f->id, sizeof f->id, "%s", col[0]); snprintf(f->iclass, sizeof f->iclass, "%s", col[1]);
        snprintf(f->isa, sizeof f->isa, "%s", col[2]);
        f->vex = atoi(col[3]); f->map = atoi(col[4]); f->pp = atoi(col[5]);
        f->opc = (int)strtol(col[6], 0, 16);
        f->ext = col[7][0] == 'x' ? -1 : atoi(col[7]);
        f->mem = !strcmp(col[8], "mem");
        f->W = col[9][0] == 'x' ? -1 : atoi(col[9]);
        f->L = col[10][0] == 'x' ? -1 : col[10][0] == '-' ? -2 : atoi(col[10]);
        f->imm8 = atoi(col[11]); f->opsz = atoi(col[12]);
        snprintf(f->ops, sizeof f->ops, "%s", col[13]); f->memw = atoi(col[14]);
        snprintf(f->memrw, sizeof f->memrw, "%s", col[15]);
        snprintf(f->name, sizeof f->name, "%s", col[16]); snprintf(f->status, sizeof f->status, "%s", col[17]);
        snprintf(f->reason, sizeof f->reason, "%s", col[18]);
    }
    fclose(fp); *out = v; return n;
}

uint64_t spec_rng(uint64_t *s) { uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return *s = x; }

int spec_encode(const specform *f, const specenc *e, uint8_t *o) {
    int n = 0;
    int reg = f->ext >= 0 ? f->ext : (e->reg & 15);
    int W = f->W >= 0 ? f->W : e->W, L = f->L >= 0 ? f->L : (f->L == -2 ? 0 : e->L);
    int rexR = f->ext >= 0 ? 0 : (e->reg >> 3) & 1, rexX = 0, rexB = 0;
    /* operand bytes: modrm [sib] [disp] */
    uint8_t m[8]; int mn = 0;
    if (!f->mem) { rexB = (e->rm >> 3) & 1; m[mn++] = (uint8_t)(0xC0 | (reg & 7) << 3 | (e->rm & 7)); }
    else if (e->rip) { m[mn++] = (uint8_t)((reg & 7) << 3 | 5); memcpy(m + mn, &e->disp, 4); mn += 4; }
    else {
        int base = e->base, idx = e->index, sc = e->scale == 8 ? 3 : e->scale == 4 ? 2 : e->scale == 2 ? 1 : 0;
        int mod, ds;
        if (base < 0) { mod = 0; ds = 4; }
        else if (e->disp == 0 && e->disp_size == 0 && (base & 7) != 5) { mod = 0; ds = 0; }
        else if (e->disp_size != 32 && e->disp >= -128 && e->disp < 128) { mod = 1; ds = 1; }
        else { mod = 2; ds = 4; }
        if (e->disp_size == 8 && base >= 0 && (base & 7) != 5) { mod = e->disp ? 1 : 1; ds = 1; }
        int needsib = idx >= 0 || base < 0 || (base & 7) == 4;
        if (needsib) {
            m[mn++] = (uint8_t)(mod << 6 | (reg & 7) << 3 | 4);
            int sidx = idx >= 0 ? (idx & 7) : 4, sbase = base >= 0 ? (base & 7) : 5;
            if (idx >= 0) rexX = (idx >> 3) & 1;
            if (base >= 0) rexB = (base >> 3) & 1;
            m[mn++] = (uint8_t)(sc << 6 | sidx << 3 | sbase);
        } else { rexB = (base >> 3) & 1; m[mn++] = (uint8_t)(mod << 6 | (reg & 7) << 3 | (base & 7)); }
        if (ds == 1) m[mn++] = (uint8_t)e->disp; else if (ds == 4) { memcpy(m + mn, &e->disp, 4); mn += 4; }
    }
    for (int i = 0; i < e->pad67; i++) o[n++] = 0x67;
    if (e->segpfx) o[n++] = (uint8_t)e->segpfx;
    if (f->vex) {
        int vvvv = (~e->vvvv) & 15;
        if (f->ext >= 0 || strstr(f->ops, "imm8") == 0) { /* vvvv as given */ }
        if (e->vex2 && f->map == 1 && !W && !rexX && !rexB) {
            o[n++] = 0xC5; o[n++] = (uint8_t)((!rexR) << 7 | vvvv << 3 | L << 2 | f->pp);
        } else {
            o[n++] = 0xC4; o[n++] = (uint8_t)((!rexR) << 7 | (!rexX) << 6 | (!rexB) << 5 | f->map);
            o[n++] = (uint8_t)(W << 7 | vvvv << 3 | L << 2 | f->pp);
        }
        o[n++] = (uint8_t)f->opc;
    } else {
        static const uint8_t pf[6][2] = {{0}, {0x66}, {0xF2}, {0xF3}, {0x66, 0xF3}, {0x66, 0xF2}};
        for (int q = 0; q < 2 && pf[f->pp][q]; q++) o[n++] = pf[f->pp][q];
        if (W || rexR || rexX || rexB) o[n++] = (uint8_t)(0x40 | W << 3 | rexR << 2 | rexX << 1 | rexB);
        o[n++] = 0x0F; if (f->map == 2) o[n++] = 0x38; if (f->map == 3) o[n++] = 0x3A;
        o[n++] = (uint8_t)f->opc;
    }
    memcpy(o + n, m, mn); n += mn;
    if (f->imm8) o[n++] = (uint8_t)e->imm;
    return n;
}
