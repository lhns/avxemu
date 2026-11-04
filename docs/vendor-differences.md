# Intel and AMD differences

What `test/reference/semantics.intel.ref` shows against `semantics.ref` (AMD). "Measured" is a
value in a raw dump; "inferred" is a conclusion drawn from them.

## The recording

- 494 forms, 212,320 cases, same inputs as the AMD recording (0 inputs differ).
- Intel Haswell-class (inferred): a KVM guest reporting `GenuineIntel`, family 15, model 107. Bare
  metal was not tested. AVX2, FMA and BMI run, ADX, SHA, CLFLUSHOPT and RTM fault with `#UD`.
- Controls reproduced `semantics.ref` byte for byte on EPYC 7763, 9V74, 9V45 and Xeon 6975P, so the
  harness itself is not the source of any difference below.

## CPUs measured

Values come from `semtest` with `SEM_DUMP` (all 212,320 cases) and from `probe -v` of the v3compat
`test/allow` probe (five seeded states per form). Ivy Bridge has no AVX2/BMI, so only `probe -v`
ran there. CPU models were taken from `/proc/cpuinfo` of the machine that ran them.

| CPU | vendor, family/model | group below |
| --- | --- | --- |
| Intel Haswell-class (inferred), KVM guest | Intel, 15/107 | Intel |
| Xeon E3-12xx v2 (Ivy Bridge), KVM guest | Intel, 6/58 | Intel (`probe -v` only) |
| Xeon Platinum 8370C | Intel, 6/106 | Intel |
| Xeon Platinum 8573C | Intel, 6/207 | Intel |
| Xeon 6975P-C | Intel, 6/173 | Intel |
| EPYC 7763 | AMD, 25/1 | AMD |
| EPYC 9V74 | AMD, 25/17 | AMD |
| EPYC 9V45 | AMD, 26/2 | AMD |

Within each group every value below is identical, so none of it is a model difference, except
where a row says otherwise.

## Same on both (measured)

NaN priority, FMA and F16C values, MXCSR, integer results, in every form. Only the cases below differ.

## Differs

| what | Intel | AMD | status |
| --- | --- | --- | --- |
| Undefined flags of 40 BMI/LZCNT/TZCNT forms, 7,700 cases (e.g. `andn`, `lzcnt`), raw rflags | `202`, `242`, `203` | `a02` or `206`, `256`/`a42`/`246`, `207` respectively | measured; digests mask them. 1,509 cases `202`→`a02`, 1,356 `202`→`206`, 1,311 `242`→`256`, 1,235 `242`→`a42`, 762 `242`→`246`, 443 `203`→`207`, the rest smaller. Same 7,700 cases on every CPU in the group, 8573C included. |
| `movbe.m64_g.2` (`66 REX.W 0F 38 F1`), page-split faulting store: 81 of 400 cases fault, 36 differ | memory digest `bdc5dc0dfb84965d` at case 8 | `4e6b899c9c0912ad` | measured. Same fault and page. Xeon 6975P matches AMD, so generation, not vendor (inferred). Repro bytes `66 4c 0f 38 f1 0c 25 f9 2f 00 30`; not otherwise captured. |
| `rsqrtps` / `vrsqrtps` (`0f 52 ce`), seed 3, xmm1 | `7f8000004f34f800` | `7f8000004f350000` | measured, Ivy Bridge included |
| `rcpps` / `vrcpps` (`0f 53 ce`), seed 4, xmm1 | `70ce600091820000` | `70ce580091820800` | measured, Ivy Bridge included |
| `rsqrtss` (`f3 0f 52 ce`), seed 3, low dword | `4f34f800` | `4f350000` | measured |
| `rcpss` (`f3 0f 53 ce`), seed 4, xmm1 | `2ceb16e091820000` | `2ceb16e091820800` | measured |
| `dpps` NaN sign (`66 0f 3a 40 ce ff`), seed 2, xmm1 | `ffc000007fc00000`; Ivy Bridge `7fc000007fc00000` | `ffc00000ffc00000` | measured |
| `f2xm1` (`d9 f0`), seed 2 status word | `fsw=20` | `fsw=220` | measured, Ivy Bridge included |
| `66 e9`, `66 e8` (`66`-prefixed near jmp, call) | run | `SIGSEGV` (`SEGV_MAPERR`) on all 5 seeds | measured: `66 e9` with `probe -v`; `66 e8` in the batch probe (all five seeds fault on AMD, none on Intel) |
| `0f 0d ce` (prefetchw with a register operand) | runs, no effect | `SIGILL` on all 5 seeds | measured. The memory forms `0f 0d 0b` and `0f 0d 1b` run on both. |
| `f3 41 90` (`pause` with `REX.B`), seed 2 | no-op: `rax=123456789abcdef0`, `r8=1a3c` unchanged | executes as `xchg r8d, eax`: `rax=1a3c`, `r8=9abcdef0` | measured; seeds 1 and 4 agree (`rax`/`r8` swap, upper halves cleared on AMD) |

## Between Intel parts

| what | Haswell-class guest, Ivy Bridge | 8370C, 8573C, 6975P (and AMD) |
| --- | --- | --- |
| `smsw r14d` (`41 0f 01 e6`), seed 1, `r14` starts as all ones | `80050033`, upper half cleared | `ffffffff80050033`, upper half kept |

AMD matches the second column. Cause unverified; no helper depends on it.

## `repne cmpsq` (`f2 48 a7`)

Not a vendor or generation difference. Run on its own it behaves the same everywhere: seed 3
(`rcx=0x20000040`, a scan that runs off the seeded memory) takes `SIGSEGV` (`SEGV_MAPERR`). Measured on
every CPU in the table, both with `probe -v` and as a single batch row (digest `c2157fbbccbd30da`,
fault mask `8`); Ivy Bridge, EPYC 7763, 9V74, 9V45 and Xeon 8370C, 8573C were rerun for this.

The earlier survey saw it fault only on some CPUs because it came from a full sweep of all rows
through one `probe` process. In that sweep the row gives mask `0` on Xeon 8573C, 6975P and EPYC 9V45
(three sweeps each on 8573C and 9V45, identical every time) and mask `8` on the Haswell-class guest,
Ivy Bridge, EPYC 7763 and 9V74. So the sweep result is order-dependent: the scan stops at whatever the
rows before it left in memory, and that differs by CPU. Treat the sweep row as noise; the instruction is
the same on both vendors. The state left behind was not dumped, so that mechanism is inferred.
v3compat still refuses the form.

## Decision

- Helpers do not change: every value a helper depends on matches.
- If undefined flags ever matter, follow Intel, since the target is Intel.
