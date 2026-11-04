#!/bin/sh
# platform: host-agnostic -- Linux binutils (nm, objdump), cc for --selftest
#   usage: check-helpers-lib.sh LIB.a      fail unless the library is plain AVX1 code that needs
#                                          nothing from libc but memcpy/memset/memmove
#          check-helpers-lib.sh --selftest prove the checks fire on deliberately bad objects
#          The CI CPU runs AVX2/BMI/FMA happily and the Ivy Bridge target does not, so only this
#          guard stops such an instruction from entering libavxemu_helpers.a.
set -eu
NM=${NM:-nm}
OBJDUMP=${OBJDUMP:-objdump}

scan() {
  # platform: nm -u on an archive lists symbols another member defines, so subtract those
  defined=$("$NM" --defined-only "$1" | awk 'NF == 3 { print $3 }')
  "$NM" -u "$1" | awk '$1 == "U" { print $2 }' | sort -u | while read -r sym; do
    case "$sym" in memcpy|memset|memmove) continue ;; esac
    printf '%s\n' "$defined" | grep -qx -- "$sym" || echo "undefined symbol: $sym"
  done
  "$OBJDUMP" -d --no-show-raw-insn "$1" | awk '
    / file format / { member = $1 }
    $1 !~ /^[0-9a-f]+:$/ { next }
    {
      m = $2; ops = ""
      for (i = 3; i <= NF; i++) ops = ops (i > 3 ? " " : "") $i
      bad = ""
      if (m ~ /^\(bad\)$/) bad = "undecodable"
      else if ((m == "rep" || m == "repz") && $3 ~ /^(bsf|bsr)/) bad = "rep bsf/bsr (tzcnt/lzcnt)"
      else if (m ~ /^(lzcnt|tzcnt|movbe|andn|bextr|blsi|blsr|blsmsk|bzhi|mulx|pdep|pext|rorx|sarx|shlx|shrx|adcx|adox|rdseed)[bwlq]?$/) bad = "BMI/LZCNT/MOVBE/ADX"
      else if (ops ~ /%zmm|%k[0-7]/) bad = "AVX-512"
      else if (m ~ /^v?(ld|st)mxcsr$/) bad = "live MXCSR (contract: passed by value)"
      else if (m ~ /^v?(add|sub|mul|div|sqrt|min|max|rcp|rsqrt|round|dp|hadd|hsub|addsub|cvt|ucomi|comi|cmp)[a-z0-9]*(ss|sd|ps|pd|si|dq)2?[a-z0-9]*$/ && m !~ /^v?cmp[bwlq]$/) bad = "FP arithmetic (reads live MXCSR, can raise #XM)"
      else if (m ~ /^vfn?m/) bad = "FMA"
      else if (m ~ /^vcvt(ph2ps|ps2ph)$/) bad = "F16C"
      else if (m ~ /^vp?gather/) bad = "gather"
      else if (m ~ /^vpbroadcast/) bad = "AVX2 broadcast"
      else if (m ~ /^(vbroadcasti128|vinserti128|vextracti128|vperm2i128|vpblendd)$/) bad = "AVX2 128-bit lane op"
      else if (m ~ /^vperm(d|q|ps|pd)$/) bad = "AVX2 permute"
      else if (m ~ /^vpmaskmov/) bad = "AVX2 masked move"
      else if (m ~ /^vp(sllv|srlv|srav)/) bad = "AVX2 variable shift"
      else if (m ~ /^vbroadcast(ss|sd)$/ && ops ~ /^%[xy]mm/) bad = "register-source broadcast (AVX2)"
      else if ((m == "vmovntdqa" || m == "vmpsadbw") && ops ~ /%ymm/) bad = "AVX2 on ymm"
      else if (m ~ /^vp/ && ops ~ /%ymm/ && m !~ /^(vptest|vpermil|vperm2f128)/) bad = "AVX2 integer op on ymm"
      if (bad != "") print member ": " $0 "   <- " bad
    }'
}

selftest() {
  cc=${CC:-cc}
  dir=$(mktemp -d "${TMPDIR:-/tmp}/helpers-lib.XXXXXX")
  trap 'rm -rf "$dir"' EXIT INT TERM
  n=0
  while IFS= read -r insn; do
    [ -n "$insn" ] || continue
    n=$((n + 1))
    printf '.text\nf:\n\t%s\n\tret\n' "$insn" > "$dir/bad$n.S"
    "$cc" -c "$dir/bad$n.S" -o "$dir/bad$n.o"
    if [ -z "$(scan "$dir/bad$n.o")" ]; then
      echo "selftest: not caught: $insn" >&2
      exit 1
    fi
  done <<'EOF'
lzcnt %rax, %rax
tzcnt %rax, %rax
rep bsf %rax, %rax
movbe (%rax), %rax
andn %rax, %rbx, %rcx
bextr %rax, %rbx, %rcx
blsi %rax, %rbx
blsr %rax, %rbx
blsmsk %rax, %rbx
bzhi %rax, %rbx, %rcx
mulx %rax, %rbx, %rcx
pdep %rax, %rbx, %rcx
pext %rax, %rbx, %rcx
rorx $3, %rax, %rbx
sarx %rax, %rbx, %rcx
shlx %rax, %rbx, %rcx
shrx %rax, %rbx, %rcx
vfmadd213ps %xmm0, %xmm1, %xmm2
vfnmadd231sd %xmm0, %xmm1, %xmm2
vcvtph2ps %xmm0, %xmm1
vcvtps2ph $0, %xmm0, %xmm1
vgatherdps %xmm0, (%rax,%xmm1,4), %xmm2
vpgatherdd %xmm0, (%rax,%xmm1,4), %xmm2
vpbroadcastb %xmm0, %xmm1
vbroadcasti128 (%rax), %ymm0
vinserti128 $1, %xmm0, %ymm1, %ymm2
vextracti128 $1, %ymm0, %xmm1
vperm2i128 $1, %ymm0, %ymm1, %ymm2
vpermd %ymm0, %ymm1, %ymm2
vpermq $1, %ymm0, %ymm1
vpermps %ymm0, %ymm1, %ymm2
vpermpd $1, %ymm0, %ymm1
vpmaskmovd (%rax), %xmm0, %xmm1
vpsllvd %xmm0, %xmm1, %xmm2
vpsrlvq %xmm0, %xmm1, %xmm2
vpsravd %xmm0, %xmm1, %xmm2
vpblendd $1, %xmm0, %xmm1, %xmm2
vbroadcastss %xmm0, %ymm1
vbroadcastsd %xmm0, %ymm1
vpaddb %ymm0, %ymm1, %ymm2
vpcmpeqb %ymm0, %ymm1, %ymm2
vmpsadbw $1, %ymm0, %ymm1, %ymm2
vmovntdqa (%rax), %ymm0
vpaddd %zmm0, %zmm1, %zmm2
ldmxcsr (%rax)
vstmxcsr (%rax)
addss %xmm0, %xmm1
vmulpd %ymm0, %ymm1, %ymm2
vdivsd %xmm0, %xmm1, %xmm2
vcvtsi2sd %rax, %xmm0, %xmm1
vcvttss2si %xmm0, %rax
vucomisd %xmm0, %xmm1
vcmpltps %ymm0, %ymm1, %ymm2
call __udivti3
EOF
  cat > "$dir/ok.S" <<'EOF'
.text
f:
	vpaddb %xmm0, %xmm1, %xmm2
	vptest %ymm0, %ymm1
	vpermilps $1, %ymm0, %ymm1
	vperm2f128 $1, %ymm0, %ymm1, %ymm2
	vbroadcastss (%rax), %ymm0
	vmovdqu %ymm0, (%rax)
	vandnps %ymm0, %ymm1, %ymm2
	vmovss (%rax), %xmm0
	vmovsd %xmm0, (%rax)
	vmovaps %ymm0, %ymm1
	cmpb $0, (%rax)
	cmpq %rax, %rbx
	vshufps $1, %ymm0, %ymm1, %ymm2
	vzeroupper
	bsf %rax, %rbx
	bsr %rax, %rbx
	call memcpy
	ret
EOF
  "$cc" -c "$dir/ok.S" -o "$dir/ok.o"
  out=$(scan "$dir/ok.o")
  [ -z "$out" ] || { printf 'selftest: clean object rejected:\n%s\n' "$out" >&2; exit 1; }
  echo "check-helpers-lib: selftest ok ($n bad objects caught)"
}

case "${1:-}" in
  --selftest) selftest ;;
  '') echo "usage: check-helpers-lib.sh LIB.a | --selftest" >&2; exit 2 ;;
  *)
    [ -f "$1" ] || { echo "check-helpers-lib: no such file: $1" >&2; exit 2; }
    out=$(scan "$1")
    if [ -n "$out" ]; then
      printf '%s\n' "$out" >&2
      echo "check-helpers-lib: $1 is not plain AVX1 / libc-free" >&2
      exit 1
    fi
    echo "check-helpers-lib: $1 ok"
    ;;
esac
