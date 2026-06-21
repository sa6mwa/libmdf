#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sh "$ROOT/scripts/version.sh")
DIST="$ROOT/dist"
ARCHIVE="$DIST/libmdf-lua-$VERSION.tar.gz"
ROCKSPEC="$DIST/libmdf-$VERSION-1.rockspec"
SRCROCK="$DIST/libmdf-$VERSION-1.src.rock"
TMP="$ROOT/build/package-verify/lua"
HOST_ARTIFACT="$DIST/libmdf-$VERSION-x86_64-linux-gnu.tar.gz"

test -f "$ARCHIVE"
test -f "$ROCKSPEC"
test -f "$SRCROCK"
test -f "$HOST_ARTIFACT"
LUA_VERSION=$(lua -e 'io.write(_VERSION)' 2>/dev/null || true)
if [ "$LUA_VERSION" != "Lua 5.5" ]; then
  printf '%s\n' "libmdf Lua artifact verification supports Lua 5.5; found ${LUA_VERSION:-no lua runtime}" >&2
  exit 1
fi
grep -F -q 'dependencies = { "lua >= 5.5, < 5.6" }' "$ROCKSPEC"

if grep -R -n -e "$ROOT" -e '/home/' -e '/tmp/' "$ROCKSPEC" >/dev/null; then
  printf '%s\n' "$ROCKSPEC contains local paths" >&2
  exit 1
fi

rm -rf "$TMP"
mkdir -p "$TMP/archive" "$TMP/srcrock" "$TMP/sdk" "$TMP/tree"
tar -C "$TMP/archive" -xzf "$ARCHIVE"
root_count=$(find "$TMP/archive" -mindepth 1 -maxdepth 1 -type d | wc -l)
test "$root_count" -eq 1
root=$(find "$TMP/archive" -mindepth 1 -maxdepth 1 -type d)
test "$(basename "$root")" = "libmdf-lua-$VERSION"

for required in \
  LICENSE \
  README.md \
  VERSION \
  RELEASE_MANIFEST \
  libmdf-scm-1.rockspec.in \
  lua/libmdf.lua \
  lua/libmdf_core.c \
  include/libmdf.h \
  include/libmdf/mdf.h \
  scripts/build_lua_rock.sh \
  scripts/render_release_rockspec.sh \
  scripts/stage_lua_rock_sources.sh
do
  test -f "$root/$required"
done
test "$(cat "$root/VERSION")" = "$VERSION"
(
  cd "$root"
  find . -type f ! -name RELEASE_MANIFEST | sed 's#^\./##' | sort > "$TMP/manifest.actual"
  sort RELEASE_MANIFEST > "$TMP/manifest.expected"
)
cmp "$TMP/manifest.actual" "$TMP/manifest.expected"

unzip -q -d "$TMP/srcrock" "$SRCROCK"
test -f "$TMP/srcrock/libmdf-$VERSION-1.rockspec"
test -f "$TMP/srcrock/libmdf-lua-$VERSION.tar.gz"
if grep -R -n -e "$ROOT" -e '/home/' -e '/tmp/' "$TMP/srcrock/libmdf-$VERSION-1.rockspec" >/dev/null; then
  printf '%s\n' "$SRCROCK contains local paths in rockspec" >&2
  exit 1
fi
grep -q "version = \"$VERSION-1\"" "$TMP/srcrock/libmdf-$VERSION-1.rockspec"
grep -q "file://libmdf-lua-$VERSION.tar.gz" "$TMP/srcrock/libmdf-$VERSION-1.rockspec"
grep -F -q 'dependencies = { "lua >= 5.5, < 5.6" }' "$TMP/srcrock/libmdf-$VERSION-1.rockspec"

tar -C "$TMP/sdk" -xzf "$HOST_ARTIFACT"
sdk_root=$(find "$TMP/sdk" -mindepth 1 -maxdepth 1 -type d)
(
  cd "$DIST"
  luarocks --tree "$TMP/tree" install "$SRCROCK" MDF_DIR="$sdk_root"
)
LUA_PATH="$TMP/tree/share/lua/5.5/?.lua;$TMP/tree/share/lua/5.5/?/init.lua;;" \
LUA_CPATH="$TMP/tree/lib/lua/5.5/?.so;;" \
LD_LIBRARY_PATH="$sdk_root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
lua -e 'local mdf = require("libmdf"); local out = mdf.render("# LuaRock\n", { boring = true }); assert(out:match("LuaRock"))'
