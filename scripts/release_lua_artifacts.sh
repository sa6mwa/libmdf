#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
version=$("$root/scripts/version.sh")
build="$root/build/lua-release"
dist=${LIBMDF_DIST_DIR:-$root/dist}
name="libmdf-lua-$version"
mkdir -p "$dist" "$build"
"$root/scripts/stage_lua_rock_sources.sh" "$version" "$build/stage"
"$root/scripts/archive.sh" "$build/stage" "$name" "$dist/$name.tar.gz"
"$root/scripts/render_release_rockspec.sh" "$dist/libmdf-$version-1.rockspec" "$version"
# zip -X removes host extras; normalize all member timestamps and permissions.
mkdir -p "$build/srcrock"
cp "$dist/libmdf-$version-1.rockspec" "$dist/$name.tar.gz" "$build/srcrock/"
chmod 644 "$build/srcrock/"*
TZ=UTC touch -t 198001010000 "$build/srcrock/"*
rm -f "$dist/libmdf-$version-1.src.rock"
(cd "$build/srcrock" && TZ=UTC zip -Xq "$dist/libmdf-$version-1.src.rock" "libmdf-$version-1.rockspec" "$name.tar.gz")
