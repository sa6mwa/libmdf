#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=${1:-$(sh "$ROOT/scripts/version.sh")}
STAGE_ROOT=${2:-"$ROOT/build/lua-release/stage"}
NAME="libmdf-lua-$VERSION"
DEST="$STAGE_ROOT/$NAME"

rm -rf "$STAGE_ROOT"
mkdir -p "$DEST/lua" "$DEST/include/libmdf" "$DEST/scripts"

cp "$ROOT/LICENSE" "$ROOT/README.md" "$DEST/"
printf '%s\n' "$VERSION" > "$DEST/VERSION"
cp "$ROOT/libmdf-scm-1.rockspec.in" "$DEST/"
cp "$ROOT/lua/libmdf.lua" "$ROOT/lua/libmdf_core.c" "$DEST/lua/"
cp "$ROOT/include/libmdf.h" "$DEST/include/"
cp "$ROOT/include/libmdf/mdf.h" "$DEST/include/libmdf/"
cp "$ROOT/scripts/build_lua_rock.sh" \
   "$ROOT/scripts/render_release_rockspec.sh" \
   "$ROOT/scripts/stage_lua_rock_sources.sh" \
   "$DEST/scripts/"

(
  cd "$DEST"
  find . -type f ! -name RELEASE_MANIFEST | sed 's#^\./##' | sort > RELEASE_MANIFEST
)

printf '%s\n' "$DEST"
