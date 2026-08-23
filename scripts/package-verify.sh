#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sh "$ROOT/scripts/version.sh")
DIST=${LIBMDF_DIST_DIR:-$ROOT/dist}
MANIFEST="$DIST/libmdf-$VERSION-CHECKSUMS"

find_darwin_tool() {
  target=$1
  name=$2
  "$ROOT/scripts/discover_target_tools.sh" --root "$ROOT" --target "$target" --preset "$target-release" "$name"
}

verify_cmdf_static() {
  cmdf=$1
  target=$2
  dyn="$tmp/cmdf-dynamic.txt"
  case "$target" in
    *-linux-*)
      if command -v readelf >/dev/null 2>&1; then
        if readelf -l "$cmdf" 2>/dev/null | grep -q 'Requesting program interpreter'; then
          printf '%s\n' "$cmdf is dynamically linked; Linux cmdf release binaries must be static" >&2
          exit 1
        fi
        if readelf -d "$cmdf" > "$dyn" 2>/dev/null; then
          if grep -q '(NEEDED)' "$dyn"; then
            printf '%s\n' "$cmdf has dynamic dependencies; Linux cmdf release binaries must be static" >&2
            exit 1
          fi
        fi
      fi
      ;;
    *-apple-darwin)
      otool=$(find_darwin_tool "$target" otool || true)
      if [ -z "$otool" ]; then
        printf '%s\n' "$cmdf is a Darwin release binary but no target-correct otool was found" >&2
        exit 1
      fi
      if "$otool" -L "$cmdf" > "$dyn" 2>/dev/null; then
        if sed '1d' "$dyn" | grep -q 'libmdf'; then
          printf '%s\n' "$cmdf links project shared libmdf; cmdf must embed libmdf statically" >&2
          exit 1
        fi
      else
        printf '%s\n' "$otool could not inspect $cmdf" >&2
        exit 1
      fi
      ;;
  esac
}

test -f "$MANIFEST"
(cd "$DIST" && sha256sum -c "$(basename "$MANIFEST")")
for artifact in "$DIST"/libmdf-"$VERSION"-*.tar.gz; do
  case "$artifact" in
    *CHECKSUMS*) continue ;;
  esac
  tmp="$ROOT/build/package-verify"
  rm -rf "$tmp"
  mkdir -p "$tmp"
  tar -C "$tmp" -xzf "$artifact"
  roots=$(find "$tmp" -mindepth 1 -maxdepth 1 -type d | wc -l)
  test "$roots" -eq 1
  root=$(find "$tmp" -mindepth 1 -maxdepth 1 -type d)
  test "$(basename "$artifact" .tar.gz)" = "$(basename "$root")"
  target=$(basename "$artifact" .tar.gz)
  target=${target#libmdf-$VERSION-}
  test -d "$root/include"
  test -d "$root/lib"
  test -f "$root/lib/pkgconfig/libmdf.pc"
  test -f "$root/lib/cmake/libmdf/libmdfConfig.cmake"
  test -f "$root/lib/cmake/libmdf/libmdfConfigVersion.cmake"
  test -f "$root/share/libmdf/package-metadata.txt"
  test -f "$root/share/doc/libmdf/LICENSE"
  test ! -e "$root/bin/cmdf"
  test -f "$root/share/doc/libmdf/OFL.txt"
  grep -q "^Name: libmdf$" "$root/lib/pkgconfig/libmdf.pc"
  grep -q "^Version: $VERSION$" "$root/lib/pkgconfig/libmdf.pc"
  grep -q '^prefix=${pcfiledir}/../..' "$root/lib/pkgconfig/libmdf.pc"
  grep -q "^name=libmdf$" "$root/share/libmdf/package-metadata.txt"
  grep -q "^version=$VERSION$" "$root/share/libmdf/package-metadata.txt"
  grep -q "^target_id=$target$" "$root/share/libmdf/package-metadata.txt"
  case "$target" in
    *-linux-*)
      grep -q '^target_os=linux$' "$root/share/libmdf/package-metadata.txt"
      ;;
    *-apple-darwin)
      grep -q '^target_os=darwin$' "$root/share/libmdf/package-metadata.txt"
      ;;
  esac
  if grep -R -a -F -e "$ROOT" -e "${HOME:-/dev/null}" "$root/lib/pkgconfig" "$root/lib/cmake" "$root/share/libmdf" >/dev/null 2>&1; then
    printf '%s\n' "$artifact contains local paths in package metadata" >&2
    exit 1
  fi
  if find "$root" -name '*.inc' -print -quit | grep -q .; then
    printf '%s\n' "$artifact contains .inc files" >&2
    exit 1
  fi
  if find "$root" -name '*hack*' -print -quit | grep -q .; then
    printf '%s\n' "$artifact contains old Hack font files" >&2
    exit 1
  fi
  if ! find "$root/lib" -type f \( -name '*.a' -o -name '*.so*' \) -exec grep -a -l -e 'JetBrains Mono' -e 'libmdf_html_jetbrains' {} + | grep -q .; then
    printf '%s\n' "$artifact does not embed JetBrains Mono font data in libmdf" >&2
    exit 1
  fi
done
for artifact in "$DIST"/cmdf-"$VERSION"-*.tar.gz; do
  tmp="$ROOT/build/package-verify"
  rm -rf "$tmp"
  mkdir -p "$tmp"
  tar -C "$tmp" -xzf "$artifact"
  roots=$(find "$tmp" -mindepth 1 -maxdepth 1 -type d | wc -l)
  test "$roots" -eq 1
  root=$(find "$tmp" -mindepth 1 -maxdepth 1 -type d)
  test "$(basename "$artifact" .tar.gz)" = "$(basename "$root")"
  target=$(basename "$artifact" .tar.gz)
  target=${target#cmdf-$VERSION-}
  test -x "$root/bin/cmdf"
  verify_cmdf_static "$root/bin/cmdf" "$target"
  test -x "$root/bin/cmdf.lua"
  test ! -d "$root/include"
  test ! -d "$root/lib"
  test -f "$root/share/doc/cmdf/LICENSE"
  test -f "$root/share/doc/cmdf/README.md"
  test -f "$root/share/doc/cmdf/OFL.txt"
  grep -a -q 'JetBrains Mono' "$root/bin/cmdf"
  grep -a -q 'JetBrains Mono' "$root/bin/cmdf.lua"
  grep -a -q 'require("libmdf")' "$root/bin/cmdf.lua"
  if grep -a -q 'regular_b64 = "' "$root/bin/cmdf.lua"; then
    printf '%s\n' "$artifact cmdf.lua embeds font data instead of using libmdf" >&2
    exit 1
  fi
  if find "$root" \( -name '*.rockspec' -o -name 'libmdf_core.c' \) -print -quit | grep -q .; then
    printf '%s\n' "$artifact contains LuaRocks source or rockspec files" >&2
    exit 1
  fi
  if find "$root" -name '*.inc' -print -quit | grep -q .; then
    printf '%s\n' "$artifact contains .inc files" >&2
    exit 1
  fi
  if find "$root" -name '*hack*' -print -quit | grep -q .; then
    printf '%s\n' "$artifact contains old Hack font files" >&2
    exit 1
  fi
done
if [ -f "$DIST/libmdf-$VERSION-1.src.rock" ] || [ -f "$DIST/libmdf-lua-$VERSION.tar.gz" ]; then
  LIBMDF_DIST_DIR="$DIST" "$ROOT/scripts/verify_lua_artifacts.sh"
fi
LIBMDF_DIST_DIR="$DIST" "$ROOT/scripts/verify_release_privacy.sh"
