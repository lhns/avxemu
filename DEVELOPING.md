# Developing AVXEmu

Build and test it now:

```sh
shipyard-cmake --preset native          # on Mac OS X 10.9; on a modern Mac use --preset cross
shipyard-cmake --build --preset native
shipyard-ctest --preset native
```

Done when ctest prints `100% tests passed`. Skips are fine. On the 10.9 machine (a 6-core Xeon
E5-1650 v2), measured on 2026-10-05, a clean configure and build takes about 11 s, and the
suite about 12 s, most of it `threadtest`.

`shipyard-cmake` comes from [shipyard](https://github.com/Mavergreen/shipyard). The build is
out of tree: `libavxemu.dylib` lands in `$TMPDIR/mm-build/<checkout>-<preset>/`.

On x86-64 glibc Linux, use CMake 3.25 or newer, GCC or Clang, GNU binutils, and Shipyard's
CMake package. Point `MavericksShipyard_DIR` at a Shipyard checkout or installed package
directory containing `MavericksShipyardConfig.cmake`:

```sh
export MavericksShipyard_DIR=/absolute/path/to/shipyard
AVXEMU_BUILD_DIR="${TMPDIR:-/tmp}/mm-build/avxemu-linux"
cmake -S . -B "$AVXEMU_BUILD_DIR" -DMavericksShipyard_DIR="$MavericksShipyard_DIR"
cmake --build "$AVXEMU_BUILD_DIR" -j2
ctest --test-dir "$AVXEMU_BUILD_DIR" --output-on-failure -L 'hermetic|replay'
ctest --test-dir "$AVXEMU_BUILD_DIR" --output-on-failure -L hardware
cmake --install "$AVXEMU_BUILD_DIR" --prefix "$HOME/.local/avxemu"
```

This installs `lib/libavxemu.so` and documentation, license and provenance under
`share/doc/AVXEmu` (both destinations honor GNUInstallDirs); see README for preload and direct linkage. Set `CC=clang`
on the initial configure to select Clang. Linux builds do not use the Darwin toolchain or
Sparkle updater. Keep build directories on local temporary storage, outside the source tree.

## Which machine runs which tests

Each test carries a label for what it needs. A test whose need is unmet skips.

| label | needs | runs on |
|---|---|---|
| `hermetic` | only the emulator, supported x86-64 CPU/OS | Darwin and Linux; macOS runtime CI uses a 10.9.5 VM |
| `replay` | only the emulator, checked against recordings from AVX2 silicon | everywhere, CI included |
| `hardware` | raw features required by each oracle, not Rosetta | capable x86-64 Darwin or Linux hosts |
| `claude-binary` | `CLAUDE_BIN=<a Claude Code binary>` | wherever you set it |
| `lowcpu` | full-system QEMU TCG, suitable kernel, static BusyBox | Linux; opt-in locally, required in CI |

To run one label: `shipyard-ctest --preset native -L replay`.

Linux uses `ctest --test-dir "$AVXEMU_BUILD_DIR" -L replay`. Feature-guarded hardware tests
report CTest skips when their raw CPU requirements are absent; supplying no `CLAUDE_BIN`
also skips application-dependent tests. The required `lowcpu` test never skips.

### Linux CI dependencies

The reusable Linux workflow runs GCC and Clang on literal Ubuntu 22.04 and 24.04 runners.
`test/prepare-linux-ci.sh TOOL_DIR ENV_FILE` installs declared Ubuntu build prerequisites,
checks CMake >=3.25 (using checksum-pinned Kitware 3.28.3 when necessary), builds QEMU, and
extracts a checksum-pinned Ubuntu kernel into temporary storage. `ENV_FILE` receives
`CMAKE_BIN`, `QEMU_SYSTEM_X86_64`, `LOWCPU_KERNEL`, and `LOWCPU_BUSYBOX`; add `CMAKE_BIN`
to PATH before configuring. No tool or kernel is installed into an AVXEmu release.

The QEMU source is official 11.1.2, SHA256
`731b5681e4bb18be313231579b8efd0296c5b015fa36dc533874b639ba838016`.
It **requires** `test/patch-lowcpu-qemu.sh`: pristine 11.1.2 swaps TZCNT's BMI1 gate and
LZCNT's ABM gate. The helper accepts only the exact original decoder checksum, applies the
two-predicate correction without fuzz, and verifies the result checksum. The CI build's
package suffix identifies this correction. QEMU's source declares its Python build tools
and pinned DTC subproject; the helper supplies compiler, Ninja, Python/venv/TOML, glib, pixman
and zlib dependencies on either Ubuntu target. QEMU itself always builds with GCC,
independently of the GCC/Clang runtime matrix. An unavailable download or failed build
fails CI; distro QEMU is never substituted. QEMU 8.2 also fails invalid-LOCK controls.

The boot kernel is Ubuntu's signed `linux-image-6.14.0-37-generic`, package version
`6.14.0-37.37~24.04.1`, SHA256
`9c17bb3f50741bf8f21b0f8a2afd088f5d0ae5b0d1cc00b7a866a63c8492d29a`.
The same kernel boots each runner's constructed initramfs, with that runner's glibc and
built test programs. This tests Ubuntu userspaces; it does not install/boot an Ubuntu VM
image or depend on the runner's host kernel package being readable.

To opt in locally after providing those tools:

```sh
export QEMU_SYSTEM_X86_64=/absolute/path/to/patched/qemu-system-x86_64
export LOWCPU_KERNEL=/absolute/path/to/vmlinuz
export LOWCPU_BUSYBOX=/absolute/path/to/static/busybox
cmake -S . -B "$AVXEMU_BUILD_DIR" -DMavericksShipyard_DIR="$MavericksShipyard_DIR" -DAVXEMU_TEST_LOWCPU=ON
cmake --build "$AVXEMU_BUILD_DIR" -j2
ctest --test-dir "$AVXEMU_BUILD_DIR" --output-on-failure -L lowcpu
```

The runner uses full-system TCG because qemu-user lacks `process_vm_readv` support needed
by safe operand reads. All fork/exec children stay in the guest. Sandy Bridge, LZCNT-only,
and BMI1-only models check raw CPUID/XGETBV, unsupported AVX2 and invalid-LOCK SIGILL
controls, advertised count semantics before preload, context restoration, ordinary/stripped
images, all relocation/native/minspill settings, cold/warm caches and concurrent relocation.
The 96 semantic launches and guest logs remain under the build directory's `lowcpu-logs/`.
This is emulated AVX1 evidence. A real Claude Code task on physical AVX1 Linux hardware is
separate application acceptance, not implied by these probes.

Linux deliberately calls CTest directly: Shipyard's `run-repo-tests.sh PRESET` currently
requires the macOS-only `shipyard-ctest` executable. The Shipyard install action provides
`MavericksShipyard_DIR` and `SHIPYARD_SCRIPTS` on Linux. Each job runs the complete
hermetic/replay label registry, raw-feature-guarded hardware registry and required lowcpu
label, with family conventions and source-tree cleanliness checks. Shipyard retains
platform-neutral conventions, Darwin preset execution, packaging, signing and publication.
Publication requires Linux success as well as the existing macOS build and key scan.

**How replay works.** `oracle`, `bmi_oracle`, `patchtest` and `bmimem` run each instruction on
the real CPU and in the emulator side by side, in `record` mode, and write
`test/reference/<name>.ref`: per op, a digest of the hardware's outputs. `check` mode replays the
emulator alone against that file. So the emulator is checked against silicon on CPUs that lack
the instructions.

### Correctness suite (`test/spec/`, `test/apps/`)

Stages, each a label; the XED ones need `-DAVXEMU_TEST_XED=ON` (fetches pinned Intel XED, test-only):

| label | what |
|---|---|
| `spec` | `manifest.tsv`: every AVX2/FMA3/BMI1/BMI2/LZCNT/F16C/MOVBE form from XED, each `tested`, `refused` or `failing`; `spec-complete` fails on a form without status or one that disagrees with `decode()` |
| `decoder` | `decode()` and `x86_len()` against XED: every form with varied REX/VEX/addressing/padding, the whole opcode space for what must be rejected (crc32, bsf/bsr, SSE4.2, AVX1), prefix orderings, random bytes |
| `semantics` | each form through the real decoder and `avxemu_emulate()` against the real CPU; `semantics-self` and the `check` replay run anywhere, `silicon` tests need AVX2 |
| `apps` | zstd, openssl, sqlite, numpy, duckdb natively and preloaded, sha256 of the output compared; `AVXEMU_APPS_REMOTE` runs them through e.g. `ssh avx1-host` |

`test/known-failures.txt` lists the failures that exist today (id, case glob, forms, reason). A failing case
that is not listed, and a listed entry whose cases all pass, both fail the run. Regenerate the manifest with
`test/spec/gen-manifest.sh BUILD_DIR`. The semantics reference (`test/reference/semantics.ref`) must be
recorded on AVX2 silicon: run the `record-references` workflow and commit its artifact. QEMU TCG
(`ctest -L tcg`) is only a stand-in oracle; it mishandles NaN payloads, fault suppression and FMA flags.

Run a stage with `ctest --test-dir "$AVXEMU_BUILD_DIR" -L spec` (likewise `decoder`, `semantics`, `apps`;
`-L semantics-full` is the full-size replay, `-L silicon` the live comparison on an AVX2 host).
The default `-L 'hermetic|replay'` run takes about a minute. `semtest record` refuses non-AVX2 CPUs and QEMU TCG (a KVM guest named QEMU runs on the host CPU and may record).

**Re-recording a reference:** `oracle`, `bmi_oracle`, `patchtest`, `bmimem`: see INGREDIENTS.md, "Releasing".
`semantics.ref` and `apps/expected.tsv`: run the `record-references` workflow (manual, or on a PR; it
has a read-only token and no secrets) on its AVX2 runner and commit the artifact it uploads.

`semantics.intel.ref` is a second recording, not replayed by any test: `semantics.ref` stays the replay
contract. It came from an Intel VM whose CPUID hides the model. Intel is measured, not taken from the
vendor string: its `rcpps`/`rsqrtps` results match Ivy Bridge and two Xeons and differ from AMD, and it has
no SSE4A. Haswell-class is inferred: AVX2, FMA and BMI run; ADX, SHA, CLFLUSHOPT and RTM are `#UD`.
It differs from `semantics.ref` in one form, `movbe.m64_g.2` (`66 REX.W 0F 38 F1`): in 36 of 81 cases that
fault on a page-split store, memory after the fault differs. A Xeon 6975P matches `semantics.ref` there, so
this is CPU generation, not vendor. `SEM_DUMP=file` writes one line per case, with raw rflags, to compare
machines; it also shows the BMI/LZCNT/TZCNT flags the SDM leaves undefined, which digests mask and which do
differ between Intel and AMD. Every measured difference and the decision on it: [docs/vendor-differences.md](docs/vendor-differences.md).

## Mavericks CI

The release workflow cross-builds on macos-26 and runs the `vex-leak-*` object checks there.
It bundles the generated CTest registry, binaries, committed source, and installed universal
Shipyard tools for a real 10.9.5 guest. Restoring their original absolute paths preserves
CTest commands and test data without maintaining a second test list. The guest installs
this run's package, verifies its version and dylib bytes, then replaces the build dylib
with a symlink to the installed one so both linked and inserted tests use the package.
The `mavericks-vm` test preset excludes only the host-side inventory checks and the
optional Claude binary tests. Hardware oracles must run; only `threadtest` may skip.

The guest uses `cpu-isa: avx2`: KVM can mask CPUID but cannot make AVX2 instructions fault
on an AVX2 host. Physical Mavericks AVX-only tests remain necessary for Claude Code and
runtime relocation. Linux CI uses full-system TCG to test genuinely missing instructions.
Rosetta is no longer an acceptance target; the former expected-failure marks on
`cachetest` and `overread-fault` have been removed. Neither is waived on Mavericks.

## Debugging

The first thing to run on a new machine, without Claude Code. It prints PASS or FAIL:

```sh
AVXEMU_SELFTEST=1 DYLD_INSERT_LIBRARIES=/usr/local/mavergreen/avxemu/lib/libavxemu.dylib /usr/bin/true
```

On macOS newer than 10.9, name a program of your own instead of `/usr/bin/true`: SIP strips
`DYLD_INSERT_LIBRARIES` from system binaries.

The dylib is silent in normal use. A `#UD` it can't emulate is chained to the program's own
handler, so a coverage gap shows up as Bun's "illegal instruction" crash report, with the
faulting address.

| env var | effect |
|---|---|
| `AVXEMU_SELFTEST=1` | check trap → decode → emulate → writeback on this CPU, then exit |
| `AVXEMU_DISABLE=1` | bypass the emulator entirely |
| `AVXEMU_NO_REBIND=1` | when linked rather than inserted, skip rebinding `sigaction`/`signal` |
| `AVXEMU_NOCACHE=1` | analyse at every launch: neither read nor write the load-time cache |
| `AVXEMU_CACHE_DIR=<dir>` | override the platform cache directory with an absolute path (Darwin: `~/Library/Caches/avxemu`; Linux: absolute `$XDG_CACHE_HOME/avxemu`, otherwise `$HOME/.cache/avxemu`) |
| `AVXEMU_CACHE_STATS=1` | report whether this launch hit, missed or refused the cache |
| `AVXEMU_MINSPILL=0` | turn off the minimal-spill tier, a live-register thunk for a single register-operand BMI op (on by default) |
| `AVXEMU_NATIVE=0` | turn off the generated native thunks, vector and scalar BMI, MULX included (on by default) |
| `AVXEMU_FORCETRAMP=1`, `AVXEMU_FORCEPATCH=1` | tests only: force trampolining or lzcnt patching on an AVX2 CPU |
| `AVXEMU_ASSUME_CAPABLE=1` | tests only: stay inert, as on a CPU that has everything |

**The risk the tests don't remove:** an emulation that is wrong in a way the oracles never
exercised corrupts results silently. The defences are the exhaustive differential oracles and
`AVXEMU_SELFTEST` on the target. The check that forced-trampolined output is byte-identical to
native, the `claude-trampoline` test, skips on both release hosts today, so it is not exercised
now: the 10.9 machine has no AVX2 to run the native side, and macOS 15 places no trampoline.

## How it works

On a CPU that already has AVX2, FMA, BMI1, BMI2, LZCNT, F16C and MOVBE, the load-time
constructor returns at once, and avxemu stays inert. Anywhere else it arms four mechanisms.

1. **Trampolines, the fast path.** Before the program runs, avxemu scans the main executable's
   code ranges, guided by normalized function bounds. It rewrites each run of faulting instructions into
   a 5-byte `jmp` to a generated thunk. The thunk saves the registers, emulates on a per-thread
   side stack, restores them and jumps back. About 97% of faulting sites never trap.
   Jump-table functions are mapped by recursive descent. (`src/tramp.c`, `src/tramp.S`,
   `src/lde.c`)
2. **The `SIGILL` handler, the fallback.** It covers sites too small or too close to a branch
   target for a 5-byte jump. It decodes at `rip`, reads the real `ymm` registers from the AVX
   signal frame (an AVX1 CPU has them, so no shadow register file), emulates, writes back and
   advances `rip`. Genuine `ud2` traps are chained, never swallowed. After a site traps once,
   avxemu tries to relocate a window of the instructions around it into its code cache and
   patch the site with a `jmp`, so it stops trapping. If it can't prove that safe, the site
   keeps using the handler. (`src/handler.c`, `src/reloc.c`)
3. **lzcnt/tzcnt patching.** These don't fault on a pre-Haswell CPU: they silently run as
   `bsr`/`bsf` and give wrong answers. avxemu rewrites each one's `F3` prefix to `F0` (lock) in
   mapped memory, so it faults and gets emulated. The file on disk is never changed.
   (`src/patch_mem.c`)
4. **The over-read fixup.** Optimized code reads a full vector past a buffer's end, relying on
   the next page being mapped. On a `SIGSEGV`/`SIGBUS` just past a mapped region, avxemu maps a
   zero page and retries. Anything else is chained to the previous handler.

**One emulator core.** The trampolines and the handler both call `avxemu_emulate()`
(`src/exec.c`, `src/exec_bmi.c`, `src/softfma.c`). It is compiled SSE4.2-only with `-mno-avx`,
and a test checks it emits no VEX. Most AVX2 integer ops work per 128-bit lane, so the core
applies the SSE equivalent to each half (`HALFOP` in `exec.c`).

**Staying first in line.** Bun installs its own `SIGILL` reporter. avxemu interposes
`sigaction`/`signal`, so a program's `SIGILL` handler becomes avxemu's chain target instead of
replacing it. The real `sigaction` is found by parsing `libsystem_c.dylib`'s symbol table,
because interposition breaks `dlsym` on Darwin. Linux resolves the next loader definition
and uses ELF symbol interposition for preload and retained direct linkage.

**Two ways to load it:**

- **Inserted:** `DYLD_INSERT_LIBRARIES`. No setup, but every child process inherits it.
- **Linked:** `change_dylib -insert` (see [Drydock](https://github.com/Mavergreen/drydock)), so
  dyld initializes it before every other dependency. On this path avxemu does dyld's
  interposition itself, by rebinding each loaded image's `sigaction`/`signal` pointers.

Mavericks Forever's `claude` wrapper inserted it only when
`sysctl machdep.cpu.leaf7_features` showed no AVX2. That is a good pattern for any integrator.

## Why emulate instead of rebuilding

Claude Code ships as one `bun build --compile` executable, and the only `darwin-x64` build needs
AVX2. Moving its JavaScript onto a stock baseline Bun does not work: Claude Code runs on a
private Bun fork, which may carry attestation. Emulation keeps the genuine binary intact and
only supplies the instructions the CPU lacks.

The floor is AVX1. A CPU without AVX at all (Nehalem, Core 2) has no hardware `ymm` registers,
so it would need a software register file. That is not attempted.

## What must be emulated

Measured by disassembling Claude Code 2.1.166 (15,380,168 instructions):

| class | static count | difficulty |
|---|---|---|
| AVX2 256-bit integer (`vpaddd`, `vpshufb`, …) | ~95k ALU ops | easy: per-lane half-split |
| AVX2 cross-lane (`vperm*`, `vpbroadcast*`, `vpmovzx*`) | thousands | real lane logic |
| FMA3, mostly scalar `sd` | 419 | needs correct fused rounding (software FMA) |
| BMI1/BMI2, LZCNT, TZCNT, MOVBE | ~38,000 | easy, individually |
| F16C (`vcvtph2ps`) | 4 | easy |

Two facts keep this tractable:

- **Gathers are rare:** `vpgather*` appears zero times in that binary. They are emulated anyway (`emulate.c`, VSIB, `d->vsib_index`),
  element by element into the register file, so a fault leaves the architectural partial state. They touch memory, so they
  are not in the helper archive: a client that only links `libavxemu_helpers.a` has to expand them itself.
- **The 256-bit moves are AVX1:** `vmovups`, `vmovdqu` and `vmovaps` (~155k) run natively.

## Performance

Per emulated instruction, measured on Haswell:

| path | cost |
|---|---|
| `SIGILL` trap → kernel → handler → return | ~4,200 ns |
| trampoline thunk | ~113 ns |
| native AVX1 doing the same work | ~0.76 ns |

End to end, on a real CPU-bound `claude` launch:

- **Steady-state execution:** about 1.2× native. AVX2 is a thin slice of what runs.
- **Startup:** the first launch of a given binary analyses its 60 MB of `__text`, about 2.2 s
  for Claude Code on an Ivy Bridge. That launch records the result in `~/Library/Caches/avxemu`
  (`src/lcache.c`), and later launches replay it in about 0.04 s. A replay checks every recorded
  site against the bytes there, and analyses afresh on any mismatch.
- **SIMD-heavy bursts:** slower than that.

Only a native build without AVX2 beats this.

## What is proven, and how

- **Emulator core:** bit-for-bit against hardware. The vector/FMA and BMI oracles, plus a
  140k-run native-vs-emulated fuzzer over the real binary.
- **Decoder:** every VEX/BMI instruction in the real binary (178,750, zero mismatches). This
  corpus caught two crash bugs random testing missed (`0x67`-padded VEX, `MOVBE` store) and a
  BMI2 memory-operand width bug.
- **lzcnt/tzcnt patch:** 5,158 sites patched, each checked `F3`→`F0` and a real zcnt.
- **Trampolines:** forced-trampolined output was byte-identical to native where a trampoline
  could be placed. Its test, `claude-trampoline`, skips on both release hosts today (see
  "Debugging"), so this is not re-checked now.
- **On a real Ivy Bridge Mac:** the AVX1 signal-frame layout, coexistence with Bun's handlers,
  and end-to-end use. The decisive bug found there: the thunk used `sub` to set up its frame,
  which clobbered the flags before saving them, so a branch after a scheduled `vpbroadcast` went
  wrong. The fix is `lea`. `tramptest` seeds every flag to catch this class of bug.

## Where things are

**The emulator** (`src/`):

| file | what |
|---|---|
| `decode.c`, `lde.c` | instruction decoder; length decoder and recursive-descent scanner |
| `exec.c`, `exec_bmi.c`, `softfma.c` | the SSE-only emulator core |
| `avxemu_helpers.h` | contract of `libavxemu_helpers.a`, the same three sources built AVX1-only for other clients (MXCSR passed in and out, never faults); `test/check-helpers-lib.sh` guards the archive, `semantics-replay-vex` runs the suite against it |
| `tramp.c`, `tramp.S`, `reloc.c` | trampoline scanner, installer and thunk template; runtime relocation of trapping sites |
| `lcache.c` | the load-time cache: records the load-time passes' result, replays it |
| `handler.c`, `patch_mem.c` | the constructor, signal handlers, lzcnt/tzcnt patcher |
| `selftest.c`, `selftest.S` | `AVXEMU_SELFTEST` |

The headers `ymm.h`, `regs.h`, `regfile.h` and `vexops.h` define the 256-bit value, the CPU
state the emulator touches, the register file both paths fill, and the opcode enum. `names.c`
holds mnemonic strings for diagnostics.

**The tests** (`test/`, registered in `test/CMakeLists.txt`):

| file | checks |
|---|---|
| `oracle.c`, `bmi_oracle.c` | emulator vs hardware: vectors and FMA; BMI values and flags |
| `patchtest.c`, `bmimem.c` | emulator vs hardware: patched lzcnt/tzcnt; BMI with a memory operand |
| `bintest.c`, `zdecode.c`, `patchdiff.c` | decoder and lzcnt patch over the real Claude binary |
| `fuzz.c` | native vs emulated, full 16-`ymm` compare |
| `tramptest.c`, `memtest.c`, `inject.c` | thunks, every addressing mode, fault injection end to end |
| `overread*.c`, `guard_page.c` | over-read fixup, and that guard pages stay untouched |
| `threadtest.c` | runtime relocation while other threads run the same sites |
| `inerttest.c`, `linkhook.c` | inert on a capable CPU; keeping `SIGILL` when linked, not inserted |
| `cachetest.c` | the load-time cache: record, replay, identical result, bad files refused |
| `nativetest.c`, `minspilltest.c` | the generated native and minimal-spill thunks vs the C emulator |

**The rest:** `packaging/build-pkg.sh` builds the `.pkg` with shipyard, `contrib/` holds scripts
for consumers (`extract-libavxemu.sh`), and `.github/workflows/` holds CI.

## Platform architecture

One emulator, instruction decoder, scanner, patcher, trampoline/relocation engine and cache
serve Darwin and Linux. Runtime C is compiled with `-msse4.2 -mno-avx -mno-fma`; explicit
assembly and hardware-oracle objects carry stronger instructions. `avxemu_emulate()` consumes
the same register file from either signal context or trampoline path.

`src/image.h` represents immutable, bounded code ranges, function spans, authoritative
data exclusions and discovery policy. Platform image backends translate Mach-O/dyld or
ELF/loader metadata into this model. `src/platform/context.h`, `memory.h`, `loader.h` and
`host.h` expose saved register access, checked mapping/read/protection operations, signal
installation/interposition, CPU/OS services and cache paths. Native signal frames, Mach
services, ELF and Mach-O parsing stay in the backends; shared algorithms use these contracts.
Vector context writes preserve unrelated xstate, x87 and MXCSR. Fault-time operations use
bounded storage; Linux self-memory reads require `process_vm_readv` permission. A sandbox
that forbids it cannot supply safe operand reads and is not a supported runtime environment.

Load-time rewriting covers the main executable, not every loaded library. Linux supports
glibc x86-64 dynamically linked PIE and non-PIE executables, including explicit-loader
invocation when the mapped file can be identified safely. It associates a readable file
with the actual loaded image before trusting build identity or metadata. Function bounds
come from validated ELF symbols and supported `.eh_frame_hdr`/`.eh_frame` records. Missing,
conflicting, malformed or ambiguous required-instruction coverage is refused; there is no
byte-scan fallback that treats possible data as code. A stripped binary can work when its
remaining unwind metadata proves the needed coverage. The parser is a bounded range reader,
not a general DWARF unwinder. Unsupported metadata/CPU/OS cases can reject an application.

The Linux interface is the shared library's source-build contract with the supported
glibc loader and signal ABI. Internal C structs and cache files are not a stable public
ABI; rebuild from source when changing toolchains/targets and allow incompatible caches to
be discarded. No Linux distribution package, musl support, static injection, pre-AVX CPU
support or cross-architecture emulation is promised.

Required-instruction discovery uses one strategy selected by the image backend.
Linux requires validated function bounds and direct recursive discovery; unresolved
indirect flow is accepted only when every possible required encoding is already
covered by a proven instruction prefix. Unknown candidate bytes fail startup.
Before excluding skipped islands, strict discovery checks direct edges from every
function, including candidate-free functions and other executable ranges. Proven
external entries seed the destination's direct map until no new entries remain.
Overlapping instructions, entries into declared data, or exhaustion of the bounded
scan budget refuse the image before patching. Function extents alone do not
establish that all entries have been discovered. Optional CPUID and
eager trampoline passes use direct roots too; unrelated LEA/table guesses cannot
authorize their writes, either during fresh analysis or cache replay.
Darwin retains its established linear-first function scan, recursive fallback,
and range mop-up for compatibility with existing Mavericks applications. This
legacy strategy is heuristic: it does not prove that every selected instruction
is reachable. Both strategies exclude verified data-in-code records and fully
resolved direct-jump islands before fresh writes. Strict cache replay rediscovers
and validates the complete prefix plan. Legacy cache replay retains the historical identity/integrity trust
model: all recorded edits are validated locally before any write, but their global
instruction boundaries and completeness are not re-proven against a maliciously
reconstructed cache. Invalid records trigger fresh discovery. Cache identity
includes the strategy and data layout.
Mach-O data-in-code parsing stays in the Darwin backend; shared consumers use
normalized bounded spans. No constant-state or interprocedural analysis is used.

## Releasing

See INGREDIENTS.md, "Releasing": which checks run by hand, on which machine, before a release.
