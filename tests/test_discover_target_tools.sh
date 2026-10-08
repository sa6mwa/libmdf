#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test tool discovery"
rm -rf -- "$work"
mkdir -p "$work/root/build/preset" "$work/bin" "$work/path"
helper="$root/scripts/discover_target_tools.sh"
tool() { printf '#!/bin/sh\nexit 0\n' > "$1"; chmod +x "$1"; }
tool "$work/bin/arm64-apple-darwin25-clang"
tool "$work/bin/arm64-apple-darwin25-strip"
tool "$work/bin/explicit-strip"
tool "$work/path/readelf"
cat > "$work/root/build/preset/CMakeCache.txt" <<CACHE
LIBMDF_TARGET_ID:STRING=arm64-apple-darwin
CMAKE_C_COMPILER:FILEPATH=$work/bin/arm64-apple-darwin25-clang
CMAKE_STRIP:FILEPATH=$work/bin/explicit-strip
CACHE
[[ $("$helper" --root "$work/root" --preset preset --target arm64-apple-darwin strip) == "$work/bin/explicit-strip" ]]
[[ $(LIBMDF_STRIP="$work/bin/arm64-apple-darwin25-strip" "$helper" --root "$work/root" --preset preset --target arm64-apple-darwin strip) == "$work/bin/arm64-apple-darwin25-strip" ]]
sed -i '/CMAKE_STRIP:/d' "$work/root/build/preset/CMakeCache.txt"
[[ $("$helper" --root "$work/root" --preset preset --target arm64-apple-darwin strip) == "$work/bin/arm64-apple-darwin25-strip" ]]
if "$helper" --root "$work/root" --preset preset --target x86_64-linux-gnu strip > "$work/mismatch.log" 2>&1; then exit 1; fi
grep -q 'does not match selected target' "$work/mismatch.log"
if CPKT_TOOLCHAIN_CACHE="$work/missing" PATH="$work/path:$PATH" "$helper" --target x86_64-linux-gnu readelf > "$work/missing.log" 2>&1; then printf 'Host binutils fallback was accepted\n' >&2; exit 1; fi
[[ $("$helper" --target x86_64-linux-gnu readelf) == $("$root/scripts/cpkt-toolchains.sh" discover x86_64-linux-gnu | sed -n 's/^readelf=//p') ]]
