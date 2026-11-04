# Intel and AMD differences

What `test/reference/semantics.intel.ref` shows against `semantics.ref` (AMD). "Measured" is a
value in the raw dumps; "inferred" is a conclusion drawn from them.

## The recording

- 494 forms, 212,320 cases, same inputs as the AMD recording (0 inputs differ).
- A KVM guest reporting `GenuineIntel`, family 15, model 107. Bare metal was not tested.
- Haswell-class is inferred: AVX2, FMA and BMI run, ADX, SHA, CLFLUSHOPT and RTM fault with `#UD`.
- Controls reproduced `semantics.ref` byte for byte on EPYC 7763, 9V74, 9V45 and Xeon 6975P, so the
  harness itself is not the source of any difference below.

## Same on both (measured)

NaN priority, FMA and F16C values, MXCSR, integer results, in every form. Only the cases below differ.

## Differs

| what | Intel | AMD | status |
| --- | --- | --- | --- |
| Undefined flags of 40 BMI/LZCNT/TZCNT forms, 7,700 cases (e.g. `andn` raw rflags, `lzcnt`) | `202`, `202` | `206`, `a02` | measured; digests mask them. Xeon 6975P matches the node. |
| `movbe.m64_g.2` (`66 REX.W 0F 38 F1`), page-split faulting store: 81 of 400 cases fault, 36 differ | memory digest `bdc5dc0dfb84965d` at case 8 | `4e6b899c9c0912ad` | measured. Same fault and page. Xeon 6975P matches AMD, so generation, not vendor (inferred). Repro bytes `66 4c 0f 38 f1 0c 25 f9 2f 00 30`; not otherwise captured. |
| `rcpps`, `rsqrtps`, SS and VEX forms | `rsqrtps` `7f8000004f34f800` | `7f8000004f350000` | measured. Results group with Ivy Bridge, Xeon 8573C, Xeon 6975P against AMD. |
| `dpps` NaN sign | node, Xeon 6975P `ffc000007fc00000`; Ivy Bridge `7fc000007fc00000` | 9V45 `ffc00000ffc00000` | measured |
| `f2xm1` status word | `fsw=20` | `fsw=220` | measured |
| `66`-prefixed near branches, `0f 0d` (prefetchw), `f3 41 90` | differ | | measured on 9V45: `66 e9` and `0f 0d ce` fault there, not on the node. Details of `f3 41 90` not re-checked. |
| `repne cmpsq` | faults on one seed | | see below |

`repne cmpsq` is reported to fault on one seed on the node, Ivy Bridge, EPYC 7763 and 9V74 but not on
Xeon 8573C, 6975P or EPYC 9V45, which would make it generation, not vendor. The probe run kept with the
dumps shows seed 3 faulting on the node, 6975P and 9V45 alike, so this is **unconfirmed**. Cause
unverified. v3compat refuses the form.

## Decision

- Helpers do not change: every value a helper depends on matches.
- If undefined flags ever matter, follow Intel, since the target is Intel.
