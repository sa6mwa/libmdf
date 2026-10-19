#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
build="$root/build/lua-release"
dist=${LIBMDF_DIST_DIR:-$root/dist}
[[ ! -L $root/build && ! -L $build && ! -L $dist ]] || { printf 'Lua packaging must use owned directories\n' >&2; exit 1; }
cmake -S "$root/cmake/lua-package" -B "$build" -G Ninja -DLIBMDF_DIST_DIR="$dist"
cmake --build "$build" --target package
