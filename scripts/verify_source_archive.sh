#!/usr/bin/env bash
# Verify source delivery against the inputs used by the canonical target builds.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
version=$("$root/scripts/version.sh")
dist=${LIBMDF_DIST_DIR:-$root/dist}
name="libmdf-$version"
work="$root/build/package-verify/source"
"$root/scripts/require_build_path.sh" "$work"
rm -rf -- "$work"
mkdir -p "$work"
tar -C "$work" -xzf "$dist/$name.tar.gz"
source="$work/$name"
[[ $(find "$work" -mindepth 1 -maxdepth 1 | wc -l) == 1 && -d $source ]]
[[ -z $(find "$source" ! -type d ! -type f -print) ]] || { printf 'Source archive must contain regular files\n' >&2; exit 1; }
test "$(cat "$source/VERSION")" = "$version" || { printf 'Source archive version mismatch\n' >&2; exit 1; }
owner=$(git -C "$root" rev-parse --show-toplevel 2>/dev/null || true)
if [[ -n $owner && $(cd "$owner" && pwd -P) == "$root" ]]; then
  { git -C "$root" ls-tree -r --name-only HEAD; printf '%s\n' VERSION RELEASE_MANIFEST; } | sort -u > "$work/expected"
else
  sort "$root/RELEASE_MANIFEST" > "$work/expected"
fi
(cd "$source" && find . -type f | sed 's#^./##' | sort) > "$work/actual"
sort "$source/RELEASE_MANIFEST" > "$work/manifest"
cmp "$work/expected" "$work/actual"
cmp "$work/expected" "$work/manifest"
while IFS= read -r path; do
  case "$path" in
    ''|/*|..|../*|*/../*|*/..) printf 'Invalid source archive path: %s\n' "$path" >&2; exit 1;;
    VERSION|RELEASE_MANIFEST) continue;;
  esac
  cmp "$root/$path" "$source/$path"
  if [[ -x $root/$path ]]; then
    test -x "$source/$path"
  else
    test ! -x "$source/$path"
  fi || { printf 'Source archive executable mode mismatch: %s\n' "$path" >&2; exit 1; }
done < "$work/expected"
