#ifndef AVXEMU_ASM_H
#define AVXEMU_ASM_H

/* Test code that executes AVX2, BMI or CPUID goes in its own executable section, away from the
 * code under test. */
#define AVXEMU_RAW_ATTR __attribute__((noinline, section(".avxemu_raw")))

#endif
