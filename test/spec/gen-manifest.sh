#!/bin/sh
# platform: host-agnostic
# usage: gen-manifest.sh BUILD_DIR -- regenerate manifest.tsv from XED (configure with -DAVXEMU_TEST_XED=ON)
set -eu
b=${1:?build dir}; d=$(cd "$(dirname "$0")" && pwd)
"$b/test/spec-check" "$d/manifest.tsv" "$d/../known-failures.txt" --propose > "$b/probe.tsv"
"$b/test/xedgen" | python3 "$d/make_manifest.py" "$d/manifest.tsv" "$b/probe.tsv" "$d/refused.tsv" "$d/../known-failures.txt"
