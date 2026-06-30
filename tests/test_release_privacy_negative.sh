#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sh "$ROOT/scripts/version.sh")
BASE="$ROOT/build/test-release-privacy-negative"
DIST="$BASE/dist"
PKG="$BASE/pkg/libmdf-$VERSION-x86_64-linux-gnu"
ARTIFACT="libmdf-$VERSION-x86_64-linux-gnu.tar.gz"
MANIFEST="libmdf-$VERSION-CHECKSUMS"

reset_dist() {
  rm -rf "$BASE"
  mkdir -p "$PKG/share/doc/libmdf" "$DIST"
  printf '%s\n' 'license' > "$PKG/share/doc/libmdf/LICENSE"
  tar -C "$BASE/pkg" -czf "$DIST/$ARTIFACT" "libmdf-$VERSION-x86_64-linux-gnu"
  (cd "$DIST" && sha256sum "$ARTIFACT" > "$MANIFEST")
}

expect_fail() {
  label=$1
  pattern=$2
  if LIBMDF_DIST_DIR="$DIST" "$ROOT/scripts/verify_release_privacy.sh" >"$BASE/$label.out" 2>"$BASE/$label.err"; then
    printf '%s\n' "privacy gate unexpectedly accepted $label" >&2
    exit 1
  fi
  grep -q "$pattern" "$BASE/$label.err"
}

reset_dist
LIBMDF_DIST_DIR="$DIST" "$ROOT/scripts/verify_release_privacy.sh"

printf '%s\n' "0000000000000000000000000000000000000000000000000000000000000000  file://$HOME/leak.tar.gz" >> "$DIST/$MANIFEST"
expect_fail manifest-local 'contains local paths'

reset_dist
cp "$DIST/$ARTIFACT" "$DIST/libmdf-9.9.9-x86_64-linux-gnu.tar.gz"
expect_fail stale-artifact 'stale release-looking artifacts'

reset_dist
cp "$DIST/$ARTIFACT" "$DIST/cmdf-$VERSION-x86_64-linux-gnu.tar.gz"
expect_fail unlisted-artifact 'not exactly represented'

reset_dist
printf '%s\n' "$ROOT" > "$PKG/share/doc/libmdf/README.md"
tar -C "$BASE/pkg" -czf "$DIST/$ARTIFACT" "libmdf-$VERSION-x86_64-linux-gnu"
(cd "$DIST" && sha256sum "$ARTIFACT" > "$MANIFEST")
expect_fail repo-path 'local paths or non-relocatable runtime paths leaked'
