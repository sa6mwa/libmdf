#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
lua="$root/build/lua-runtime/bin/lua"
eval "$("$root/scripts/bootlin_x86_runtime.sh")"
cmake -DREADELF="$LIBMDF_BOOTLIN_READELF" -DEXECUTABLE="$lua" \
  -DINTERPRETER="$LIBMDF_BOOTLIN_INTERPRETER" -DRUNTIME_DIR="$LIBMDF_BOOTLIN_RUNTIME_DIR" \
  -P "$root/tests/assert_bootlin_runtime.cmake"
LUA_PATH="$root/build/luarocks/tree/share/lua/5.5/?.lua;;" \
LUA_CPATH="$root/build/luarocks/tree/lib/lua/5.5/?.so;;" \
"$lua" - "$(cd "$LIBMDF_BOOTLIN_RUNTIME_DIR" && pwd -P)" \
  "$root/build/luarocks/libmdf-prefix/lib/" <<'LUA'
assert(os.getenv("LD_LIBRARY_PATH") == nil, "test requires no LD_LIBRARY_PATH")
local mdf = require("libmdf")
assert(mdf.render("# runtime\n", { boring = true }):match("runtime"))
local maps = assert(io.open("/proc/self/maps")):read("*a")
local libc = assert(maps:match("(/[^\n ]*/libc%.so%.6)"))
assert(libc:find(arg[1], 1, true), libc)
local libm = assert(maps:match("(/[^\n ]*/libm%.so%.6)"))
assert(libm:find(arg[1], 1, true), libm)
local loader = assert(maps:match("(/[^\n ]*/ld%-linux%-x86%-64%.so%.2)"))
assert(loader:find(arg[1], 1, true), loader)
local library = assert(maps:match("(/[^\n ]*/libmdf%.so[^\n ]*)"))
assert(library:find(arg[2], 1, true), library)
-- Host children must retain their own runtime and ordinary command behavior.
local child = assert(io.popen("/bin/sh -c 'printf host-child'"))
assert(child:read("*a") == "host-child")
assert(child:close())
LUA
