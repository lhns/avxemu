#ifndef AVXEMU_PLATFORM_CONTEXT_H
#define AVXEMU_PLATFORM_CONTEXT_H
#include "regfile.h"
/* Interrupted caller's signal mask, before handler-specific masking.
 * Returns 0/1, or -1 for an invalid context/signal. */
int avxemu_context_signal_blocked(const void *context, int signal);
uintptr_t avxemu_context_ip(const void *context);
void avxemu_context_set_ip(void *context, uintptr_t ip);
/* vectors=0 needs only integer state. Failures leave destinations untouched;
 * vector writes preserve x87, MXCSR and unrelated extended state. No allocation. */
int avxemu_context_read(const void *context, avxemu_regfile *rf, int vectors);
int avxemu_context_write(void *context, const avxemu_regfile *rf, int vectors);
/* The interrupted code's MXCSR (the handler itself starts from the reset value). */
int avxemu_context_mxcsr(const void *context, uint32_t *mxcsr);
int avxemu_context_set_mxcsr(void *context, uint32_t mxcsr);
#endif
