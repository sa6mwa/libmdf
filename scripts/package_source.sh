#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sh "$ROOT/scripts/version.sh")
NAME="libmdf-$VERSION"
DIST="$ROOT/dist"
BUILD="$ROOT/build/package-source"
STAGE="$BUILD/$NAME"
OUT="$DIST/$NAME.tar.gz"

rm -rf "$BUILD"
mkdir -p "$STAGE" "$DIST"

if git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
  git -C "$ROOT" archive --format=tar --prefix="$NAME/" HEAD | tar -C "$BUILD" -xf -
  {
    git -C "$ROOT" ls-files
    printf '%s\n' 'RELEASE_MANIFEST' 'VERSION'
  } | sort -u > "$STAGE/RELEASE_MANIFEST"
else
  tar \
    --exclude='./build' \
    --exclude='./dist' \
    --exclude='./.git' \
    -C "$ROOT" -cf - . | tar -C "$STAGE" -xf -
  printf '%s\n' "$VERSION" > "$STAGE/VERSION"
  if [ ! -f "$STAGE/RELEASE_MANIFEST" ]; then
    {
      find "$STAGE" -type f | sed "s#^$STAGE/##"
      printf '%s\n' 'RELEASE_MANIFEST'
    } | sort -u > "$STAGE/RELEASE_MANIFEST"
  fi
fi

printf '%s\n' "$VERSION" > "$STAGE/VERSION"
tar -C "$BUILD" -czf "$OUT" "$NAME"
printf '%s\n' "$OUT"
