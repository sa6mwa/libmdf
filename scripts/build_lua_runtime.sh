#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
libdir=${1:-"$root/build/luarocks/libmdf-prefix/lib"}
build=${2:-"$root/build/lua-runtime"}
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || {
  printf 'Local Lua verification requires native x86_64 Linux\n' >&2
  exit 1
}
cmake -S "$root/cmake/lua-runtime" -B "$build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchains/x86_64-linux-gnu.cmake" \
  -DLIBMDF_LUA_MDF_LIBDIR="$libdir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build"
eval "$("$root/scripts/bootlin_x86_runtime.sh")"
cmake -DREADELF="$LIBMDF_BOOTLIN_READELF" -DEXECUTABLE="$build/bin/lua" \
  -DINTERPRETER="$LIBMDF_BOOTLIN_INTERPRETER" -DRUNTIME_DIR="$LIBMDF_BOOTLIN_RUNTIME_DIR" \
  -P "$root/tests/assert_bootlin_runtime.cmake"
"$build/bin/lua" -e 'assert(_VERSION == "Lua 5.5")'
