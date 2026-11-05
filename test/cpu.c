#include "cpu.h"
#include "asm.h"
#include <cpuid.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Every function here asks the CPU itself: a record-mode feature check must never be answered by
 * anything that emulates, or the emulator would be recorded against itself. */
#define RAW_CPUID AVXEMU_RAW_ATTR

RAW_CPUID static uint64_t xgetbv0(void) {
    uint32_t lo, hi;
    __asm__ volatile(".byte 0x0f,0x01,0xd0" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((uint64_t)hi << 32) | lo;
}

RAW_CPUID unsigned cpu_features(void) {
    const char *forced = getenv("AVXEMU_TEST_FEATURES");
    if (forced && *forced) return (unsigned)strtoul(forced, NULL, 16);
    unsigned a, b, c, d, f = 0, max = __get_cpuid_max(0, 0);
    if (max < 1 || !__get_cpuid(1, &a, &b, &c, &d)) return 0;
    int ymm = ((c >> 27) & 1) && ((xgetbv0() & 6) == 6);
    if (ymm && ((c >> 28) & 1)) f |= CPU_AVX;
    if (ymm && ((c >> 12) & 1)) f |= CPU_FMA;
    if (ymm && ((c >> 29) & 1)) f |= CPU_F16C;
    if ((c >> 22) & 1) f |= CPU_MOVBE;
    if (max >= 7) {
        __cpuid_count(7, 0, a, b, c, d);
        if ((b >> 3) & 1) f |= CPU_BMI1;
        if (ymm && ((b >> 5) & 1)) f |= CPU_AVX2;
        if ((b >> 8) & 1) f |= CPU_BMI2;
    }
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d) && ((c >> 5) & 1)) f |= CPU_LZCNT;
    return f;
}

const char *cpu_feature_name(unsigned bit) {
    static const char *const names[CPU_NFEATURES] =
        {"avx", "avx2", "fma", "f16c", "bmi1", "bmi2", "lzcnt", "movbe"};
    for (int i = 0; i < CPU_NFEATURES; i++) if (bit == (1u << i)) return names[i];
    return "?";
}

RAW_CPUID void cpu_brand(char out[49]) {
    unsigned r[12] = {0};
    memset(out, 0, 49);
    if (__get_cpuid_max(0x80000000u, 0) < 0x80000004u) { strcpy(out, "unknown"); return; }
    for (unsigned i = 0; i < 3; i++)
        __get_cpuid(0x80000002u + i, &r[4*i], &r[4*i + 1], &r[4*i + 2], &r[4*i + 3]);
    memcpy(out, r, 48);
    char *p = out; while (*p == ' ') p++;
    memmove(out, p, strlen(p) + 1);
}
