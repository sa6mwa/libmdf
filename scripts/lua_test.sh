#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
"$ROOT/scripts/build_lua_rock.sh"
if [ -e "$ROOT/libmdf" ]; then
  printf '%s\n' "Lua rock build leaked a source-tree module directory: $ROOT/libmdf" >&2
  exit 1
fi
env -u LD_LIBRARY_PATH bash "$ROOT/tests/test_lua_runtime.sh"
"$ROOT/scripts/generate_cmdf_lua.sh" "$ROOT/build/luarocks/cmdf.lua"
"$ROOT/scripts/generate_cmdf_dev_sh.sh" "$ROOT/build/cmdf.sh"
TREE="$ROOT/build/luarocks/tree"
LIBMDF_CMDF="$ROOT/build/debug/cmdf" \
LIBMDF_CMDF_LUA="$ROOT/build/luarocks/cmdf.lua" \
PATH="$ROOT/build/lua-runtime/bin:$PATH" \
LUA_PATH="$TREE/share/lua/5.5/?.lua;$TREE/share/lua/5.5/?/init.lua;;" \
LUA_CPATH="$TREE/lib/lua/5.5/?.so;;" \
"$ROOT/build/lua-runtime/bin/lua" "$ROOT/tests/lua_smoke.lua"

sample='# Lua

body with **strong** and [link](https://example.com)
'
cmdf_out=$(printf '%s' "$sample" | "$ROOT/build/debug/cmdf" -b)
cmdf_sh_out=$(printf '%s' "$sample" | "$ROOT/build/cmdf.sh" -b)
if [ "$cmdf_sh_out" != "$cmdf_out" ]; then
  printf '%s\n' 'build/cmdf.sh output mismatch' >&2
  exit 1
fi
