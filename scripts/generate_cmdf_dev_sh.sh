#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${1:-"$ROOT/build/cmdf.sh"}
LUA_BIN=$(command -v lua)
TREE="$ROOT/build/luarocks/tree"
PREFIX="$ROOT/build/luarocks/libmdf-prefix"
DEBUG_LIB="$ROOT/build/debug"
CMDF_LUA="$ROOT/build/luarocks/cmdf.lua"

quote_sh() {
  printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

mkdir -p "$(dirname -- "$OUT")"
{
  printf '%s\n' '#!/usr/bin/env sh'
  printf '%s\n' 'set -eu'
  printf 'export LUA_PATH='
  quote_sh "$TREE/share/lua/5.5/?.lua;$TREE/share/lua/5.5/?/init.lua;;"
  printf '\n'
  printf 'export LUA_CPATH='
  quote_sh "$TREE/lib/lua/5.5/?.so;;"
  printf '\n'
  printf 'export LD_LIBRARY_PATH='
  quote_sh "$DEBUG_LIB:$PREFIX/lib"
  printf '%s\n' '${LD_LIBRARY_PATH:+":$LD_LIBRARY_PATH"}'
  printf 'exec '
  quote_sh "$LUA_BIN"
  printf ' '
  quote_sh "$CMDF_LUA"
  printf '%s\n' ' "$@"'
} > "$OUT"
chmod +x "$OUT"
