/* F2 0F 38 F0/F1 is CRC32, not MOVBE: it must run natively, never be rewritten or emulated. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t ref8(uint32_t crc, const uint8_t *p, size_t n) {
    while (n--) { crc ^= *p++; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0x82F63B78u & -(crc & 1)); }
    return crc;
}
__attribute__((noinline)) static uint32_t crc_b(uint32_t c, const uint8_t *p) {
    __asm__ volatile("crc32b (%1),%0" : "+r"(c) : "r"(p) : "memory"); return c;
}
__attribute__((noinline)) static uint32_t crc_w(uint32_t c, const uint8_t *p) {
    __asm__ volatile("crc32w (%1),%0" : "+r"(c) : "r"(p) : "memory"); return c;
}
__attribute__((noinline)) static uint32_t crc_l(uint32_t c, const uint8_t *p) {
    __asm__ volatile("crc32l (%1),%0" : "+r"(c) : "r"(p) : "memory"); return c;
}
__attribute__((noinline)) static uint64_t crc_q(uint64_t c, const uint8_t *p) {
    __asm__ volatile("crc32q (%1),%0" : "+r"(c) : "r"(p) : "memory"); return c;
}
__attribute__((noinline)) static uint32_t crc_rb(uint32_t c, uint8_t v) {
    __asm__ volatile("crc32b %1,%0" : "+r"(c) : "r"(v)); return c;
}
int main(void) {
    static const uint8_t msg[] = "123456789";
    uint8_t buf[16]; memcpy(buf, msg, 9);
    uint32_t c = ~0u;
    for (int i = 0; i < 9; i++) c = crc_rb(c, msg[i]);
    if ((c ^ ~0u) != 0xE3069283u) { printf("crc32 reg FAIL %08x\n", c ^ ~0u); return 1; }
    c = ~0u; for (int i = 0; i < 9; i++) c = crc_b(c, buf + i);
    if ((c ^ ~0u) != 0xE3069283u) { printf("crc32b mem FAIL %08x\n", c ^ ~0u); return 2; }
    if (memcmp(buf, msg, 9)) { puts("crc32b mem clobbered buffer"); return 3; }
    uint8_t w[8] = {1, 2, 3, 4, 5, 6, 7, 8}, keep[8]; memcpy(keep, w, 8);
    if (crc_w(5, w) != ref8(5, w, 2) || crc_l(5, w) != ref8(5, w, 4) ||
        crc_q(5, w) != ref8(5, w, 8)) { puts("crc32 w/l/q FAIL"); return 4; }
    if (memcmp(w, keep, 8)) { puts("crc32 w/l/q clobbered buffer"); return 5; }
    puts("crc32-e2e: OK");
    return 0;
}
