#!/bin/sh
# Regenerate manifest.tsv from XED (build with -DAVXEMU_TEST_XED=ON), probing decode.c for the
# status of new forms: gen-manifest.sh BUILD_DIR
set -eu
b=${1:?build dir}; d=$(cd "$(dirname "$0")" && pwd)
"$b/test/spec-check" "$d/manifest.tsv" "$d/../known-failures.txt" --propose > "$b/probe.tsv"
"$b/xedgen" | python3 "$d/make_manifest.py" "$d/manifest.tsv" "$b/probe.tsv" "$d/refused.tsv"
