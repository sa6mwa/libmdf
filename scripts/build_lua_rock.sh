#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sh "$ROOT/scripts/version.sh")
PREFIX="$ROOT/build/luarocks/libmdf-prefix"
bash "$ROOT/scripts/build_lua_runtime.sh"
LUA_ROOT="$ROOT/build/lua-runtime"
"$ROOT/scripts/configure_preset.sh" debug lua -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build --preset debug
cmake --install "$ROOT/build/debug"
mkdir -p "$ROOT/build/luarocks"
sed "s/@VERSION@/$VERSION/g" "$ROOT/libmdf-scm-1.rockspec.in" > "$ROOT/build/luarocks/libmdf-$VERSION-1.rockspec"
eval "$("$ROOT/scripts/bootlin_x86_runtime.sh")"
CC="$LIBMDF_BOOTLIN_CC"
luarocks --lua-dir "$LUA_ROOT" --lua-version 5.5 --tree "$ROOT/build/luarocks/tree" \
  make "$ROOT/build/luarocks/libmdf-$VERSION-1.rockspec" MDF_DIR="$PREFIX" \
  CC="$CC" LD="$CC" CFLAGS="-O2 -fPIC -Wall -Wextra -Werror" \
  LUA_INCDIR="$LUA_ROOT/include"
