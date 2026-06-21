#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sh "$ROOT/scripts/version.sh")
PREFIX="$ROOT/build/luarocks/libmdf-prefix"
LUA_VERSION=$(lua -e 'io.write(_VERSION)' 2>/dev/null || true)
if [ "$LUA_VERSION" != "Lua 5.5" ]; then
  printf '%s\n' "libmdf Lua bindings support Lua 5.5; found ${LUA_VERSION:-no lua runtime}" >&2
  exit 1
fi
"$ROOT/scripts/configure_preset.sh" debug lua -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build --preset debug
cmake --install "$ROOT/build/debug"
mkdir -p "$ROOT/build/luarocks"
sed "s/@VERSION@/$VERSION/g" "$ROOT/libmdf-scm-1.rockspec.in" > "$ROOT/build/luarocks/libmdf-$VERSION-1.rockspec"
luarocks --tree "$ROOT/build/luarocks/tree" make "$ROOT/build/luarocks/libmdf-$VERSION-1.rockspec" MDF_DIR="$PREFIX"
