#!/bin/sh
# Exercise the production copy/install boundary without building the renderer.
# Usage: run.sh NATIVE_ASSET_COMPILER
set -eu
asset=${1:?usage: run.sh NATIVE_ASSET_COMPILER}
root=$(cd "$(dirname "$0")/../../.." && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/studio-native-asset.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/sdk/share/chibi" "$work/sdk/libexec/chibi"
status=0
cmake -S "$root/tests/packaging/asset_compiler" -B "$work/missing" \
    -DChibi_RESOURCE_DIR="$work/sdk/share/chibi" >"$work/missing.log" 2>&1 || status=$?
[ "$status" != 0 ]
grep -F 'The installed SDK lacks the native asset compiler' "$work/missing.log" >/dev/null
[ ! -e "$work/missing/bin/chibi-asset-compiler" ]
cp "$asset" "$work/sdk/libexec/chibi/chibi-asset-compiler"
cmake -S "$root/tests/packaging/asset_compiler" -B "$work/build" \
    -DChibi_RESOURCE_DIR="$work/sdk/share/chibi"
cmp "$asset" "$work/build/bin/chibi-asset-compiler"
cmake --install "$work/build" --prefix "$work/studio"
cmp "$asset" "$work/studio/bin/chibi-asset-compiler"
mv "$work/studio" "$work/relocated Studio"
rm -rf "$work/sdk"
status=0
env -i PATH= CC=/usr/bin/false CXX=/usr/bin/false \
    "$work/relocated Studio/bin/chibi-asset-compiler" >"$work/service.log" 2>&1 || status=$?
[ "$status" = 2 ]
grep -F 'usage: chibi-asset-compiler' "$work/service.log" >/dev/null
printf '%s\n' 'Studio native asset compiler copy, install and relocation passed'
