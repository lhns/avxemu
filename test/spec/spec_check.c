/*
 * spec-complete: every manifest form has a status, and the status matches what decode.c does.
 *   tested   decode() accepts the canonical encoding at its exact length
 *   failing  accepted, and a known-failures entry names the form
 *   refused  decode() rejects it (the emulator refuses loudly: no silent wrong answer)
 * usage: spec_check MANIFEST KNOWN [--propose]
 */
#include "spec.h"
#include "known.h"
#include "decode.h"
#include <fnmatch.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    specform *f; int n = spec_load(argv[1], &f);
    if (n < 0 || known_load(argv[2]) < 0) return 1;
    int propose = argc > 3, bad = 0, cnt[4] = {0};
    static const char *const st[] = {"tested", "refused", "failing", "UNSET"};
    for (int i = 0; i < n; i++) {
        specenc e = {.reg = 1, .vvvv = 2, .rm = 3, .base = 3, .index = -1, .scale = 1, .W = 0, .L = 0, .imm = 0x5a};
        uint8_t b[32] = {0}; decoded d;
        int len = spec_encode(&f[i], &e, b);
        int got = decode(b, &d);
        if (propose) { printf("%s\t%s\t%d\n", f[i].name, got == len ? "accepted" : got ? "WRONGLEN" : "rejected", got); continue; }
        int k;
        for (k = 0; k < 3; k++) if (!strcmp(f[i].status, st[k])) break;
        if (k == 3) { printf("%s: no valid status (%s)\n", f[i].name, f[i].status); bad++; continue; }
        cnt[k]++;
        if (k == 1 && got) { printf("%s: refused, but decode() accepts it\n", f[i].name); bad++; }
        if (k != 1 && got != len) { printf("%s: %s, but decode() returned %d for a %d-byte encoding\n", f[i].name, f[i].status, got, len); bad++; }
        if (k == 1 && !f[i].reason[0]) { printf("%s: refused without a reason\n", f[i].name); bad++; }
        int named = 0;
        for (int j = 0; j < known_count(); j++) if (known_get(j)->forms[0] != '-' && !fnmatch(known_get(j)->forms, f[i].name, 0)) named = 1;
        if (k == 2 && !named) { printf("%s: failing, but no known-failures entry names it\n", f[i].name); bad++; }
        if (k != 2 && named) { printf("%s: named by known-failures but status is %s\n", f[i].name, f[i].status); bad++; }
    }
    if (propose) return 0;
    for (int j = 0; j < known_count(); j++) {
        int hit = 0;
        for (int i = 0; i < n; i++) if (known_get(j)->forms[0] != '-' && !fnmatch(known_get(j)->forms, f[i].name, 0)) hit = 1;
        if (known_get(j)->forms[0] != '-' && !hit) { printf("known failure %s names no form\n", known_get(j)->id); bad++; }
    }
    printf("spec: %d forms: %d tested, %d refused, %d failing, %d problems\n", n, cnt[0], cnt[1], cnt[2], bad);
    return bad != 0;
}
