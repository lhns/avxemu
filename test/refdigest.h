#ifndef AVXEMU_TEST_REFDIGEST_H
#define AVXEMU_TEST_REFDIGEST_H
#include <stddef.h>
#include <stdint.h>

enum ref_mode { REF_CHECK, REF_RECORD };
struct ref_op { const char *name; uint64_t hw, emu; unsigned long cases, live_bad; };

#define REF_FNV_OFFSET 0xcbf29ce484222325ull

uint64_t      ref_fnv1a(uint64_t h, const void *p, size_t n);
enum ref_mode ref_init(int argc, char **argv, const char *suite, unsigned need);
int           ref_begin(enum ref_mode mode, const char *suite, const char *path);
void          ref_reset(void);
void          ref_op_init(struct ref_op *o, const char *name);
void          ref_hw(struct ref_op *o, const void *p, size_t n);
void          ref_emu(struct ref_op *o, const void *p, size_t n);
void          ref_case(struct ref_op *o, int live_ok);
int           ref_report(const struct ref_op *ops, int n);
int           ref_finish(void);
int           ref_put(const char *name, unsigned long cases, uint64_t digest);   /* record mode */
int           ref_get(const char *name, unsigned long *cases, uint64_t *digest); /* check mode; marks the line seen */

#endif
