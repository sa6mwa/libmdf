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
expect_fail repo-path 'README.md'

reset_dist
TOOLCHAIN_ROOT=$("$ROOT/scripts/cpkt-toolchains.sh" discover x86_64-linux-gnu | sed -n 's/^root=//p')
TOOLCHAIN_CC=$("$ROOT/scripts/cpkt-toolchains.sh" discover x86_64-linux-gnu | sed -n 's/^cc=//p')
test -n "$TOOLCHAIN_ROOT"
test -x "$TOOLCHAIN_CC"
mkdir -p "$PKG/bin"
printf '%s\n' 'int main(void) { return 0; }' > "$BASE/bootlin-leak.c"
"$TOOLCHAIN_CC" "$BASE/bootlin-leak.c" "-Wl,-rpath,$TOOLCHAIN_ROOT" -o "$PKG/bin/bootlin-leak"
tar -C "$BASE/pkg" -czf "$DIST/$ARTIFACT" "libmdf-$VERSION-x86_64-linux-gnu"
(cd "$DIST" && sha256sum "$ARTIFACT" > "$MANIFEST")
expect_fail bootlin-path 'contains pinned Bootlin toolchain path'

reset_dist
mkdir -p "$PKG/bin"
printf '%s\n' 'int main(void) { return 0; }' > "$BASE/runtime.c"
"$TOOLCHAIN_CC" "$BASE/runtime.c" '-Wl,-rpath,$ORIGIN/../lib' -o "$PKG/bin/runtime"
tar -C "$BASE/pkg" -czf "$DIST/$ARTIFACT" "libmdf-$VERSION-x86_64-linux-gnu"
(cd "$DIST" && sha256sum "$ARTIFACT" > "$MANIFEST")
LIBMDF_DIST_DIR="$DIST" "$ROOT/scripts/verify_release_privacy.sh"
if LIBMDF_READELF="$BASE/missing-readelf" LIBMDF_DIST_DIR="$DIST" "$ROOT/scripts/verify_release_privacy.sh" >"$BASE/missing-tool.out" 2>"$BASE/missing-tool.err"; then
  printf '%s\n' 'privacy gate accepted a missing target inspection tool' >&2
  exit 1
fi
grep -q 'no available target-correct readelf' "$BASE/missing-tool.err"
"$TOOLCHAIN_CC" "$BASE/runtime.c" '-Wl,-rpath,$ORIGIN/../lib:/opt/undeclared-runtime' -o "$PKG/bin/runtime"
tar -C "$BASE/pkg" -czf "$DIST/$ARTIFACT" "libmdf-$VERSION-x86_64-linux-gnu"
(cd "$DIST" && sha256sum "$ARTIFACT" > "$MANIFEST")
expect_fail mixed-origin-path 'non-\$ORIGIN RPATH/RUNPATH'

# Required classification must fail closed rather than skip this ELF's bad path.
mkdir -p "$BASE/classifier-tools"
cat > "$BASE/classifier-tools/file" <<'EOF'
#!/bin/sh
exit "${TEST_FILE_EXIT:-17}"
EOF
chmod +x "$BASE/classifier-tools/file"
for classifier_exit in 17 127; do
  if TEST_FILE_EXIT="$classifier_exit" PATH="$BASE/classifier-tools:$PATH" \
      LIBMDF_DIST_DIR="$DIST" "$ROOT/scripts/verify_release_privacy.sh" \
      > "$BASE/classifier-$classifier_exit.out" 2> "$BASE/classifier-$classifier_exit.err"; then
    printf '%s\n' 'privacy gate accepted failed payload classification' >&2
    exit 1
  fi
  grep -q 'could not classify payload' "$BASE/classifier-$classifier_exit.err"
done
