#ifndef AVXEMU_PLATFORM_MEMORY_H
#define AVXEMU_PLATFORM_MEMORY_H
#include <stddef.h>
#include <stdint.h>

enum { AVXEMU_PROT_R = 1, AVXEMU_PROT_W = 2, AVXEMU_PROT_X = 4 };
typedef struct { uintptr_t begin, end; unsigned prot; } avxemu_mapping;

/* The page size is cached on first use; the operations below use bounded storage and are
 * safe to call from a signal handler once it is cached. */
size_t avxemu_page_size(void);
/* 1: mapped (including protection zero), 0: verified gap, -1: unknown/error.
 * Failed queries leave mapping unchanged. */
int avxemu_memory_query(uintptr_t address, avxemu_mapping *mapping);
/* Return the readable prefix copied, without faulting on inaccessible input. */
size_t avxemu_memory_read(void *dst, uintptr_t src, size_t size);
#endif
