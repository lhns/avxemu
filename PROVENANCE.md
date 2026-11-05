# Provenance

AVXEmu was extracted from
[Wowfunhappy/Mavericks-Porting-Resources](https://github.com/Wowfunhappy/Mavericks-Porting-Resources)
on 2026-09-27, at Wowfunhappy's invitation, keeping the original commits rather than
squashing them; each keeps its author and date. Of the 14 upstream commits that touched
`avxemu/`, Wowfunhappy wrote three: the emulator itself (`Add AVX2 → AVX1 emulator…`,
2026-06-08), an update for a newer Claude Code (2026-06-26) and the documentation tweak that
is upstream `6ead179` (2026-09-08). Amitai Schleier wrote the other eleven (2026-06-30 to
2026-08-14), fixes and features across the decoder, executor, trampolines and signal handling.
Release 1 ships all fourteen. This repo's own build, tests and packaging sit on top as
ordinary commits, by Amitai Schleier with Claude (credited in trailers).

## How

`git filter-branch --prune-empty --subdirectory-filter avxemu` over a throwaway
`git clone --no-local`, run on the maintainer's branch `avxemu-tests-on-avx2-oracle`
(`f86d863`) and on `avxemu-minspill-bmi-tier`, with every other ref, the reflog and
unreachable objects pruned. Verified by tree identity, not by inspection:

- 23 commits, no merges: the 14 that touched `avxemu/` upstream, then 9 unreleased fixes;
- the rewritten `6ead179` ("Documentation tweak", upstream `master` at extraction) has
  exactly upstream's `avxemu/` tree; so does the rewritten `f86d863`.

## Branches and what follows

`main` is upstream `6ead179`, then this repo's own build, tests and packaging. Release
`20260908.1` ships this code unchanged.

Release `20261005.1` carries the maintainer's follow-up work: nine fixes, run on a 10.9
machine since September (load-time fixes, thread-safe live patching, hermetic tests on CPUs
without AVX2, a load-time analysis cache, and fixes found running the suite on an AVX2 Mac).
One more fix was folded into "stay inert on a CPU that already has everything we emulate"
during the replay, so that every commit passes on an AVX2 Mac too: the test programs that
drive the `SIGILL` handler install it on any CPU. The release also carries a speedup for
register-resident BMI. It was held back at first because Claude Code's hot path uses memory
operands, which it declines, not for any known defect, and it is on by default
(`AVXEMU_MINSPILL=0` turns it off).

## Licensing

Settled 2026-09-08 in
[issue #4](https://github.com/Wowfunhappy/Mavericks-Porting-Resources/issues/4):
anything original in that repository is public domain / CC0 / WTFPL. See `LICENSE`.

## The dylib mavericksforever.com serves

Checked 2026-09-27 on Mac OS X 10.9 with `Apple LLVM version 6.0 (clang-600.0.57) (based on LLVM 3.5svn)`.

- `https://mavericksforever.com/claude/libavxemu.dylib` is 82,616 bytes, sha256
  `3998619b2a3de0654629b67a9ae934f1e8c7040d192032bdfa04049118a79407`.
- This repo's CMake build of release 1's source compiles byte-identical objects; linked the
  way the old `build.sh` linked them (no SDK), they reproduce that hash exactly.
- Linked against the pinned 10.9 SDK, as every family build is, the result has the same
  code, symbols, binds, rebases and exports. Only `LC_DYLIB_CODE_SIGN_DRS` differs (24 bytes
  instead of 64), with the link-edit offsets and UUID that follow from it.
- Release `20260908.1` is cross-built in CI with the runner's Xcode clang (shipyard's
  toolchain sets the SDK, not the compiler), so its bytes differ again. Its behaviour is
  checked by the hermetic and replay tests, which pass on both builds, and by a run of the
  shipped dylib on Mac OS X 10.9 before release (the release process is in git history).

## Earlier extraction

An extraction of 2026-08-14 lived in this checkout, local only and never pushed. It stopped
before the linked-load rebind and Wowfunhappy's last commit, and carried none of the
fixes above. This history supersedes it.

## This fork

lhns/avxemu keeps the emulation core and the test suite and drops the preload mechanism and the
macOS port, which stay in git history (everything up to `fc34cc8`). The history above, the
authorship of every commit and the CC0 licence are unchanged.
