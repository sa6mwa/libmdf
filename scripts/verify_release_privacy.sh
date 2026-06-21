#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sh "$ROOT/scripts/version.sh")
MANIFEST="$ROOT/dist/libmdf-$VERSION-CHECKSUMS"
TMP="$ROOT/build/package-verify/privacy"
HITS="$ROOT/build/package-verify/privacy-hits.txt"

test -f "$MANIFEST"
rm -rf "$TMP"
mkdir -p "$TMP"
: > "$HITS"

artifact_list="$TMP/artifacts.txt"
awk '{print $2}' "$MANIFEST" | sed 's#^\*##' > "$artifact_list"

release_like="$TMP/release-like.txt"
find "$ROOT/dist" -maxdepth 1 -type f \( \
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

extract_artifact() {
  artifact=$1
  dest=$2
  mkdir -p "$dest"
  case "$artifact" in
    *.tar.gz)
      tar -C "$dest" -xzf "$artifact"
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
    find "$root" -type f \( -name '*.tar.gz' -o -name '*.src.rock' \) |
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
        *.src.rock)
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
  name=$1
  if command -v "$name" >/dev/null 2>&1; then
    command -v "$name"
    return 0
  fi
  for cache in $(find "$ROOT/build" -name CMakeCache.txt -type f 2>/dev/null); do
    for key in CMAKE_C_COMPILER CMAKE_STRIP CMAKE_OBJDUMP; do
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
  tool=$(find "$ROOT/build" -type f -name "$name" -perm -111 2>/dev/null | sed -n '1p')
  if [ -n "$tool" ]; then
    printf '%s\n' "$tool"
    return 0
  fi
  return 1
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
  otool=$(find_darwin_tool otool || true)
  if [ -z "$otool" ]; then
    return 0
  fi
  deps="$TMP/macho-deps.txt"
  if ! "$otool" -L "$file" > "$deps" 2>/dev/null; then
    return 0
  fi
  if grep -F -e "$ROOT" -e "${HOME:-/dev/null}" "$deps" >/dev/null 2>&1; then
    printf '%s\n' "$file has local Mach-O dependency path" >> "$HITS"
    return 1
  fi
  if grep -E '^[[:space:]]*/' "$deps" | grep -vE '^[[:space:]]*/usr/lib/|^[[:space:]]*/System/Library/' >/dev/null 2>&1; then
    printf '%s\n' "$file has non-system absolute Mach-O dependency path" >> "$HITS"
    return 1
  fi
  if "$otool" -D "$file" > "$deps" 2>/dev/null && grep -E '^/' "$deps" | grep -vE '^/usr/lib/|^/System/Library/' >/dev/null 2>&1; then
    printf '%s\n' "$file has non-system absolute Mach-O install name" >> "$HITS"
    return 1
  fi
  return 0
}

scan_runtime_paths() {
  root=$1
  failed=0
  find "$root" -type f |
  while IFS= read -r file; do
    desc=$(file -b "$file" 2>/dev/null || true)
    case "$desc" in
      *ELF*)
        scan_elf_rpath "$file" || failed=1
        ;;
      *Mach-O*)
        scan_macho_paths "$file" || failed=1
        ;;
    esac
    if [ "$failed" -ne 0 ]; then
      exit 1
    fi
  done
}

while IFS= read -r artifact_name; do
  artifact="$ROOT/dist/$artifact_name"
  test -f "$artifact"
  artifact_tmp="$TMP/$artifact_name.extract"
  extract_artifact "$artifact" "$artifact_tmp"
  expand_nested_archives "$artifact_tmp"
  scan_local_paths "$artifact_tmp" || true
  scan_runtime_paths "$artifact_tmp" || true
done < "$artifact_list"

if [ -s "$HITS" ]; then
  printf '%s\n' "release privacy gate failed; local paths or non-relocatable runtime paths leaked:" >&2
  sort -u "$HITS" >&2
  exit 1
fi
