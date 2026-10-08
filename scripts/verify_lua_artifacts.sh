#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
version=$("$root/scripts/version.sh")
dist=${LIBMDF_DIST_DIR:-$root/dist}
work="$root/build/package-verify/lua"
archive="$dist/libmdf-lua-$version.tar.gz"
rockspec="$dist/libmdf-$version-1.rockspec"
srcrock="$dist/libmdf-$version-1.src.rock"
sdk_archive="$dist/libmdf-$version-x86_64-linux-gnu.tar.gz"
lua_root="$root/build/debug/lua-runtime"
test -x "$lua_root/bin/lua" || { printf 'Prepare the native Lua interpreter with make build-debug\n' >&2; exit 1; }
for file in "$archive" "$rockspec" "$srcrock" "$sdk_archive"; do test -f "$file"; done
rm -rf -- "$work"
mkdir -p "$work/archive" "$work/srcrock" "$work/sdk" "$work/tree"
tar -C "$work/archive" -xzf "$archive"
source="$work/archive/libmdf-lua-$version"
test "$(cat "$source/VERSION")" = "$version"
for required in LICENSE README.md VERSION RELEASE_MANIFEST libmdf-scm-1.rockspec.in lua/libmdf.lua lua/libmdf_core.c; do test -f "$source/$required"; done
(cd "$source" && find . -type f ! -name RELEASE_MANIFEST | sed 's#^./##' | sort) > "$work/manifest.actual"
sort "$source/RELEASE_MANIFEST" > "$work/manifest.expected"
cmp "$work/manifest.actual" "$work/manifest.expected"
unzip -q -d "$work/srcrock" "$srcrock"
[[ $(find "$work/srcrock" -type f | wc -l) == 2 ]]
cmp "$rockspec" "$work/srcrock/libmdf-$version-1.rockspec"
cmp "$archive" "$work/srcrock/libmdf-lua-$version.tar.gz"
grep -Fq "https://github.com/sa6mwa/libmdf/releases/download/v$version/libmdf-lua-$version.tar.gz" "$rockspec"
grep -Fq 'dependencies = { "lua >= 5.5, < 5.6" }' "$rockspec"
if grep -Fq 'file://' "$rockspec"; then printf 'Lua source acquisition URL must be public\n' >&2; exit 1; fi
if grep -R -a -l -e "$root" -e /home/ -e /tmp/ "$work/archive" "$work/srcrock"; then printf 'Local paths in Lua release payload\n' >&2; exit 1; fi
tar -C "$work/sdk" -xzf "$sdk_archive"
sdk="$work/sdk/libmdf-$version-x86_64-linux-gnu"
runtime_env=$("$root/scripts/bootlin_x86_runtime.sh")
eval "$runtime_env"
(cd "$dist" && luarocks --lua-dir "$lua_root" --lua-version 5.5 --tree "$work/tree" install "$srcrock" \
  MDF_DIR="$sdk" CC="$LIBMDF_BOOTLIN_CC" LD="$LIBMDF_BOOTLIN_CC" CFLAGS='-O2 -fPIC -Wall -Wextra -Werror' \
  LDFLAGS="-Wl,--disable-new-dtags,-rpath,$sdk/lib" LUA_INCDIR="$lua_root/include")
LUA_PATH="$work/tree/share/lua/5.5/?.lua;;" LUA_CPATH="$work/tree/lib/lua/5.5/?.so;;" \
  "$lua_root/bin/lua" - "$sdk/lib/" "$version" <<'LUA'
local mdf = require("libmdf")
assert(mdf.version == arg[2])
assert(mdf.render("# LuaRock\n", { boring = true }):match("LuaRock"))
local file = assert(io.open("/proc/self/maps"))
local maps = file:read("*a"); file:close()
local library = assert(maps:match("(/[^\n ]*/libmdf%.so[^\n ]*)"))
assert(library:find(arg[1], 1, true), "source rock loaded a different SDK: " .. library)
LUA
