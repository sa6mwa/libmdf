#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sh "$ROOT/scripts/version.sh")
MANIFEST="$ROOT/dist/libmdf-$VERSION-CHECKSUMS"

find_darwin_tool() {
  name=$1
  if command -v "$name" >/dev/null 2>&1; then
    command -v "$name"
    return 0
  fi
  for cache in $(find "$ROOT/build" -name CMakeCache.txt -type f 2>/dev/null); do
    for key in CMAKE_C_COMPILER CMAKE_STRIP CMAKE_INSTALL_NAME_TOOL CMAKE_OTOOL; do
      tool=$(sed -n "s/^$key:FILEPATH=//p" "$cache" | sed -n '1p')
      if [ -n "$tool" ]; then
        dir=$(dirname "$tool")
        if [ -x "$dir/$name" ]; then
          printf '%s\n' "$dir/$name"
          return 0
        fi
      fi
    done
  done
  return 1
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
      otool=$(find_darwin_tool otool || true)
      if [ -n "$otool" ] && "$otool" -L "$cmdf" > "$dyn" 2>/dev/null; then
        if grep -q 'libmdf' "$dyn"; then
          printf '%s\n' "$cmdf links project shared libmdf; cmdf must embed libmdf statically" >&2
          exit 1
        fi
      fi
      ;;
  esac
}

test -f "$MANIFEST"
(cd "$ROOT/dist" && sha256sum -c "$(basename "$MANIFEST")")
for artifact in "$ROOT"/dist/libmdf-"$VERSION"-*.tar.gz; do
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
  test -d "$root/include"
  test -d "$root/lib"
  test -f "$root/share/doc/libmdf/LICENSE"
  test ! -e "$root/bin/cmdf"
  test ! -f "$root/share/doc/libmdf/OFL.txt"
  if find "$root" -name '*.inc' -print -quit | grep -q .; then
    printf '%s\n' "$artifact contains .inc files" >&2
    exit 1
  fi
  if find "$root" -name '*hack*' -print -quit | grep -q .; then
    printf '%s\n' "$artifact contains old Hack font files" >&2
    exit 1
  fi
  if find "$root/lib" -type f \( -name '*.a' -o -name '*.so*' \) -exec grep -a -l -e 'JetBrains Mono' -e 'libmdf_html_jetbrains' {} + | grep -q .; then
    printf '%s\n' "$artifact embeds cmdf font data in libmdf" >&2
    exit 1
  fi
done
for artifact in "$ROOT"/dist/cmdf-"$VERSION"-*.tar.gz; do
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
  grep -a -q 'regular_b64 = "' "$root/bin/cmdf.lua"
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
if [ -f "$ROOT/dist/libmdf-$VERSION-1.src.rock" ] || [ -f "$ROOT/dist/libmdf-lua-$VERSION.tar.gz" ]; then
  "$ROOT/scripts/verify_lua_artifacts.sh"
fi
"$ROOT/scripts/verify_release_privacy.sh"
