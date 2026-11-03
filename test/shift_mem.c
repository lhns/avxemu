/* vps{ll,rl,ra}{w,d,q} ymm, ymm, m128 read 16 bytes, not 32: with the operand at the end of a
 * mapping followed by PROT_NONE they must not fault. "tramp": code in the main image
 * (AVXEMU_FORCETRAMP); "handler": in an mmap'd buffer. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

/* rdi = count operand, rsi = src (32 bytes), rdx = out (32 bytes) */
#define SHIFT_FN(name, op) \
    __asm__(".text\n.globl " #name "\n.type " #name ",@function\n" #name ":\n.cfi_startproc\n" \
            "vmovdqu (%rsi),%ymm1\n" #op " (%rdi),%ymm1,%ymm0\nvmovdqu %ymm0,(%rdx)\nvzeroupper\nret\n" \
            ".cfi_endproc\n.size " #name ",.-" #name "\n"); \
    void name(const uint8_t *, const uint8_t *, uint8_t *);
SHIFT_FN(sllw, vpsllw) SHIFT_FN(slld, vpslld) SHIFT_FN(sllq, vpsllq)
SHIFT_FN(srlw, vpsrlw) SHIFT_FN(srld, vpsrld) SHIFT_FN(srlq, vpsrlq)
SHIFT_FN(sraw, vpsraw) SHIFT_FN(srad, vpsrad)
typedef void (*fn)(const uint8_t *, const uint8_t *, uint8_t *);

/* kind: 0 sll, 1 srl, 2 sra; lane bytes 2/4/8 */
static void ref(int kind, int lane, uint64_t count, const uint8_t *src, uint8_t *out) {
    int bits = lane * 8;
    for (int i = 0; i < 32; i += lane) {
        uint64_t v = 0; memcpy(&v, src + i, lane);
        int64_t sv = (int64_t)(v << (64 - bits)) >> (64 - bits);
        uint64_t r;
        if (kind == 2) r = (uint64_t)(sv >> (count >= (uint64_t)bits ? bits - 1 : (int)count));
        else if (count >= (uint64_t)bits) r = 0;
        else r = kind == 0 ? v << count : v >> count;
        memcpy(out + i, &r, lane);
    }
}

int main(int argc, char **argv) {
    static const struct { fn f; unsigned char op; int kind, lane; } t[] = {
        {sllw, 0xF1, 0, 2}, {slld, 0xF2, 0, 4}, {sllq, 0xF3, 0, 8},
        {srlw, 0xD1, 1, 2}, {srld, 0xD2, 1, 4}, {srlq, 0xD3, 1, 8},
        {sraw, 0xE1, 2, 2}, {srad, 0xE2, 2, 4},
    };
    int handler = argc > 1 && !strcmp(argv[1], "handler");
    unsigned char *code = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    unsigned char *m = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (code == MAP_FAILED || m == MAP_FAILED || mprotect(m + 4096, 4096, PROT_NONE)) return 2;
    uint8_t src[32], out[32], want[32];
    for (int i = 0; i < 32; i++) src[i] = (uint8_t)(0x81 + i * 37);
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) {
        fn f = t[i].f;
        if (handler) {
            const unsigned char b[] = {0xc5,0xfe,0x6f,0x0e, 0xc5,0xf5,t[i].op,0x07, 0xc5,0xfe,0x7f,0x02, 0xc5,0xf8,0x77, 0xc3};
            memcpy(code, b, sizeof b); f = (fn)(void *)code;
        }
        static const uint64_t counts[] = {0, 3, 15, 16, 31, 33, 64, 200};
        for (unsigned c = 0; c < sizeof counts / sizeof *counts; c++) {
            uint8_t *op = m + 4096 - 16;               /* the operand ends at the PROT_NONE page */
            memset(op, 0xA5, 16); memcpy(op, &counts[c], 8);
            f(op, src, out);
            ref(t[i].kind, t[i].lane, counts[c], src, want);
            if (memcmp(out, want, 32)) { printf("shift_mem: op %02x count %llu wrong\n", t[i].op, (unsigned long long)counts[c]); return 1; }
        }
    }
    puts("shift-mem: OK");
    return 0;
}
