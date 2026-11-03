/* A 0x67 prefix truncates the effective address to 32 bits: with high garbage in the base
 * register, `vpand (%edi),%ymm1,%ymm0` must still read the low-memory operand.
 * "tramp": the code is in the main image (AVXEMU_FORCETRAMP); "handler": in an mmap'd buffer. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

/* rdi = base with garbage high bits, rsi = src1 (32 bytes), rdx = out (32 bytes) */
#define BODY \
    "\xc5\xfe\x6f\x0e"          /* vmovdqu (%rsi),%ymm1 */ \
    "\x67\xc5\xf5\xdb\x07"      /* vpand (%edi),%ymm1,%ymm0 */ \
    "\xc5\xfe\x7f\x02"          /* vmovdqu %ymm0,(%rdx) */ \
    "\xc5\xf8\x77\xc3"          /* vzeroupper; ret */
static const char body[] = BODY;
__asm__(".text\n.globl addr32_img\n.type addr32_img,@function\naddr32_img:\n.cfi_startproc\n"
        ".byte 0xc5,0xfe,0x6f,0x0e, 0x67,0xc5,0xf5,0xdb,0x07, 0xc5,0xfe,0x7f,0x02, 0xc5,0xf8,0x77, 0xc3\n"
        ".cfi_endproc\n.size addr32_img,.-addr32_img\n");
void addr32_img(uint64_t base, const uint8_t *src, uint8_t *out);
typedef void (*fn)(uint64_t, const uint8_t *, uint8_t *);

int main(int argc, char **argv) {
    fn f = addr32_img;
    if (argc > 1 && !strcmp(argv[1], "handler")) {
        unsigned char *m = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) return 2;
        memcpy(m, body, sizeof body - 1); f = (fn)(void *)m;
    }
    uint8_t *low = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_32BIT, -1, 0);
    if (low == MAP_FAILED || (uintptr_t)low >> 32) return 3;
    uint8_t src[32], out[32], want[32];
    for (int i = 0; i < 32; i++) { src[i] = (uint8_t)(0xF0 + i * 7); low[i] = (uint8_t)(0x3C ^ (i * 11)); want[i] = src[i] & low[i]; }
    memset(out, 0, sizeof out);
    f((0x1234ull << 32) | (uintptr_t)low, src, out);
    if (memcmp(out, want, 32)) { puts("addr32: wrong operand"); return 1; }
    puts("addr32: OK");
    return 0;
}
