#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test-configure-presets"
mkdir -p "$work"
cd "$root"
check_cache() {
  local build=$1 key=$2 expected=$3
  grep -Fxq "$key:BOOL=$expected" "$build/CMakeCache.txt" || {
    printf 'Unexpected %s in %s; expected %s\n' "$key" "$build" "$expected" >&2; exit 1;
  }
}
for preset in debug x86_64-linux-gnu-release fuzz; do
  build="$work/$preset"
  cmake --preset "$preset" -B "$build" -DLIBMDF_BUILD_HOST_TESTS=OFF -DLIBMDF_BUILD_LUA_TESTS=OFF > "$work/$preset.log" 2>&1
  case "$preset" in
    debug) check_cache "$build" LIBMDF_BUILD_TESTS ON; check_cache "$build" LIBMDF_CMDF_STATIC_RUNTIME OFF;;
    *-release) check_cache "$build" LIBMDF_BUILD_TESTS ON; check_cache "$build" LIBMDF_CMDF_STATIC_RUNTIME ON; check_cache "$build" LIBMDF_INSTALL_BINARY ON;;
    fuzz) check_cache "$build" LIBMDF_BUILD_FUZZERS ON; check_cache "$build" LIBMDF_BUILD_SHARED OFF;;
  esac
  touch "$build/preserve.marker"
  cmake --preset "$preset" -B "$build" -DLIBMDF_BUILD_HOST_TESTS=OFF -DLIBMDF_BUILD_LUA_TESTS=OFF >> "$work/$preset.log" 2>&1
  test -f "$build/preserve.marker"
done
if cmake --preset nonexistent -B "$work/debug" > "$work/invalid.log" 2>&1; then
  printf 'Unknown preset accepted\n' >&2; exit 1;
fi
test -f "$work/debug/preserve.marker"
