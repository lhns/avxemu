#include "cpu.h"
#include <stdio.h>

int main(void) {
    unsigned f = cpu_features();
    const char *sep = "";
    for (int i = 0; i < CPU_NFEATURES; i++)
        if (f & (1u << i)) { printf("%s%s", sep, cpu_feature_name(1u << i)); sep = " "; }
    printf("\n");
    return 0;
}
