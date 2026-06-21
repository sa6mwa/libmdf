#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sh "$ROOT/scripts/version.sh")

unset CC CFLAGS CPPFLAGS LDFLAGS

if command -v x86_64-linux-musl-gcc >/dev/null 2>&1; then
  TARGET=x86_64-linux-musl
else
  TARGET=x86_64-linux-gnu
fi

PRESET="$TARGET-release"
CACHE="$ROOT/build/$PRESET/CMakeCache.txt"
ARTIFACT="$ROOT/dist/libmdf-$VERSION-$TARGET.tar.gz"
ROOT_NAME="libmdf-$VERSION-$TARGET"

cmake --preset "$PRESET" \
  -DLIBMDF_INSTALL=OFF \
  -DLIBMDF_INSTALL_BINARY=OFF \
  -DLIBMDF_BUILD_STATIC=OFF \
  -DLIBMDF_BUILD_SHARED=ON \
  -DLIBMDF_BUILD_BINARY=OFF \
  -DLIBMDF_BUILD_TESTS=OFF \
  -DLIBMDF_BUILD_EXAMPLES=OFF \
  -DLIBMDF_BUILD_FUZZERS=OFF \
  -DLIBMDF_CMDF_STATIC_RUNTIME=ON >/dev/null

grep -q '^LIBMDF_INSTALL:BOOL=OFF$' "$CACHE"
grep -q '^LIBMDF_BUILD_STATIC:BOOL=OFF$' "$CACHE"
grep -q '^LIBMDF_BUILD_SHARED:BOOL=ON$' "$CACHE"
grep -q '^LIBMDF_BUILD_BINARY:BOOL=OFF$' "$CACHE"

LIBMDF_TARGETS="$TARGET" "$ROOT/scripts/package.sh" >/dev/null

test -f "$ARTIFACT"
test -f "$ROOT/dist/cmdf-$VERSION-$TARGET.tar.gz"
grep -q '^LIBMDF_INSTALL:BOOL=ON$' "$CACHE"
grep -q '^LIBMDF_BUILD_STATIC:BOOL=ON$' "$CACHE"
grep -q '^LIBMDF_BUILD_SHARED:BOOL=ON$' "$CACHE"
grep -q '^LIBMDF_BUILD_BINARY:BOOL=ON$' "$CACHE"
grep -q '^LIBMDF_BUILD_TESTS:BOOL=OFF$' "$CACHE"
grep -q '^LIBMDF_BUILD_EXAMPLES:BOOL=OFF$' "$CACHE"
grep -q '^LIBMDF_BUILD_FUZZERS:BOOL=OFF$' "$CACHE"
tar -tzf "$ARTIFACT" | grep -q "^$ROOT_NAME/include/libmdf/mdf.h$"
tar -tzf "$ROOT/dist/cmdf-$VERSION-$TARGET.tar.gz" | grep -q "^cmdf-$VERSION-$TARGET/bin/cmdf$"
