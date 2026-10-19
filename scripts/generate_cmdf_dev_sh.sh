#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${1:-"$ROOT/build/cmdf.sh"}
LUA_BIN=${2:-"$ROOT/build/debug/lua-runtime/bin/lua"}
TREE=${3:-"$ROOT/build/debug/lua-tree"}
CMDF_LUA=${4:-"$ROOT/build/debug/cmdf.lua"}

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
  printf 'exec '
  quote_sh "$LUA_BIN"
  printf ' '
  quote_sh "$CMDF_LUA"
  printf '%s\n' ' "$@"'
} > "$OUT"
chmod +x "$OUT"
