#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sh "$ROOT/scripts/version.sh")
DIST=${LIBMDF_DIST_DIR:-$ROOT/dist}
ARCHIVE="$DIST/libmdf-$VERSION.tar.gz"
WORK="$ROOT/build/package-source-smoke"

test -f "$ARCHIVE"
rm -rf "$WORK"
mkdir -p "$WORK"
tar -C "$WORK" -xzf "$ARCHIVE"
roots=$(find "$WORK" -mindepth 1 -maxdepth 1 -type d | wc -l)
test "$roots" -eq 1
SRC=$(find "$WORK" -mindepth 1 -maxdepth 1 -type d)
test "$(basename "$SRC")" = "libmdf-$VERSION"
test "$(cat "$SRC/VERSION")" = "$VERSION"

(
  cd "$SRC"
  find . -type f | sed 's#^\./##' | sort > "$WORK/manifest.actual"
  sort RELEASE_MANIFEST > "$WORK/manifest.expected"
)
cmp "$WORK/manifest.actual" "$WORK/manifest.expected"

cmake -S "$SRC" -B "$WORK/build" -G Ninja \
  -DLIBMDF_BUILD_TESTS=ON \
  -DLIBMDF_BUILD_EXAMPLES=ON \
  -DLIBMDF_BUILD_FUZZERS=OFF \
  -DLIBMDF_INSTALL=ON >/dev/null
cmake --build "$WORK/build" >/dev/null
ctest --test-dir "$WORK/build" --output-on-failure
