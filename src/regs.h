#ifndef AVXEMU_REGS_H
#define AVXEMU_REGS_H

#include <stdint.h>

/* RFLAGS bit positions (the ones BMI/logic ops touch). */
#define FLAG_CF (1ull << 0)
#define FLAG_ZF (1ull << 6)
#define FLAG_SF (1ull << 7)
#define FLAG_OF (1ull << 11)

#endif /* AVXEMU_REGS_H */
