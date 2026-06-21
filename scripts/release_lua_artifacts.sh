#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sh "$ROOT/scripts/version.sh")
BUILD="$ROOT/build/lua-release"
DIST="$ROOT/dist"
NAME="libmdf-lua-$VERSION"
ARCHIVE="$DIST/$NAME.tar.gz"
ROCKSPEC="$DIST/libmdf-$VERSION-1.rockspec"
SRCROCK="$DIST/libmdf-$VERSION-1.src.rock"
SRCROCK_STAGE="$BUILD/srcrock"

mkdir -p "$DIST" "$BUILD"
"$ROOT/scripts/stage_lua_rock_sources.sh" "$VERSION" "$BUILD/stage" >/dev/null
tar -C "$BUILD/stage" -czf "$ARCHIVE" "$NAME"

"$ROOT/scripts/render_release_rockspec.sh" "$ROCKSPEC" "$VERSION" "file://$NAME.tar.gz" "$NAME"

rm -rf "$SRCROCK_STAGE"
mkdir -p "$SRCROCK_STAGE"
cp "$ROCKSPEC" "$SRCROCK_STAGE/libmdf-$VERSION-1.rockspec"
cp "$ARCHIVE" "$SRCROCK_STAGE/$NAME.tar.gz"
rm -f "$SRCROCK"
(
  cd "$SRCROCK_STAGE"
  zip -q "$SRCROCK" "libmdf-$VERSION-1.rockspec" "$NAME.tar.gz"
)

"$ROOT/scripts/package_checksums.sh"
