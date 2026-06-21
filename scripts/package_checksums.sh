#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sh "$ROOT/scripts/version.sh")
MANIFEST="$ROOT/dist/libmdf-$VERSION-CHECKSUMS"
TMP="$ROOT/build/checksums"

mkdir -p "$ROOT/dist" "$TMP"
LIST="$TMP/artifacts.txt"
: > "$LIST"

for artifact in \
  "$ROOT"/dist/libmdf-"$VERSION".tar.gz \
  "$ROOT"/dist/libmdf-"$VERSION"-*.tar.gz \
  "$ROOT"/dist/cmdf-"$VERSION"-*.tar.gz \
  "$ROOT"/dist/libmdf-lua-"$VERSION".tar.gz \
  "$ROOT"/dist/libmdf-"$VERSION"-1.rockspec \
  "$ROOT"/dist/libmdf-"$VERSION"-1.src.rock
do
  if [ -f "$artifact" ]; then
    basename "$artifact" >> "$LIST"
  fi
done

sort -u "$LIST" > "$LIST.sorted"
if [ ! -s "$LIST.sorted" ]; then
  printf '%s\n' "no release artifacts found in dist for $VERSION" >&2
  exit 1
fi

(
  cd "$ROOT/dist"
  xargs sha256sum < "$LIST.sorted" > "$MANIFEST"
)
