# Developing

x86-64 Linux only. CMake >= 3.25, gcc or clang, GNU binutils (for the helper-library guard), Python 3.
No Docker or QEMU is needed.

```sh
cmake -S . -B build
cmake --build build -j
sh test/check-helpers-lib.sh build/libavxemu_helpers.a
ctest --test-dir build -L 'hermetic|replay' --output-on-failure   # about a minute, any x86-64 CPU
```

## What is in the repo

| path | what |
|---|---|
| `src/exec.c`, `exec_bmi.c`, `softfma.c` | the emulation core. Vector ops, FMA (exact integer arithmetic, not libm `fma()`, so results, NaN choice and flags match hardware) and scalar BMI. |
| `src/avxemu_helpers.h` | the contract of `libavxemu_helpers.a`: those three sources built AVX1-only, needing nothing from libc but `memcpy`/`memset`/`memmove` |
| `src/decode.c`, `emulate.c`, `names.c`, `regfile.h` | decoder and register-file emulator. Not in the library; the suite drives instructions through them |
| `src/platform/linux/memory.c`, `maps.c` | fault-safe memory reads and mapping queries for operands |
| `test/spec/` | the semantic suite (below) |
| `test/reference/` | recorded digests: `semantics.ref` (the replay contract), `semantics.intel.ref` (a second recording, not replayed), `oracle.ref`, `bmi_oracle.ref` |
| `test/check-helpers-lib.sh` | fails unless the archive is plain AVX1 code with no libc dependency; `--selftest` proves it fires on bad objects |
| `docs/vendor-differences.md` | measured Intel/AMD differences and the decision on each |

The archive is the ceiling for the client's CPU: the CI runner executes AVX2, BMI and FMA happily, so
only the guard script stops one from entering `libavxemu_helpers.a`.

## How lhns/v3compat uses it

v3compat fetches this repo at a pinned commit (`AVXEMU_COMMIT` in its `CMakeLists.txt`) with no CMake
project, and takes:

- `src/exec.c`, `exec_bmi.c`, `softfma.c`, built with its own flags into its helper archive; headers from `src/`;
- `test/check-helpers-lib.sh`, run on that archive;
- `test/spec/{sem_test.c,sem_hw.S,spec.c,known.c}`, `test/refdigest.c`, `test/cpu.c` (with `-I src -I test -I test/spec`),
  plus `test/spec/manifest.tsv` and `test/reference/semantics.ref`, run under its DynamoRIO client;
- `test/spec/manifest.tsv` as the list of forms the suite accounts for, regenerated with the same Intel XED
  this repo pins (see `spec.cmake`).

`sem_test.c` is `#include`d by the adapter, which supplies `decode`, `avxemu_emulate` and `avxemu_page_size`.
Changing those paths, the helper interface, `sem_test.c`'s entry points or `manifest.tsv`'s format needs a
matching v3compat change. Test a change by pointing its pin at your branch.

## The semantic suite (`test/spec/`)

Stages are ctest labels. The XED stages need `-DAVXEMU_TEST_XED=ON`, which fetches and builds pinned Intel XED
(test-only, Apache-2.0).

| label | what |
|---|---|
| `spec` | `manifest.tsv`: every AVX2/FMA3/BMI1/BMI2/LZCNT/F16C/MOVBE form from XED, each `tested`, `refused` or `failing`. `spec-complete` fails on a form without a status or one that disagrees with `decode()`; `spec-xed-fresh` that the manifest is what XED yields now |
| `decoder` | `decode()` against XED: every form with varied REX/VEX/addressing/padding, the whole opcode space for what must be rejected, prefix orderings, random bytes |
| `semantics` | each form through `decode()` and `avxemu_emulate()` against the real CPU. `semantics-self` and the `check` replays run anywhere; `silicon` tests need AVX2 |

`semtest` modes: `self` (emulator only: decodes, runs, deterministic), `check REF` (replay against recorded digests),
`live` (emulator vs. this CPU), `record FILE`, `fault`. `smoke` is a subset; `full` is all ~212k cases.
`semantics-replay-vex` and `-full-vex` run the same replay with the core taken from `libavxemu_helpers.a`,
which must give identical answers. `semantics-static-fault` checks fault handling in a static executable.

`test/known-failures.txt` lists failures that exist today (id, case glob, forms, reason). An unlisted failure
and a listed entry whose cases all pass both fail the run.

Environment knobs: `KNOWN_VERBOSE=1`; `SEM_ONLY=glob`, `SEM_DUMP=file` (one line per case, raw rflags, to
compare machines), `SEM_TRACE`, `SEM_DIFFMAX=n`.

Run a stage with `ctest --test-dir build -L spec` (likewise `decoder`, `semantics`). `-L semantics-full` is the
full replay, `-L silicon` the live comparison on an AVX2 host, `-L hardware` the oracle recordings.
QEMU TCG (`-L tcg`, if `qemu-x86_64` is installed) is only a stand-in oracle: it mishandles NaN payloads, fault
suppression and FMA flags.

### Re-recording a reference

Only when an emulator change is meant to change an output, or a form or op is added. Recording needs AVX2,
FMA and BMI silicon: `semtest record` refuses other CPUs and TCG.

- `semantics.ref`: run the `record-references` workflow (manual, or on a PR touching `src/` or `test/spec/`) on
  its AVX2 runner. It also runs every case live against the CPU. Commit the `semantics.ref` it uploads, on its
  own, saying why.
- `oracle.ref`, `bmi_oracle.ref`: on an AVX2 host run the `oracle-record` and `bmi_oracle-record` tests; each
  prints the `cp` that adopts the recording.
- Manifest: `test/spec/gen-manifest.sh BUILD_DIR` (configured with `-DAVXEMU_TEST_XED=ON`) regenerates
  `manifest.tsv` from XED. Changing XED's pin changes the manifest; v3compat regenerates its allow list from it.

`semantics.intel.ref` came from an Intel VM whose CPUID hides the model; it differs from `semantics.ref` in
one form, `movbe.m64_g.2`, and is not replayed. See `docs/vendor-differences.md`.

## CI

`.github/workflows/linux.yml`: gcc and clang on Ubuntu 22.04 and 24.04 build everything, run the helper-archive
guard, `-L 'hermetic|replay'`, the full replay (`-L semantics-full`, skipping live) and the oracle recordings
(which skip on CPUs lacking the instructions). `record-references.yml` is the silicon job.
