/* VCVTPH2PS quiets a signalling NaN: payload kept, quiet bit set. */
#include "softfma.h"
#include <assert.h>
#include <string.h>
static uint32_t bits(uint16_t h) { float f = half_to_float(h); uint32_t u; memcpy(&u, &f, 4); return u; }
int main(void) {
    assert(bits(0x7C01) == 0x7FC02000u);
    assert(bits(0xFC01) == 0xFFC02000u);
    assert(bits(0x7DFF) == 0x7FFFE000u);
    assert(bits(0x7E00) == 0x7FC00000u);   /* already quiet */
    assert(bits(0x7C00) == 0x7F800000u);   /* infinity is not a NaN */
    assert(bits(0xFC00) == 0xFF800000u);
    return 0;
}
