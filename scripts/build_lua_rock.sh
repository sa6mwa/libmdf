#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
"$root/scripts/build.sh" debug
version=$("$root/scripts/version.sh")
runtime_env=$("$root/scripts/bootlin_x86_runtime.sh")
eval "$runtime_env"
work="$root/build/luarocks"
mkdir -p "$work/source/lua"
mkdir -p "$work/scratch"
export TMPDIR="$work/scratch"
cp "$root/lua/libmdf.lua" "$root/lua/libmdf_core.c" "$work/source/lua/"
"$root/scripts/render_release_rockspec.sh" "$work/source/libmdf-$version-1.rockspec" "$version"
cd "$work/source"
luarocks --lua-dir "$root/build/debug/lua-runtime" --lua-version 5.5 --tree "$work/tree" \
  make "libmdf-$version-1.rockspec" MDF_DIR="$root/build/debug/lua-sdk" \
  CC="$LIBMDF_BOOTLIN_CC" LD="$LIBMDF_BOOTLIN_CC" CFLAGS='-O2 -fPIC -Wall -Wextra -Werror' \
  LUA_INCDIR="$root/build/debug/lua-runtime/include"
