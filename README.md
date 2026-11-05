# AVXEmu helpers

Fork of AVXEmu, which emulates AVX2, FMA, BMI1/2, LZCNT, MOVBE and F16C instructions on CPUs that
have AVX but not AVX2. It was developed by @Wowfunhappy for Mavericks (see `PROVENANCE.md`) and is
CC0 (`LICENSE`).

This fork keeps only the part worth reusing: the instruction semantics and the test suite that
proves them against real silicon. The `LD_PRELOAD`/Mach-O mechanism that patched programs is gone
(it stays in git history).

| | |
|---|---|
| `libavxemu_helpers.a` | `src/exec.c`, `exec_bmi.c`, `softfma.c`: the emulation core as a freestanding AVX1 library. Contract in `src/avxemu_helpers.h`. |
| `src/decode.c`, `emulate.c` | decoder and register-file emulator; used by the test suite, not part of the library |
| `test/spec/` | the semantic suite: a manifest of every instruction form (from Intel XED), a case generator, and `semtest` |
| `test/reference/` | digests recorded on AVX2 silicon, replayed anywhere |
| `docs/vendor-differences.md` | where Intel and AMD silicon disagree, and what the suite does about it |

## Used by

[lhns/v3compat](https://github.com/lhns/v3compat) pins this repo at a commit. It compiles the three
helper sources itself (with VEX encoding), checks the result with `test/check-helpers-lib.sh`, and runs
`test/spec/sem_test.c`, `manifest.tsv` and `reference/semantics.ref` under its DynamoRIO client. Keep
those paths and the helper interface stable; see DEVELOPING.md.

Build and test: DEVELOPING.md.
