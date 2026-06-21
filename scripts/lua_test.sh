#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
"$ROOT/scripts/build_lua_rock.sh"
"$ROOT/scripts/generate_cmdf_lua.sh" "$ROOT/build/luarocks/cmdf.lua"
PREFIX="$ROOT/build/luarocks/libmdf-prefix"
TREE="$ROOT/build/luarocks/tree"
LIBMDF_CMDF="$ROOT/build/debug/cmdf" \
LIBMDF_CMDF_LUA="$ROOT/build/luarocks/cmdf.lua" \
LUA_PATH="$TREE/share/lua/5.5/?.lua;$TREE/share/lua/5.5/?/init.lua;;" \
LUA_CPATH="$TREE/lib/lua/5.5/?.so;;" \
LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
lua "$ROOT/tests/lua_smoke.lua"
