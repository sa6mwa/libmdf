#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sh "$ROOT/scripts/version.sh")
PREFIX="$ROOT/build/luarocks/libmdf-prefix"
mkdir -p "$ROOT/build/luarocks"
WORK=$(mktemp -d "$ROOT/build/luarocks/work.XXXXXX")

cleanup() {
  rm -rf "$WORK"
}
trap cleanup EXIT HUP INT TERM

if [ -e "$ROOT/libmdf" ]; then
  printf '%s\n' "refusing source-tree Lua module directory: $ROOT/libmdf" >&2
  exit 1
fi

mkdir -p "$WORK/lua"
cp "$ROOT/lua/libmdf.lua" "$WORK/lua/libmdf.lua"
cp "$ROOT/lua/libmdf_core.c" "$WORK/lua/libmdf_core.c"
bash "$ROOT/scripts/build_lua_runtime.sh"
LUA_ROOT="$ROOT/build/lua-runtime"
(
  cd "$ROOT"
  "$ROOT/scripts/configure_preset.sh" debug lua -DCMAKE_INSTALL_PREFIX="$PREFIX"
  cmake --build --preset debug
)
cmake --install "$ROOT/build/debug"
ROCKSPEC="$WORK/libmdf-$VERSION-1.rockspec"
sed "s/@VERSION@/$VERSION/g" "$ROOT/libmdf-scm-1.rockspec.in" > "$ROCKSPEC"
eval "$("$ROOT/scripts/bootlin_x86_runtime.sh")"
CC="$LIBMDF_BOOTLIN_CC"
(
  cd "$WORK"
  luarocks --lua-dir "$LUA_ROOT" --lua-version 5.5 --tree "$ROOT/build/luarocks/tree" \
    make "$ROCKSPEC" MDF_DIR="$PREFIX" \
    CC="$CC" LD="$CC" CFLAGS="-O2 -fPIC -Wall -Wextra -Werror" \
    LUA_INCDIR="$LUA_ROOT/include"
)
