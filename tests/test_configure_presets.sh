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

# The normal editor database must also work from a cold Debug tree with the
# optional Lua facade disabled. Keep that configuration isolated from this tree.
source="$work/editor-source"
rm -rf -- "$source"
mkdir -p "$source/parityjudge"
cp "$root/"{CMakeLists.txt,CMakePresets.json,LICENSE,README.md,libmdf-scm-1.rockspec.in,.clangd} "$source/"
cp -R "$root/"{assets,benchmarks,cmake,examples,fuzz,include,scripts,src,tests} "$source/"
cp "$root/parityjudge/"{*.go,*.[ch],go.mod,go.sum} "$source/parityjudge/"
cmake --preset debug -S "$source" -B "$source/build/debug" -DLIBMDF_BUILD_LUA_TESTS=OFF > "$work/editor-native.log" 2>&1
ctest --test-dir "$source/build/debug" -R '^editor_clangd$' --output-on-failure --no-tests=error \
  >> "$work/editor-native.log" 2>&1 || { cat "$work/editor-native.log" >&2; exit 1; }
test ! -d "$source/build/debug/lua-runtime"
# Enabling Lua must add its real C99 implementation to the registered editor check.
cp -R "$root/lua" "$source/"
cmake --preset debug -S "$source" -B "$source/build/debug" -DLIBMDF_BUILD_LUA_TESTS=ON > "$work/editor-lua.log" 2>&1
ctest --test-dir "$source/build/debug" -N -V -R '^editor_clangd$' >> "$work/editor-lua.log" 2>&1
grep -Fq 'lua/libmdf_core.c' "$work/editor-lua.log"
