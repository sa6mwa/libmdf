#!/usr/bin/env bash
# Exercise real Lua artifacts: warm reuse, input changes, and missing outputs.
set -euo pipefail
unset LIBMDF_RELEASE_VERSION LIBMDF_DIST_DIR
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test-lua-package-reuse"
rm -rf -- "$work"
source="$work/source"
mkdir -p "$source/scripts" "$source/lua" "$source/cmake"
cp "$root/scripts/"{release_lua_artifacts.sh,stage_lua_rock_sources.sh,render_release_rockspec.sh,archive.sh,version.sh} "$source/scripts/"
cp "$root/scripts/package_lua_sources.sh" "$source/scripts/"
cp "$root/scripts/require_build_path.sh" "$source/scripts/"
cp -R "$root/cmake/lua-package" "$source/cmake/"
cp "$root/"{LICENSE,README.md,libmdf-scm-1.rockspec.in} "$source/"
cp "$root/lua/"{libmdf.lua,libmdf_core.c} "$source/lua/"
produce() { bash "$source/scripts/release_lua_artifacts.sh" > "$work/$1.log" 2>&1; }
timestamps() { find "$source/dist" -type f -printf '%f %T@\n' | sort; }
produce first
timestamps > "$work/first.timestamps"
mkdir -p "$work/first"
cp "$source/dist/"* "$work/first/"
sleep 1
produce warm
timestamps > "$work/warm.timestamps"
cmp "$work/first.timestamps" "$work/warm.timestamps"
# An ordinary source edit must replace the actual archive payload.
printf '\n-- Lua package invalidation fixture\n' >> "$source/lua/libmdf.lua"
produce changed
mkdir -p "$work/extract"
tar -C "$work/extract" -xzf "$source/dist/libmdf-lua-0.0.0.tar.gz"
cmp "$source/lua/libmdf.lua" "$work/extract/libmdf-lua-0.0.0/lua/libmdf.lua"
if cmp -s "$work/first/libmdf-lua-0.0.0.tar.gz" "$source/dist/libmdf-lua-0.0.0.tar.gz"; then exit 1; fi
mkdir -p "$work/rock"
unzip -q -d "$work/rock" "$source/dist/libmdf-0.0.0-1.src.rock"
cmp "$source/dist/libmdf-lua-0.0.0.tar.gz" "$work/rock/libmdf-lua-0.0.0.tar.gz"
cmp "$source/dist/libmdf-0.0.0-1.rockspec" "$work/rock/libmdf-0.0.0-1.rockspec"
mkdir -p "$work/changed"
cp "$source/dist/"* "$work/changed/"
for payload in libmdf-lua-0.0.0.tar.gz libmdf-0.0.0-1.rockspec libmdf-0.0.0-1.src.rock; do
  rm "$source/dist/$payload"
  produce missing
  cmp "$work/changed/$payload" "$source/dist/$payload"
done
# Extracted-source version changes select a new complete artifact set.
printf '2.3.4\n' > "$source/VERSION"
touch "$source/RELEASE_MANIFEST"
produce version
tar -C "$work/extract" -xzf "$source/dist/libmdf-lua-2.3.4.tar.gz"
test "$(cat "$work/extract/libmdf-lua-2.3.4/VERSION")" = 2.3.4
grep -Fq 'version = "2.3.4-1"' "$source/dist/libmdf-2.3.4-1.rockspec"
test -f "$source/dist/libmdf-2.3.4-1.src.rock"
test -f "$source/dist/libmdf-0.0.0-1.src.rock"
rm "$source/lua/libmdf_core.c"
if produce missing-input; then printf 'Missing Lua source prerequisite accepted\n' >&2; exit 1; fi
