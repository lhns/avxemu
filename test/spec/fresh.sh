#!/bin/sh
# usage: fresh.sh XEDGEN PYTHON SPEC_DIR -- regenerate into a scratch copy, require no new/removed/changed form
set -eu
xedgen=$1 py=$2 dir=$3
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
cp "$dir/manifest.tsv" "$t/m.tsv"
"$xedgen" | "$py" "$dir/make_manifest.py" "$t/m.tsv"
cmp "$dir/manifest.tsv" "$t/m.tsv" || { diff "$dir/manifest.tsv" "$t/m.tsv" | head -20; echo "manifest is stale: run test/spec/gen-manifest.sh"; exit 1; }
