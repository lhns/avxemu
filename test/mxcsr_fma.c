/* Emulated FMA must honour the application's MXCSR (rounding, FTZ, DAZ) and leave its
 * sticky flags set. "tramp": the FMA sits in the main image (AVXEMU_FORCETRAMP).
 * "handler": it sits in an mmap'd buffer, which only the SIGILL handler can reach. */
#include <fenv.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <xmmintrin.h>

/* xmm0 = xmm1*xmm0 + xmm2 */
__asm__(".text\n.globl mxfma_img\n.type mxfma_img,@function\nmxfma_img:\n.cfi_startproc\n"
        "vfmadd213sd %xmm2,%xmm1,%xmm0\nret\n.cfi_endproc\n.size mxfma_img,.-mxfma_img\n");
double mxfma_img(double a, double b, double c);
typedef double (*fn)(double, double, double);

static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static int bad;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); puts(""); bad = 1; } } while (0)

int main(int argc, char **argv) {
    fn f = mxfma_img;
    if (argc > 1 && !strcmp(argv[1], "handler")) {
        static const unsigned char code[] = {0xc4, 0xe2, 0xf1, 0xa9, 0xc2, 0xc3};
        unsigned char *m = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) return 2;
        memcpy(m, code, sizeof code); f = (fn)(void *)m;
    }
    volatile double one = 1.0, tiny = 1e-300;
    fesetround(FE_UPWARD);
    CHECK(bits(f(one, one, tiny)) == 0x3FF0000000000001ull, "FE_UPWARD gives %016llx", (unsigned long long)bits(f(one, one, tiny)));
    fesetround(FE_TONEAREST);
    CHECK(bits(f(one, one, tiny)) == 0x3FF0000000000000ull, "nearest");
    /* FTZ: 2^-1022 * 0.5 is the subnormal 2^-1023 -> +0 */
    volatile double dmin = 2.2250738585072014e-308, half = 0.5, zero = 0.0;
    CHECK(bits(f(dmin, half, zero)) == 0x0008000000000000ull, "subnormal without FTZ");
    _mm_setcsr(_mm_getcsr() | 0x8000);
    CHECK(bits(f(dmin, half, zero)) == 0, "FTZ gives %016llx", (unsigned long long)bits(f(dmin, half, zero)));
    _mm_setcsr(_mm_getcsr() & ~0x8000u);
    /* DAZ: 2^-1074 * 2^1000 is 2^-74 unless the denormal input reads as zero */
    volatile double den = 4.9406564584124654e-324, big = 1.0715086071862673e+301;
    CHECK(bits(f(den, big, zero)) == bits(den * big) && bits(f(den, big, zero)) != 0, "DAZ off");
    _mm_setcsr(_mm_getcsr() | 0x40);
    CHECK(bits(f(den, big, zero)) == 0, "DAZ gives %016llx", (unsigned long long)bits(f(den, big, zero)));
    _mm_setcsr(_mm_getcsr() & ~0x40u);
    /* sticky flags */
    feclearexcept(FE_ALL_EXCEPT);
    (void)f(one, one, one);
    CHECK(!fetestexcept(FE_INEXACT), "exact FMA raised inexact");
    (void)f(one, one, tiny);
    CHECK(fetestexcept(FE_INEXACT), "inexact flag lost");
    feclearexcept(FE_ALL_EXCEPT);
    if (bad) return 1;
    puts("mxcsr-fma: OK");
    return 0;
}
