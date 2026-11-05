#ifndef AVXEMU_TEST_CPU_H
#define AVXEMU_TEST_CPU_H

enum {
    CPU_AVX   = 1u << 0, CPU_AVX2 = 1u << 1, CPU_FMA   = 1u << 2, CPU_F16C  = 1u << 3,
    CPU_BMI1  = 1u << 4, CPU_BMI2 = 1u << 5, CPU_LZCNT = 1u << 6, CPU_MOVBE = 1u << 7,
    CPU_NFEATURES = 8
};

unsigned    cpu_features(void);
const char *cpu_feature_name(unsigned bit);
void        cpu_brand(char out[49]);

#endif
