#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sh "$ROOT/scripts/version.sh")
DIST=${LIBMDF_DIST_DIR:-$ROOT/dist}
MANIFEST="$DIST/libmdf-$VERSION-CHECKSUMS"
TMP="$ROOT/build/package-verify/privacy"
HITS="$ROOT/build/package-verify/privacy-hits.txt"

test -f "$MANIFEST"
rm -rf "$TMP"
mkdir -p "$TMP"
: > "$HITS"

active_manifests="$TMP/active-manifests.txt"
find "$DIST" -maxdepth 1 -type f -name 'libmdf-*-CHECKSUMS' -exec basename {} \; | sort > "$active_manifests"
if [ "$(wc -l < "$active_manifests")" -ne 1 ] || ! grep -Fx "$(basename "$MANIFEST")" "$active_manifests" >/dev/null; then
  printf '%s\n' "exactly one active checksum manifest is required for $VERSION" >&2
  sed 's/^/manifest: /' "$active_manifests" >&2
  exit 1
fi

artifact_list="$TMP/artifacts.txt"
awk '{print $2}' "$MANIFEST" | sed 's#^\*##' > "$artifact_list"

if grep -a -F -e "$ROOT" -e "file://$ROOT" "$MANIFEST" >/dev/null 2>&1 ||
   { [ -n "${HOME:-}" ] && grep -a -F -e "$HOME" -e "file://$HOME" "$MANIFEST" >/dev/null 2>&1; }; then
  printf '%s\n' "$MANIFEST contains local paths" >&2
  exit 1
fi

release_like="$TMP/release-like.txt"
find "$DIST" -maxdepth 1 -type f \( \
  -name "libmdf-$VERSION.tar.gz" -o \
  -name "libmdf-$VERSION-*.tar.gz" -o \
  -name "cmdf-$VERSION-*.tar.gz" -o \
  -name "libmdf-lua-$VERSION.tar.gz" -o \
  -name "libmdf-$VERSION-1.rockspec" -o \
  -name "libmdf-$VERSION-1.src.rock" \
\) -exec basename {} \; | sort > "$release_like"
sort "$artifact_list" > "$artifact_list.sorted"
if ! cmp -s "$release_like" "$artifact_list.sorted"; then
  printf '%s\n' "release-looking artifacts are not exactly represented by $MANIFEST" >&2
  comm -23 "$release_like" "$artifact_list.sorted" | sed 's/^/unlisted: /' >&2
  comm -13 "$release_like" "$artifact_list.sorted" | sed 's/^/missing: /' >&2
  exit 1
fi

stale_release_like="$TMP/stale-release-like.txt"
find "$DIST" -maxdepth 1 -type f \( \
  -name 'libmdf-*.tar.gz' -o \
  -name 'cmdf-*.tar.gz' -o \
  -name 'libmdf-lua-*.tar.gz' -o \
  -name 'libmdf-*-1.rockspec' -o \
  -name 'libmdf-*-1.src.rock' \
\) ! \( \
  -name "libmdf-$VERSION.tar.gz" -o \
  -name "libmdf-$VERSION-*.tar.gz" -o \
  -name "cmdf-$VERSION-*.tar.gz" -o \
  -name "libmdf-lua-$VERSION.tar.gz" -o \
  -name "libmdf-$VERSION-1.rockspec" -o \
  -name "libmdf-$VERSION-1.src.rock" \
\) -exec basename {} \; | sort > "$stale_release_like"
if [ -s "$stale_release_like" ]; then
  printf '%s\n' "stale release-looking artifacts for another version remain under $DIST" >&2
  sed 's/^/stale: /' "$stale_release_like" >&2
  exit 1
fi

extract_artifact() {
  artifact=$1
  dest=$2
  mkdir -p "$dest"
  case "$artifact" in
    *.tar.gz)
      tar -C "$dest" -xzf "$artifact"
      ;;
    *.tgz)
      tar -C "$dest" -xzf "$artifact"
      ;;
    *.tar.xz)
      tar -C "$dest" -xJf "$artifact"
      ;;
    *.zip|*.rock)
      unzip -q "$artifact" -d "$dest"
      ;;
    *.src.rock)
      unzip -q "$artifact" -d "$dest"
      ;;
    *.rockspec)
      cp "$artifact" "$dest/"
      ;;
    *)
      printf '%s\n' "unsupported release artifact for privacy scan: $artifact" >&2
      exit 1
      ;;
  esac
}

expand_nested_archives() {
  root=$1
  pass=0
  while [ "$pass" -lt 4 ]; do
    find "$root" -type f \( -name '*.tar.gz' -o -name '*.tgz' -o -name '*.tar.xz' -o -name '*.zip' -o -name '*.rock' -o -name '*.src.rock' \) |
    while IFS= read -r nested; do
      marker="$nested.privacy-expanded"
      if [ -e "$marker" ]; then
        continue
      fi
      dest="$nested.contents"
      mkdir -p "$dest"
      case "$nested" in
        *.tar.gz)
          tar -C "$dest" -xzf "$nested"
          ;;
        *.tgz)
          tar -C "$dest" -xzf "$nested"
          ;;
        *.tar.xz)
          tar -C "$dest" -xJf "$nested"
          ;;
        *.zip|*.rock|*.src.rock)
          unzip -q "$nested" -d "$dest"
          ;;
      esac
      : > "$marker"
      : > "$root/.privacy-expanded"
    done
    if [ ! -e "$root/.privacy-expanded" ]; then
      break
    fi
    rm -f "$root/.privacy-expanded"
    pass=$((pass + 1))
  done
}

scan_local_paths() {
  root=$1
  patterns="$TMP/local-path-patterns.txt"
  : > "$patterns"
  printf '%s\n' "$ROOT" >> "$patterns"
  if [ -n "${HOME:-}" ]; then
    printf '%s\n' "$HOME" >> "$patterns"
  fi
  printf '%s\n' "file://$ROOT" >> "$patterns"
  if [ -n "${HOME:-}" ]; then
    printf '%s\n' "file://$HOME" >> "$patterns"
  fi
  if grep -R -a -l -f "$patterns" "$root" >> "$HITS"; then
    return 1
  fi
  return 0
}

find_darwin_tool() {
  target=$1
  name=$1
  shift
  name=$1
  "$ROOT/scripts/discover_target_tools.sh" --root "$ROOT" --target "$target" --preset "$target-release" "$name"
}

scan_elf_rpath() {
  file=$1
  if ! command -v readelf >/dev/null 2>&1; then
    return 0
  fi
  dyn="$TMP/elf-dynamic.txt"
  if ! readelf -d "$file" > "$dyn" 2>/dev/null; then
    return 0
  fi
  if ! grep -E 'RPATH|RUNPATH' "$dyn" >/dev/null 2>&1; then
    return 0
  fi
  if grep -E 'RPATH|RUNPATH' "$dyn" | grep -v '\$ORIGIN' >/dev/null 2>&1; then
    printf '%s\n' "$file has non-\$ORIGIN RPATH/RUNPATH" >> "$HITS"
    return 1
  fi
  if grep -E 'RPATH|RUNPATH' "$dyn" | grep -F -e "$ROOT" -e "${HOME:-/dev/null}" >/dev/null 2>&1; then
    printf '%s\n' "$file has local RPATH/RUNPATH" >> "$HITS"
    return 1
  fi
  return 0
}

scan_macho_paths() {
  file=$1
  target=$2
  otool=$(find_darwin_tool "$target" otool || true)
  if [ -z "$otool" ]; then
    printf '%s\n' "$file is Mach-O but no target-correct otool was found" >> "$HITS"
    return 1
  fi
  deps="$TMP/macho-deps.txt"
  if ! "$otool" -L "$file" > "$deps" 2>/dev/null; then
    printf '%s\n' "$otool could not inspect $file" >> "$HITS"
    return 1
  fi
  dep_entries="$TMP/macho-deps.entries.txt"
  sed '1d' "$deps" > "$dep_entries"
  if grep -F -e "$ROOT" -e "${HOME:-/dev/null}" "$dep_entries" >/dev/null 2>&1; then
    printf '%s\n' "$file has local Mach-O dependency path" >> "$HITS"
    return 1
  fi
  if grep -E '^[[:space:]]*/' "$dep_entries" | grep -vE '^[[:space:]]*/usr/lib/|^[[:space:]]*/System/Library/' >/dev/null 2>&1; then
    printf '%s\n' "$file has non-system absolute Mach-O dependency path" >> "$HITS"
    return 1
  fi
  if "$otool" -D "$file" > "$deps" 2>/dev/null; then
    sed '1d' "$deps" > "$dep_entries"
  else
    : > "$dep_entries"
  fi
  if grep -E '^/' "$dep_entries" | grep -vE '^/usr/lib/|^/System/Library/' >/dev/null 2>&1; then
    printf '%s\n' "$file has non-system absolute Mach-O install name" >> "$HITS"
    return 1
  fi
  load_commands="$TMP/macho-load-commands.txt"
  rpaths="$TMP/macho-rpaths.txt"
  if ! "$otool" -l "$file" > "$load_commands" 2>/dev/null; then
    printf '%s\n' "$otool could not inspect load commands for $file" >> "$HITS"
    return 1
  fi
  awk '
    $1 == "cmd" && $2 == "LC_RPATH" { in_rpath = 1; next }
    $1 == "cmd" { in_rpath = 0 }
    in_rpath && $1 == "path" { print $2 }
  ' "$load_commands" > "$rpaths"
  if grep -F -e "$ROOT" -e "${HOME:-/dev/null}" "$rpaths" >/dev/null 2>&1; then
    printf '%s\n' "$file has local Mach-O LC_RPATH" >> "$HITS"
    return 1
  fi
  if grep -E '^/' "$rpaths" | grep -vE '^/usr/lib/|^/System/Library/' >/dev/null 2>&1; then
    printf '%s\n' "$file has non-system absolute Mach-O LC_RPATH" >> "$HITS"
    return 1
  fi
  return 0
}

scan_runtime_paths() {
  root=$1
  target=$2
  failed=0
  find "$root" -type f |
  while IFS= read -r file; do
    desc=$(file -b "$file" 2>/dev/null || true)
    case "$desc" in
      *ELF*)
        scan_elf_rpath "$file" || failed=1
        ;;
      *Mach-O*)
        scan_macho_paths "$file" "$target" || failed=1
        ;;
    esac
    if [ "$failed" -ne 0 ]; then
      exit 1
    fi
  done
}

while IFS= read -r artifact_name; do
  artifact="$DIST/$artifact_name"
  test -f "$artifact"
  artifact_tmp="$TMP/$artifact_name.extract"
  target=
  case "$artifact_name" in
    libmdf-"$VERSION"-*.tar.gz)
      target=${artifact_name#libmdf-$VERSION-}
      target=${target%.tar.gz}
      ;;
    cmdf-"$VERSION"-*.tar.gz)
      target=${artifact_name#cmdf-$VERSION-}
      target=${target%.tar.gz}
      ;;
  esac
  extract_artifact "$artifact" "$artifact_tmp"
  expand_nested_archives "$artifact_tmp"
  scan_local_paths "$artifact_tmp" || true
  scan_runtime_paths "$artifact_tmp" "$target" || true
done < "$artifact_list"

if [ -s "$HITS" ]; then
  printf '%s\n' "release privacy gate failed; local paths or non-relocatable runtime paths leaked:" >&2
  sort -u "$HITS" >&2
  exit 1
fi
