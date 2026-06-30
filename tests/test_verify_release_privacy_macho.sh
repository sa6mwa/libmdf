#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sh "$ROOT/scripts/version.sh")
BASE="$ROOT/build/test-verify-release-privacy-macho"
DIST="$BASE/dist"
TARGET=x86_64-apple-darwin
HOST=x86_64-apple-darwin25
PKG="$BASE/pkg/libmdf-$VERSION-$TARGET"
TOOLS="$BASE/tools"
ARTIFACT="libmdf-$VERSION-$TARGET.tar.gz"

rm -rf "$BASE"
mkdir -p "$PKG/lib" "$DIST" "$TOOLS"
printf 'fake macho\n' > "$PKG/lib/libmdf.dylib"
tar -C "$BASE/pkg" -czf "$DIST/$ARTIFACT" "libmdf-$VERSION-$TARGET"
(cd "$DIST" && sha256sum "$ARTIFACT" > "libmdf-$VERSION-CHECKSUMS")

cat > "$TOOLS/file" <<'EOF'
#!/usr/bin/env sh
printf '%s\n' 'Mach-O 64-bit dynamically linked shared library arm64'
EOF
chmod +x "$TOOLS/file"

if OSXCROSS_ROOT="$BASE/no-osxcross" PATH="$TOOLS:$PATH" LIBMDF_DIST_DIR="$DIST" "$ROOT/scripts/verify_release_privacy.sh" >/dev/null 2>"$BASE/no-otool.err"; then
  printf '%s\n' 'privacy gate accepted Mach-O artifact without target otool' >&2
  exit 1
fi
grep -q 'no target-correct otool' "$BASE/no-otool.err"

cat > "$TOOLS/$HOST-otool" <<'EOF'
#!/usr/bin/env sh
case "$1" in
  -L)
    printf '%s\n' "$2:"
    printf '%s\n' '	/lib/libbad.dylib (compatibility version 1.0.0, current version 1.0.0)'
    ;;
  -D)
    printf '%s\n' "$2:"
    printf '%s\n' '@rpath/libmdf.0.dylib'
    ;;
  -l)
    printf '%s\n' 'Load command 0'
    printf '%s\n' '          cmd LC_RPATH'
    printf '%s\n' '      cmdsize 48'
    printf '%s\n' '         path @loader_path/../lib (offset 12)'
    ;;
  *)
    exit 2
    ;;
esac
EOF
chmod +x "$TOOLS/$HOST-otool"

if OSXCROSS_ROOT="$BASE/no-osxcross" PATH="$TOOLS:$PATH" LIBMDF_DIST_DIR="$DIST" "$ROOT/scripts/verify_release_privacy.sh" >/dev/null 2>"$BASE/bad-dep.err"; then
  printf '%s\n' 'privacy gate accepted non-system absolute Mach-O dependency' >&2
  exit 1
fi
grep -q 'non-system absolute Mach-O dependency path' "$BASE/bad-dep.err"

cat > "$TOOLS/$HOST-otool" <<'EOF'
#!/usr/bin/env sh
case "$1" in
  -L)
    printf '%s\n' "$2:"
    printf '%s\n' '	@rpath/libmdf.0.dylib (compatibility version 0.0.0, current version 0.2.0)'
    printf '%s\n' '	/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1356.0.0)'
    ;;
  -D)
    printf '%s\n' "$2:"
    printf '%s\n' '@rpath/libmdf.0.dylib'
    ;;
  -l)
    printf '%s\n' 'Load command 0'
    printf '%s\n' '          cmd LC_RPATH'
    printf '%s\n' '      cmdsize 48'
    printf '%s\n' '         path /tmp/libmdf (offset 12)'
    ;;
  *)
    exit 2
    ;;
esac
EOF
chmod +x "$TOOLS/$HOST-otool"

if OSXCROSS_ROOT="$BASE/no-osxcross" PATH="$TOOLS:$PATH" LIBMDF_DIST_DIR="$DIST" "$ROOT/scripts/verify_release_privacy.sh" >/dev/null 2>"$BASE/bad-rpath.err"; then
  printf '%s\n' 'privacy gate accepted local Mach-O LC_RPATH' >&2
  exit 1
fi
grep -q 'Mach-O LC_RPATH' "$BASE/bad-rpath.err"
